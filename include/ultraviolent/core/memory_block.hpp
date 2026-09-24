#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace ultraviolent {

// Host storage behind guest-visible memory such as RAM or ROM contents. A block has no
// address of its own; an AddressSpace maps ranges of it, possibly more than once (aliases).
//
// Contents start zeroed so that runs are deterministic. Real DRAM contents at power-on are
// undefined, so guest software cannot legitimately depend on this choice.
//
// Not copyable or movable: mappings refer to the block's storage for its whole lifetime.
class MemoryBlock {
  public:
    explicit MemoryBlock(std::size_t size) : bytes_(size) {}
    MemoryBlock(const MemoryBlock&) = delete;
    MemoryBlock& operator=(const MemoryBlock&) = delete;

    [[nodiscard]] std::size_t size() const {
        return bytes_.size();
    }

    // Direct access for loading ROM images and for host-side inspection.
    [[nodiscard]] std::span<std::byte> bytes() {
        return bytes_;
    }
    [[nodiscard]] std::span<const std::byte> bytes() const {
        return bytes_;
    }

  private:
    std::vector<std::byte> bytes_;
};

} // namespace ultraviolent
