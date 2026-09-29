#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ultraviolent {

// Host storage behind a guest block device (a disk or CD-ROM image). The guest-visible
// device decides block sizes and commands; the store only moves bytes at byte offsets.
// Backends implement it (for example a host file).
class BlockStore {
  public:
    virtual ~BlockStore() = default;

    [[nodiscard]] virtual std::uint64_t size() const = 0;
    [[nodiscard]] virtual bool writable() const = 0;
    // Reads or writes `bytes.size()` bytes at `offset`; false if any of them is outside the
    // store or the host fails.
    virtual bool read(std::uint64_t offset, std::span<std::byte> bytes) = 0;
    virtual bool write(std::uint64_t offset, std::span<const std::byte> bytes) = 0;

  protected:
    BlockStore() = default;
    BlockStore(const BlockStore&) = default;
    BlockStore& operator=(const BlockStore&) = default;
};

} // namespace ultraviolent
