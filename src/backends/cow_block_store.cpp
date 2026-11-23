#include <ultraviolent/backends/cow_block_store.hpp>

#include <ultraviolent/backends/file_block_store.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <unistd.h>

namespace ultraviolent::backends {

namespace {

constexpr std::array<std::byte, 8> magic{std::byte{'U'}, std::byte{'V'}, std::byte{'C'},
                                         std::byte{'O'}, std::byte{'W'}, std::byte{0},
                                         std::byte{0},   std::byte{1}};

template <class T> T get(std::span<const std::byte> bytes, std::size_t offset) {
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(std::to_integer<std::uint64_t>(bytes[offset + i]) << (8 * i));
    }
    return value;
}

template <class T> void put(std::span<std::byte> bytes, std::size_t offset, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes[offset + i] = static_cast<std::byte>(static_cast<std::uint64_t>(value) >> (8 * i));
    }
}

std::string error_text(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

// The base path resolved from the overlay's directory, unless absolute.
std::string resolve(const std::string& overlay, const std::string& base) {
    const std::filesystem::path path{base};
    if (path.is_absolute()) {
        return base;
    }
    return (std::filesystem::path{overlay}.parent_path() / path).string();
}

} // namespace

bool CowBlockStore::is_overlay(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    std::array<std::byte, 8> start{};
    const bool read = read_fully(fd, 0, start);
    ::close(fd);
    return read && start == magic;
}

std::expected<void, std::string> CowBlockStore::create(const std::string& path,
                                                       const std::string& base) {
    if (base.size() > header_size - 40) {
        return std::unexpected{"the base path is too long"};
    }
    auto opened = open_disk_image(resolve(path, base), false);
    if (!opened) {
        return std::unexpected{"base image: " + opened.error()};
    }
    const std::uint64_t size = (*opened)->size();
    const std::uint64_t clusters = (size + cluster_size - 1) / cluster_size;
    const std::uint64_t bitmap_bytes = (clusters + 7) / 8;
    const std::uint64_t first =
        (header_size + bitmap_bytes + cluster_size - 1) / cluster_size * cluster_size;

    std::vector<std::byte> header(header_size);
    std::ranges::copy(magic, header.begin());
    put<std::uint64_t>(header, 8, size);
    put<std::uint32_t>(header, 16, cluster_size);
    put<std::uint32_t>(header, 20, static_cast<std::uint32_t>(base.size()));
    put<std::uint64_t>(header, 24, header_size);
    put<std::uint64_t>(header, 32, first);
    std::memcpy(header.data() + 40, base.data(), base.size());

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        return std::unexpected{error_text("cannot create " + path)};
    }
    const std::vector<std::byte> bitmap(bitmap_bytes);
    const bool written = write_fully(fd, 0, header) && write_fully(fd, header_size, bitmap) &&
                         ::ftruncate(fd, static_cast<off_t>(first)) == 0 && ::fsync(fd) == 0;
    ::close(fd);
    if (!written) {
        return std::unexpected{error_text("cannot write " + path)};
    }
    return {};
}

std::expected<std::unique_ptr<CowBlockStore>, std::string>
CowBlockStore::open(const std::string& path, bool writable) {
    const int fd = ::open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected{error_text("cannot open " + path)};
    }
    const auto fail = [&](std::string why) {
        ::close(fd);
        return std::unexpected{path + ": " + why};
    };
    std::vector<std::byte> header(header_size);
    if (!read_fully(fd, 0, header) || !std::ranges::equal(std::span{header}.first(8), magic)) {
        return fail("not a copy-on-write overlay");
    }
    const auto size = get<std::uint64_t>(header, 8);
    const auto cluster = get<std::uint32_t>(header, 16);
    const auto base_length = get<std::uint32_t>(header, 20);
    const auto bitmap_offset = get<std::uint64_t>(header, 24);
    const auto first = get<std::uint64_t>(header, 32);
    const std::uint64_t clusters = (size + cluster_size - 1) / cluster_size;
    const std::uint64_t bitmap_bytes = (clusters + 7) / 8;
    if (cluster != cluster_size || base_length > header_size - 40 || bitmap_offset < header_size ||
        first < bitmap_offset + bitmap_bytes) {
        return fail("unsupported or damaged overlay header");
    }
    std::string base_path(base_length, '\0');
    std::memcpy(base_path.data(), header.data() + 40, base_length);
    std::string resolved = resolve(path, base_path);
    auto base = open_disk_image(resolved, false);
    if (!base) {
        return fail("base image: " + base.error());
    }
    if ((*base)->size() != size) {
        return fail(std::format("the base image {} is {} bytes, not the {} it had when the "
                                "overlay was made",
                                resolved, (*base)->size(), size));
    }
    std::vector<std::uint8_t> bitmap(bitmap_bytes);
    if (!read_fully(fd, bitmap_offset, std::as_writable_bytes(std::span{bitmap}))) {
        return fail("cannot read the overlay's bitmap");
    }
    return std::unique_ptr<CowBlockStore>{
        new CowBlockStore{fd, writable, size, bitmap_offset, first, std::move(base_path),
                          std::move(resolved), std::move(*base), std::move(bitmap)}};
}

