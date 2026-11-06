#include <ultraviolent/core/state_image.hpp>

#include <ultraviolent/core/byte_order.hpp>
#include <ultraviolent/core/invariant.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace ultraviolent {

namespace {

// File layout: magic, then records of (u32 key length, key, u64 value length, value), all
// integers little-endian.
constexpr std::string_view magic = "UVSTATE1";

constexpr std::uint64_t sparse_page = 0x1'0000;

void append_integer(std::vector<std::byte>& out, std::uint64_t value, std::size_t size) {
    const std::size_t offset = out.size();
    out.resize(offset + size);
    store_unsigned(std::span{out}.subspan(offset, size), value, ByteOrder::little);
}

} // namespace

void StateImage::put_bytes(std::string key, std::span<const std::byte> bytes) {
    // Two components saving under one key would lose one silently.
    const bool inserted =
        fields_.try_emplace(std::move(key), Bytes(bytes.begin(), bytes.end())).second;
    invariant(inserted, "a snapshot key was saved twice");
}

std::optional<std::span<const std::byte>> StateImage::get_bytes(std::string_view key) const {
    const auto found = fields_.find(key);
    if (found == fields_.end()) {
        return std::nullopt;
    }
    return std::span<const std::byte>{found->second};
}

void StateImage::put_image(std::string_view prefix, const StateImage& other) {
    for (const auto& [key, value] : other.fields_) {
        put_bytes(std::string{prefix} + key, value);
    }
}

StateImage StateImage::sub_image(std::string_view prefix) const {
    StateImage image;
    for (auto it = fields_.lower_bound(prefix); it != fields_.end() && it->first.starts_with(prefix);
         ++it) {
        image.fields_.emplace(it->first.substr(prefix.size()), it->second);
    }
    return image;
}

void StateImage::put_sparse(const std::string& key, std::span<const std::byte> bytes) {
    std::vector<std::uint64_t> pages;
    std::vector<std::byte> contents;
    for (std::uint64_t page = 0; page * sparse_page < bytes.size(); ++page) {
        const auto chunk =
            bytes.subspan(page * sparse_page,
                          std::min<std::size_t>(sparse_page, bytes.size() - page * sparse_page));
        if (std::ranges::any_of(chunk, [](std::byte b) { return b != std::byte{0}; })) {
            pages.push_back(page);
            contents.insert(contents.end(), chunk.begin(), chunk.end());
            contents.resize(pages.size() * sparse_page);
        }
    }
    put(key + ".size", std::uint64_t{bytes.size()});
    put_bytes(key + ".pages", std::as_bytes(std::span{pages}));
    put_bytes(key + ".contents", contents);
}

bool StateImage::get_sparse(const std::string& key, std::span<std::byte> bytes) const {
    // Snapshots from before `.size` was recorded are accepted when their pages fit.
    std::uint64_t size = bytes.size();
    const auto pages = get_bytes(key + ".pages");
    const auto contents = get_bytes(key + ".contents");
    if ((get_bytes(key + ".size") && (!get(key + ".size", size) || size != bytes.size())) ||
        !pages || !contents || pages->size() % sizeof(std::uint64_t) != 0 ||
        contents->size() != pages->size() / sizeof(std::uint64_t) * sparse_page) {
        return false;
    }
    const std::size_t count = pages->size() / sizeof(std::uint64_t);
    std::vector<std::uint64_t> indices(count);
    if (count != 0) { // memcpy's pointers must be valid even for no bytes
        std::memcpy(indices.data(), pages->data(), pages->size());
    }
    if (std::ranges::any_of(indices,
                            [&](std::uint64_t page) { return page * sparse_page >= size; })) {
        return false;
    }
    // Pages not saved become zero; those already zero are left untouched, so that a fresh
    // machine's memory is not written twice (MemoryBlock).
    std::vector<bool> saved((size + sparse_page - 1) / sparse_page);
    for (const std::uint64_t page : indices) {
        saved[page] = true;
    }
    for (std::uint64_t page = 0; page < saved.size(); ++page) {
        const auto chunk = bytes.subspan(
            page * sparse_page, std::min<std::size_t>(sparse_page, size - page * sparse_page));
        if (!saved[page] &&
            std::ranges::any_of(chunk, [](std::byte b) { return b != std::byte{0}; })) {
            std::ranges::fill(chunk, std::byte{0});
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t offset = indices[i] * sparse_page;
        const std::size_t length = std::min<std::uint64_t>(sparse_page, size - offset);
        std::memcpy(bytes.data() + offset, contents->data() + i * sparse_page, length);
    }
    return true;
}

std::vector<std::byte> StateImage::serialize() const {
    std::vector<std::byte> out;
    for (const char c : magic) {
        out.push_back(static_cast<std::byte>(c));
    }
    for (const auto& [key, value] : fields_) {
        append_integer(out, key.size(), 4);
        for (const char c : key) {
            out.push_back(static_cast<std::byte>(c));
        }
        append_integer(out, value.size(), 8);
        out.insert(out.end(), value.begin(), value.end());
    }
    return out;
}

std::optional<StateImage> StateImage::deserialize(std::span<const std::byte> data) {
    std::size_t offset = 0;
    return deserialize(data.size(), [&](std::span<std::byte> out) {
        std::memcpy(out.data(), data.data() + offset, out.size());
        offset += out.size();
        return true;
    });
}

std::optional<StateImage>
StateImage::deserialize(std::uint64_t size, const std::function<bool(std::span<std::byte>)>& read) {
    std::uint64_t remaining = size;
    // Reads the next out.size() bytes, which must be within `size`.
    const auto take = [&](std::span<std::byte> out) {
        if (remaining < out.size()) {
            return false;
        }
        remaining -= out.size();
        return out.empty() || read(out);
    };
    std::array<std::byte, magic.size()> header{};
    if (!take(header) || !std::ranges::equal(header, magic, {}, {},
                                             [](char c) { return static_cast<std::byte>(c); })) {
        return std::nullopt;
    }
    StateImage image;
    while (remaining != 0) {
        std::array<std::byte, 4> key_size{};
        if (!take(key_size)) {
            return std::nullopt;
        }
        const std::uint64_t key_length = load_unsigned(key_size, ByteOrder::little);
        if (remaining < key_length) {
            return std::nullopt;
        }
        std::string key(key_length, '\0');
        std::array<std::byte, 8> value_size{};
        if (!take(std::as_writable_bytes(std::span{key})) || !take(value_size)) {
            return std::nullopt;
        }
        const std::uint64_t length = load_unsigned(value_size, ByteOrder::little);
        if (remaining < length || image.fields_.contains(key)) {
            return std::nullopt; // truncated, or malformed: a key twice
        }
        Bytes value(length);
        if (!take(value)) {
            return std::nullopt;
        }
        image.fields_.emplace(std::move(key), std::move(value));
    }
    return image;
}

} // namespace ultraviolent
