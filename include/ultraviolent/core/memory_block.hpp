#pragma once

#include <cstddef>
#include <cstdint>
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

    // Code-frame tracking for execution engines (IR.adoc "Keeping blocks valid"). An engine
    // marks the 4 KiB frames it decoded code from; a write that may change a marked frame
    // advances the frame's generation and clears its mark. Writers of guest-visible memory
    // report here (AddressSpace::write, AddressSpace::writable_memory_bytes). Writes through
    // bytes() happen only when loading images and snapshots, which invalidate decoded code
    // globally instead.
    static constexpr std::size_t frame_size = 0x1000;
    void mark_code(std::size_t offset) {
        std::uint8_t& mark = code_marks_[offset / frame_size];
        if (mark == 0) {
            mark = 1;
            ++marked_frames_;
        }
    }
    [[nodiscard]] bool holds_code(std::size_t offset) const {
        return code_marks_[offset / frame_size] != 0;
    }
    // The generation of the frame holding `offset`; stable storage for the block's lifetime.
    [[nodiscard]] const std::uint64_t* code_generation(std::size_t offset) const {
        return &code_generations_[offset / frame_size];
    }
    // A write of `size` bytes at `offset` may follow: advances and unmarks the marked frames it
    // touches. Returns whether it touched one.
    bool note_write(std::size_t offset, std::size_t size) {
        if (marked_frames_ == 0 || size == 0) {
            return false;
        }
        bool touched = false;
        for (std::size_t frame = offset / frame_size; frame <= (offset + size - 1) / frame_size;
             ++frame) {
            if (code_marks_[frame] != 0) {
                code_marks_[frame] = 0;
                ++code_generations_[frame];
                --marked_frames_;
                touched = true;
            }
        }
        return touched;
    }
    [[nodiscard]] std::size_t marked_frames() const {
        return marked_frames_;
    }

  private:
    std::vector<std::byte> bytes_;
    std::vector<std::uint8_t> code_marks_ = std::vector<std::uint8_t>(frames());
    std::vector<std::uint64_t> code_generations_ = std::vector<std::uint64_t>(frames());
    std::size_t marked_frames_{};

    [[nodiscard]] std::size_t frames() const {
        return (bytes_.size() + frame_size - 1) / frame_size;
    }
};

} // namespace ultraviolent