CowBlockStore::CowBlockStore(int fd, bool writable, std::uint64_t size, std::uint64_t bitmap_offset,
                             std::uint64_t first_cluster, std::string base_path,
                             std::string resolved_base, std::unique_ptr<BlockStore> base,
                             std::vector<std::uint8_t> bitmap)
    : fd_{fd}, writable_{writable}, size_{size}, bitmap_offset_{bitmap_offset},
      first_cluster_{first_cluster}, base_path_{std::move(base_path)},
      resolved_base_{std::move(resolved_base)}, base_{std::move(base)}, bitmap_{std::move(bitmap)} {
}

CowBlockStore::~CowBlockStore() {
    ::close(fd_);
}

std::uint64_t CowBlockStore::cluster_length(std::uint64_t cluster) const {
    return std::min<std::uint64_t>(cluster_size, size_ - cluster * cluster_size);
}

std::uint64_t CowBlockStore::clusters_held() const {
    std::uint64_t count = 0;
    for (const std::uint8_t byte : bitmap_) {
        count += static_cast<std::uint64_t>(std::popcount(byte));
    }
    return count;
}

bool CowBlockStore::read(std::uint64_t offset, std::span<std::byte> bytes) {
    if (offset > size_ || bytes.size() > size_ - offset) {
        return false;
    }
    while (!bytes.empty()) {
        const std::uint64_t cluster = offset / cluster_size;
        const std::uint64_t within = offset % cluster_size;
        const std::size_t length =
            static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), cluster_size - within));
        const auto part = bytes.first(length);
        const bool read = held(cluster) ? read_fully(fd_, cluster_offset(cluster) + within, part)
                                        : base_->read(offset, part);
        if (!read) {
            return false;
        }
        bytes = bytes.subspan(length);
        offset += length;
    }
    return true;
}

bool CowBlockStore::write(std::uint64_t offset, std::span<const std::byte> bytes) {
    if (!writable_ || offset > size_ || bytes.size() > size_ - offset) {
        return false;
    }
    std::vector<std::byte> merged;
    while (!bytes.empty()) {
        const std::uint64_t cluster = offset / cluster_size;
        const std::uint64_t within = offset % cluster_size;
        const std::size_t length =
            static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), cluster_size - within));
        const auto part = bytes.first(length);
        if (held(cluster)) {
            if (!write_fully(fd_, cluster_offset(cluster) + within, part)) {
                return false;
            }
        } else {
            // The cluster's first copy: the base's bytes with the written ones merged, then
            // the bitmap (ORCHESTRATION.adoc: an interrupted write leaves it unclaimed).
            merged.resize(static_cast<std::size_t>(cluster_length(cluster)));
            if (length != merged.size() && !base_->read(cluster * cluster_size, merged)) {
                return false;
            }
            std::ranges::copy(part, merged.begin() + static_cast<std::ptrdiff_t>(within));
            if (!write_fully(fd_, cluster_offset(cluster), merged)) {
                return false;
            }
            bitmap_[cluster / 8] |= static_cast<std::uint8_t>(1u << (cluster % 8));
            const std::byte byte{bitmap_[cluster / 8]};
            if (!write_fully(fd_, bitmap_offset_ + cluster / 8, std::span{&byte, 1})) {
                return false;
            }
        }
        bytes = bytes.subspan(length);
        offset += length;
    }
    return true;
}

} // namespace ultraviolent::backends
