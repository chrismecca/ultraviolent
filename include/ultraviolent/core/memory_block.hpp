#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ultraviolent {

// Host storage behind guest-visible memory such as RAM or ROM contents. A block has no
// address of its own; an AddressSpace maps ranges of it, possibly more than once (aliases).
//
// Contents start zeroed so that runs are deterministic. Real DRAM contents at power-on are
// undefined, so guest software cannot legitimately depend on this choice. The storage is
// fresh anonymous pages from the host, already zeroed: a machine's RAM costs host memory and
// time only as it is touched. (Not calloc: an allocator may zero the memory itself, as
// ThreadSanitizer's does, which commits every page of a 1 GB directory window at once.)
//
// Not copyable or movable: mappings refer to the block's storage for its whole lifetime.
class MemoryBlock {
  public:
    explicit MemoryBlock(std::size_t size) : bytes_{allocate(size)}, size_{size} {}
    MemoryBlock(const MemoryBlock&) = delete;
    MemoryBlock& operator=(const MemoryBlock&) = delete;

    [[nodiscard]] std::size_t size() const {
        return size_;
    }

    // Direct access for loading ROM images and for host-side inspection.
    [[nodiscard]] std::span<std::byte> bytes() {
        return {bytes_.get(), size_};
    }
    [[nodiscard]] std::span<const std::byte> bytes() const {
        return {bytes_.get(), size_};
    }

    // Code-frame tracking for execution engines (IR.adoc "Keeping blocks valid"). An engine
    // marks the 4 KiB frames it decoded code from; a write that may change a marked frame
    // advances the frame's generation and clears its mark. Writers of guest-visible memory
    // report here (AddressSpace::write, AddressSpace::writable_memory_bytes). Writes through
    // bytes() happen only when loading images and snapshots, which invalidate decoded code
    // globally instead.
    //
    // CPUs on several host threads mark and write concurrently (SMP.adoc "S4"): marks and
    // generations are atomics. Relaxed, except where a mark is set or cleared, which decides
    // who counts it; the CPUs' own protocol orders the rest (Cpu::claim_code_frame).
    static constexpr std::size_t frame_size = 0x1000;
    // Returns whether the frame was not marked before.
    bool mark_code(std::size_t offset) {
        std::atomic<std::uint8_t>& mark = code_marks_[offset / frame_size];
        if (mark.load(std::memory_order_relaxed) != 0 || mark.exchange(1) != 0) {
            return false;
        }
        marked_frames_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    [[nodiscard]] bool holds_code(std::size_t offset) const {
        return code_marks_[offset / frame_size].load(std::memory_order_relaxed) != 0;
    }
    // The generation of the frame holding `offset`; stable storage for the block's lifetime.
    [[nodiscard]] const std::atomic<std::uint64_t>* code_generation(std::size_t offset) const {
        return &code_generations_[offset / frame_size];
    }
    // A write of `size` bytes at `offset` may follow: advances and unmarks the marked frames it
    // touches. Returns whether it touched one.
    bool note_write(std::size_t offset, std::size_t size) {
        if (marked_frames_.load(std::memory_order_relaxed) == 0 || size == 0) {
            return false;
        }
        bool touched = false;
        for (std::size_t frame = offset / frame_size; frame <= (offset + size - 1) / frame_size;
             ++frame) {
            std::atomic<std::uint8_t>& mark = code_marks_[frame];
            if (mark.load(std::memory_order_relaxed) != 0 && mark.exchange(0) != 0) {
                code_generations_[frame].fetch_add(1, std::memory_order_relaxed);
                marked_frames_.fetch_sub(1, std::memory_order_relaxed);
                touched = true;
            }
        }
        return touched;
    }
    [[nodiscard]] std::size_t marked_frames() const {
        return marked_frames_.load(std::memory_order_relaxed);
    }

  private:
    struct Free {
        std::size_t size;
        void operator()(std::byte* bytes) const;
    };
    static std::unique_ptr<std::byte[], Free> allocate(std::size_t size);

    std::unique_ptr<std::byte[], Free> bytes_;
    std::size_t size_;
    std::vector<std::atomic<std::uint8_t>> code_marks_ =
        std::vector<std::atomic<std::uint8_t>>(frames());
    std::vector<std::atomic<std::uint64_t>> code_generations_ =
        std::vector<std::atomic<std::uint64_t>>(frames());
    std::atomic<std::size_t> marked_frames_{};

    [[nodiscard]] std::size_t frames() const {
        return (size_ + frame_size - 1) / frame_size;
    }
};

} // namespace ultraviolent
