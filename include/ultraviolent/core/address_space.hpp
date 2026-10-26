#pragma once

#include <ultraviolent/core/address.hpp>
#include <ultraviolent/core/byte_order.hpp>
#include <ultraviolent/core/memory_block.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
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
    // Requesters: the bus masters whose accesses a register window may tell apart, such as
    // the two processors on an IP27 Hub, each with its own connection (ADR-023). Requester 0
    // is the default for masters that are not told apart.
    static constexpr unsigned max_requesters = 2;
    using Ports = std::array<MmioTarget*, max_requesters>;

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
    // Maps `range` onto one register window per requester: `ports[r]` serves requester r.
    std::expected<void, MapError> map_ports(PhysicalRange range, const Ports& ports);

    // Replace the mapping of exactly `range` (a mapping made earlier with the same range).
    std::expected<void, MapError> remap_memory(PhysicalRange range, MemoryBlock& block,
                                               std::uint64_t block_offset, MemoryAccess access,
                                               MmioTarget* write_target = nullptr);
    std::expected<void, MapError> remap_mmio(PhysicalRange range, MmioTarget& target);
    // Removes every mapping, so a machine can rebuild its decoding (an IP27 node whose ID
    // changed). A target's access may do this: the space does not touch the mapping after
    // calling it.
    void clear();
    // Changes between begin_update and end_update become visible together, when shared. They
    // nest.
    void begin_update() {
        ++updating_;
    }
    void end_update();
    // Several threads read the space while one changes it (host-parallel CPUs, SMP.adoc "S4"):
    // each change then publishes a new version of the mappings, and the space keeps every
    // version until it is destroyed, so a reader is never left holding a removed table.
    // Changes still come from one thread at a time. Unshared, readers see the table being
    // changed, as when one thread does everything.
    void set_shared(bool shared);

    std::expected<std::uint64_t, AccessFault> read(PhysicalAddress address, AccessWidth width,
                                                   unsigned requester = 0);

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
    // The device behind every address of [address, address + size), and the offset of
    // `address` in its mapping, when one MMIO mapping covers the whole range: an access there
    // is exactly target->mmio_read or mmio_write at that offset (for caching device accesses
    // by page; valid while generation() is unchanged).
    struct MmioRange {
        MmioTarget* target;
        std::uint64_t offset;
    };
    [[nodiscard]] std::optional<MmioRange> mmio_range(PhysicalAddress address, std::uint64_t size,
                                                     unsigned requester = 0) const;
    // Writes through this space that touched a frame holding decoded code (write and
    // writable_memory_bytes), and through every space sharing its count.
    [[nodiscard]] std::uint64_t code_writes() const {
        return code_writes_->load(std::memory_order_relaxed);
    }
    // Counts code-frame writes together with `other` (which must outlive this space): spaces
    // that map the same memory, so that a CPU on one sees writes made through the other.
    void share_code_writes(AddressSpace& other) {
        code_writes_ = other.code_writes_;
    }
    // Changes whenever a mapping is added or replaced. Once a reader sees a new generation,
    // it finds the mappings that go with it.
    [[nodiscard]] std::uint64_t generation() const {
        return generation_.load(std::memory_order_acquire);
    }
    std::expected<void, AccessFault> write(PhysicalAddress address, AccessWidth width,
                                           std::uint64_t value, unsigned requester = 0);

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
        std::variant<MemoryMapping, Ports> target;
    };

    using Mappings = std::vector<Mapping>;

    [[nodiscard]] const Mapping* find(PhysicalAddress address) const;
    std::expected<void, MapError> insert(Mapping mapping);
    std::expected<void, MapError> replace(Mapping mapping);
    // Makes the changes to mappings_ visible to readers (unless an update is open).
    void publish();

    ByteOrder byte_order_;
    // The mappings as changed, sorted by base address; ranges never overlap.
    Mappings mappings_;
    // What readers decode with: mappings_ itself, or when shared, the latest version.
    std::atomic<const Mappings*> current_{&mappings_};
    std::vector<std::unique_ptr<const Mappings>> versions_;
    bool shared_{};
    unsigned updating_{};
    // Index of the mapping that decoded the last access, tried first. Transparent because
    // mappings never overlap. Measured: the binary search dominated AddressSpace::read in
    // IP27 PROM runs (callgrind, 2026-09-24).
    mutable std::atomic<std::size_t> last_hit_{};
    std::atomic<std::uint64_t> generation_{};
    std::atomic<std::uint64_t> own_code_writes_{};
    std::atomic<std::uint64_t>* code_writes_{&own_code_writes_};
};

} // namespace ultraviolent
