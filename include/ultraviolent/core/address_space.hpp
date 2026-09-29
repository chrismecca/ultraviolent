#pragma once

#include <ultraviolent/core/address.hpp>
#include <ultraviolent/core/byte_order.hpp>
#include <ultraviolent/core/memory_block.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace ultraviolent {

// Width of a single naturally aligned access.
enum class AccessWidth : std::uint8_t {
    bits8 = 1,
    bits16 = 2,
    bits32 = 4,
    bits64 = 8,
};

constexpr std::size_t byte_count(AccessWidth width) {
    return std::to_underlying(width);
}

// Why an access did not complete. What the guest observes as a result (bus error, ignored
// write, ...) is decided by whoever issued the access, not by the address space.
enum class AccessFault : std::uint8_t {
    // No mapping decodes the address.
    unmapped,
    // The address is not a multiple of the access width.
    misaligned,
    // Write to memory mapped read-only.
    read_only,
    // The target decodes the address but does not implement this access.
    unsupported,
};

// A memory-mapped register window. Offsets are relative to the start of the window, so a
// device does not know where the machine placed it.
//
// Values follow the owning AddressSpace's byte-lane convention: a read or write of width W
// at offset A carries the integer formed by the W bytes at [A, A + W) in that space's byte
// order. For a big-endian space, a 32-bit access at offset 0 of a 64-bit register therefore
// carries the register's upper half.
class MmioTarget {
  public:
    virtual std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                                AccessWidth width) = 0;
    virtual std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                        std::uint64_t value) = 0;

  protected:
    ~MmioTarget() = default;
};

enum class MemoryAccess : std::uint8_t {
    read_write,
    read_only,
};

// Why a mapping was rejected. These are machine-construction errors, not guest behavior.
enum class MapError : std::uint8_t {
    // Empty, or wraps past the top of the address domain.
    invalid_range,
    // Base or size is not a multiple of mapping_alignment.
    misaligned,
    // Overlaps an existing mapping.
    overlap,
    // The mapped slice extends past the end of the memory block.
    outside_block,
};

// Decodes CPU physical addresses to RAM, ROM, and MMIO targets (ADR-009: this is the
// CPU-side physical domain, not a universal bus; DMA, PCI, and Xtalk transactions have their
// own paths).
//
// Accesses are 1, 2, 4, or 8 bytes and naturally aligned. Mappings are 8-byte aligned, so no
// access can straddle two mappings.
class AddressSpace {
  public:
    static constexpr std::uint64_t mapping_alignment = 8;

    explicit AddressSpace(ByteOrder byte_order) : byte_order_{byte_order} {}
    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;

    [[nodiscard]] ByteOrder byte_order() const {
        return byte_order_;
    }

    // Maps `range` onto `block` starting at `block_offset`. The block must outlive the space.
    // For a read-only mapping, `write_target` (if any) receives the writes, with offsets from
    // the range's base: memory a device reads fast but controls writes to, such as flash.
    std::expected<void, MapError> map_memory(PhysicalRange range, MemoryBlock& block,
                                             std::uint64_t block_offset, MemoryAccess access,
                                             MmioTarget* write_target = nullptr);

    // Maps `range` onto a register window. The target must outlive the space.
    std::expected<void, MapError> map_mmio(PhysicalRange range, MmioTarget& target);

    // Replace the mapping of exactly `range` (a mapping made earlier with the same range).
    std::expected<void, MapError> remap_memory(PhysicalRange range, MemoryBlock& block,
                                               std::uint64_t block_offset, MemoryAccess access,
                                               MmioTarget* write_target = nullptr);
    std::expected<void, MapError> remap_mmio(PhysicalRange range, MmioTarget& target);

    std::expected<std::uint64_t, AccessFault> read(PhysicalAddress address, AccessWidth width);

    // The host bytes behind [address, address + size) when one memory mapping covers the
    // whole range, for execution-engine fast paths. Empty for MMIO and unmapped ranges.
    // Valid until generation() changes.
    [[nodiscard]] std::span<const std::byte> memory_bytes(PhysicalAddress address,
                                                          std::uint64_t size) const;
    // The same for writable memory: empty unless one read-write memory mapping covers the
    // range. Writes through it are exactly the bus writes a CPU store would make. Obtaining
    // the span counts as a write for code-frame tracking (MemoryBlock::note_write).
    // Contract (IR.adoc "Mutable memory spans"): write through the span synchronously, during
    // the operation that obtained it; do not keep it across a return to the scheduler, to CPU
    // execution, or to block construction. The CPU's own data page cache is the one retained
    // writable span, and the CPU invalidates it itself.
    [[nodiscard]] std::span<std::byte> writable_memory_bytes(PhysicalAddress address,
                                                             std::uint64_t size);
    // The memory block and offset behind a memory-mapped address, for code-frame tracking.
    struct MemoryFrame {
        MemoryBlock* block;
        std::uint64_t offset;
    };
    [[nodiscard]] std::optional<MemoryFrame> memory_frame(PhysicalAddress address) const;
    // Writes through this space that touched a frame holding decoded code (write and
    // writable_memory_bytes).
    [[nodiscard]] std::uint64_t code_writes() const {
        return code_writes_;
    }
    // Changes whenever a mapping is added or replaced.
    [[nodiscard]] std::uint64_t generation() const {
        return generation_;
    }
    std::expected<void, AccessFault> write(PhysicalAddress address, AccessWidth width,
                                           std::uint64_t value);

  private:
    struct MemoryMapping {
        std::span<std::byte> bytes;
        MemoryAccess access;
        MmioTarget* write_target{};
        MemoryBlock* block{};
        std::uint64_t block_offset{};
    };

    struct Mapping {
        PhysicalRange range;
        std::variant<MemoryMapping, MmioTarget*> target;
    };

    [[nodiscard]] const Mapping* find(PhysicalAddress address) const;
    std::expected<void, MapError> insert(Mapping mapping);
    std::expected<void, MapError> replace(Mapping mapping);

    ByteOrder byte_order_;
    // Sorted by base address; ranges never overlap.
    std::vector<Mapping> mappings_;
    // Index of the mapping that decoded the last access, tried first. Transparent because
    // mappings never overlap. Measured: the binary search dominated AddressSpace::read in
    // IP27 PROM runs (callgrind, 2026-09-24).
    mutable std::size_t last_hit_{};
    std::uint64_t generation_{};
    std::uint64_t code_writes_{};
};

} // namespace ultraviolent
