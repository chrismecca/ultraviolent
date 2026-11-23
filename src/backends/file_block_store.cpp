#include <ultraviolent/backends/file_block_store.hpp>

#include <ultraviolent/backends/cow_block_store.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ultraviolent::backends {

bool read_fully(int fd, std::uint64_t offset, std::span<std::byte> bytes) {
    while (!bytes.empty()) {
        const ssize_t n = ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(n));
        offset += static_cast<std::uint64_t>(n);
    }
    return true;
}

bool write_fully(int fd, std::uint64_t offset, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const ssize_t n = ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(n));
        offset += static_cast<std::uint64_t>(n);
    }
    return true;
}

std::unique_ptr<FileBlockStore> FileBlockStore::open(const std::string& path, bool writable) {
    const int fd = ::open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        ::close(fd);
        return nullptr;
    }
    return std::unique_ptr<FileBlockStore>{
        new FileBlockStore{fd, static_cast<std::uint64_t>(status.st_size), writable}};
}

FileBlockStore::~FileBlockStore() {
    ::close(fd_);
}

bool FileBlockStore::read(std::uint64_t offset, std::span<std::byte> bytes) {
    if (offset > size_ || bytes.size() > size_ - offset) {
        return false;
    }
    return read_fully(fd_, offset, bytes);
}

bool FileBlockStore::write(std::uint64_t offset, std::span<const std::byte> bytes) {
    if (!writable_ || offset > size_ || bytes.size() > size_ - offset) {
        return false;
    }
    return write_fully(fd_, offset, bytes);
}

std::expected<std::unique_ptr<BlockStore>, std::string> open_disk_image(const std::string& path,
                                                                        bool writable) {
    if (CowBlockStore::is_overlay(path)) {
        auto overlay = CowBlockStore::open(path, writable);
        if (!overlay) {
            return std::unexpected{overlay.error()};
        }
        return std::unique_ptr<BlockStore>{std::move(*overlay)};
    }
    auto file = FileBlockStore::open(path, writable);
    if (!file) {
        return std::unexpected{std::string{"cannot open "} + path + ": " + std::strerror(errno)};
    }
    return std::unique_ptr<BlockStore>{std::move(file)};
}

} // namespace ultraviolent::backends
