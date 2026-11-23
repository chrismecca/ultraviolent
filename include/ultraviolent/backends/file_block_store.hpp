#pragma once

#include <ultraviolent/core/block_store.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace ultraviolent::backends {

// A BlockStore on a host file, opened read-only or read-write. Reads and writes are
// positioned (pread/pwrite), so a transfer is one system call.
class FileBlockStore final : public BlockStore {
  public:
    // Opens `path`; nullptr if it cannot be opened.
    static std::unique_ptr<FileBlockStore> open(const std::string& path, bool writable);
    FileBlockStore(const FileBlockStore&) = delete;
    FileBlockStore& operator=(const FileBlockStore&) = delete;
    ~FileBlockStore() override;

    [[nodiscard]] std::uint64_t size() const override {
        return size_;
    }
    [[nodiscard]] bool writable() const override {
        return writable_;
    }
    bool read(std::uint64_t offset, std::span<std::byte> bytes) override;
    bool write(std::uint64_t offset, std::span<const std::byte> bytes) override;

  private:
    FileBlockStore(int fd, std::uint64_t size, bool writable)
        : fd_{fd}, size_{size}, writable_{writable} {}

    int fd_;
    std::uint64_t size_;
    bool writable_;
};

// A disk image for a guest disk: a plain image file, or a copy-on-write overlay
// (CowBlockStore) recognized by its header. On failure returns the reason.
std::expected<std::unique_ptr<BlockStore>, std::string> open_disk_image(const std::string& path,
                                                                        bool writable);

// Reads or writes all of `bytes` at `offset` of `fd`, retrying short transfers; false on a
// host error or end of file.
bool read_fully(int fd, std::uint64_t offset, std::span<std::byte> bytes);
bool write_fully(int fd, std::uint64_t offset, std::span<const std::byte> bytes);

} // namespace ultraviolent::backends
