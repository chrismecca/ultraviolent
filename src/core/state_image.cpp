#include <ultraviolent/core/state_image.hpp>

#include <ultraviolent/core/byte_order.hpp>

#include <algorithm>
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
    fields_.insert_or_assign(std::move(key), std::vector<std::byte>(bytes.begin(), bytes.end()));
}

std::optional<std::span<const std::byte>> StateImage::get_bytes(std::string_view key) const {
    const auto found = fields_.find(key);
    if (found == fields_.end()) {
        return std::nullopt;
    }
    return std::span<const std::byte>{found->second};
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
    std::memcpy(indices.data(), pages->data(), pages->size());
    if (std::ranges::any_of(indices,
                            [&](std::uint64_t page) { return page * sparse_page >= size; })) {
        return false;
    }
    std::ranges::fill(bytes, std::byte{0});
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
    if (data.size() < magic.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < magic.size(); ++i) {
        if (data[i] != static_cast<std::byte>(magic[i])) {
            return std::nullopt;
        }
    }
    StateImage image;
    std::size_t offset = magic.size();
    while (offset < data.size()) {
        if (data.size() - offset < 4) {
            return std::nullopt;
        }
        const std::uint64_t key_size = load_unsigned(data.subspan(offset, 4), ByteOrder::little);
        offset += 4;
        if (data.size() - offset < key_size + 8) {
            return std::nullopt;
        }
        std::string key(key_size, '\0');
        for (std::size_t i = 0; i < key_size; ++i) {
            key[i] = static_cast<char>(data[offset + i]);
        }
        offset += key_size;
        const std::uint64_t value_size = load_unsigned(data.subspan(offset, 8), ByteOrder::little);
        offset += 8;
        if (data.size() - offset < value_size) {
            return std::nullopt;
        }
        image.put_bytes(std::move(key), data.subspan(offset, value_size));
        offset += value_size;
    }
    return image;
}

} // namespace ultraviolent
