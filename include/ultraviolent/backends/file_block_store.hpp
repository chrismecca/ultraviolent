#pragma once

#include <ultraviolent/core/block_store.hpp>

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

namespace ultraviolent::backends {

// A BlockStore on a host file, opened read-only or read-write.
class FileBlockStore final : public BlockStore {
  public:
    // Opens `path`; nullptr if it cannot be opened.
    static std::unique_ptr<FileBlockStore> open(const std::string& path, bool writable);

    [[nodiscard]] std::uint64_t size() const override {
        return size_;
    }
    [[nodiscard]] bool writable() const override {
        return writable_;
    }
    bool read(std::uint64_t offset, std::span<std::byte> bytes) override;
    bool write(std::uint64_t offset, std::span<const std::byte> bytes) override;

  private:
    FileBlockStore(std::fstream stream, std::uint64_t size, bool writable)
        : stream_{std::move(stream)}, size_{size}, writable_{writable} {}

    std::fstream stream_;
    std::uint64_t size_;
    bool writable_;
};

} // namespace ultraviolent::backends
