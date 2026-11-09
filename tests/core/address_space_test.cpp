#include "support/test.hpp"

#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/memory_block.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace ultraviolent;

constexpr PhysicalAddress ram_base{0x0};
constexpr PhysicalAddress rom_base{0x1000'0000};
constexpr PhysicalAddress mmio_base{0x2000'0000};

// A register window that records every access and answers reads with offset + width.
class RecordingTarget final : public MmioTarget {
  public:
    struct Access {
        bool is_write;
        std::uint64_t offset;
        AccessWidth width;
        std::uint64_t value;

        bool operator==(const Access&) const = default;
    };

    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth width) override {
        if (offset >= 0x80) {
            return std::unexpected(AccessFault::unsupported);
        }
        const std::uint64_t value = offset + byte_count(width);
        accesses.push_back({false, offset, width, value});
        return value;
    }

    std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                std::uint64_t value) override {
        accesses.push_back({true, offset, width, value});
        return {};
    }

    std::vector<Access> accesses;
};

// A block's contents start zeroed (MemoryBlock), however large: a machine's RAM is one.
const test::Registration memory_block_starts_zeroed{
    "memory_block.starts_zeroed", [](test::Context& t) {
        for (const std::size_t size :
             {std::size_t{1}, std::size_t{0x1000}, std::size_t{64} << 20}) {
            MemoryBlock block{size};
            t.check_equal(block.size(), size);
            const auto bytes = block.bytes();
            t.check(std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{0}; }));
            bytes[size - 1] = std::byte{1};
            t.check(block.bytes()[size - 1] == std::byte{1}, "writable");
        }
    }};

const test::Registration ram_is_big_endian_lanes{
    "address_space.ram_uses_big_endian_lanes", [](test::Context& t) {
        MemoryBlock ram{0x1000};
        AddressSpace space{ByteOrder::big};
        t.check(space.map_memory({ram_base, 0x1000}, ram, 0, MemoryAccess::read_write).has_value());

        t.check(space.write(ram_base + 8, AccessWidth::bits64, 0x0011223344556677).has_value());
        t.check_equal(space.read(ram_base + 8, AccessWidth::bits32).value(),
                      std::uint64_t{0x00112233});
        t.check_equal(space.read(ram_base + 12, AccessWidth::bits32).value(),
                      std::uint64_t{0x44556677});
        t.check_equal(space.read(ram_base + 9, AccessWidth::bits8).value(), std::uint64_t{0x11});
        t.check(ram.bytes()[8] == std::byte{0x00} && ram.bytes()[15] == std::byte{0x77},
                "bytes land at big-endian positions in the backing block");

        t.check(space.write(ram_base + 14, AccessWidth::bits16, 0xabcd).has_value());
        t.check_equal(space.read(ram_base + 8, AccessWidth::bits64).value(),
                      std::uint64_t{0x001122334455abcd});
    }};

const test::Registration little_endian_space{
    "address_space.little_endian_space", [](test::Context& t) {
        MemoryBlock ram{0x100};
        AddressSpace space{ByteOrder::little};
        t.check(space.map_memory({ram_base, 0x100}, ram, 0, MemoryAccess::read_write).has_value());
        t.check(space.write(ram_base, AccessWidth::bits32, 0x11223344).has_value());
        t.check(ram.bytes()[0] == std::byte{0x44}, "least significant byte first");
        t.check_equal(space.read(ram_base, AccessWidth::bits16).value(), std::uint64_t{0x3344});
    }};

const test::Registration rom_rejects_writes{
    "address_space.rom_rejects_writes", [](test::Context& t) {
        MemoryBlock rom{0x100};
        rom.bytes()[0] = std::byte{0x5a};
        AddressSpace space{ByteOrder::big};
        t.check(space.map_memory({rom_base, 0x100}, rom, 0, MemoryAccess::read_only).has_value());

        t.check_equal(space.read(rom_base, AccessWidth::bits8).value(), std::uint64_t{0x5a});
        const auto result = space.write(rom_base, AccessWidth::bits8, 0);
        t.check(!result && result.error() == AccessFault::read_only);
        t.check(rom.bytes()[0] == std::byte{0x5a}, "read-only contents are unchanged");
    }};

const test::Registration faults{
    "address_space.faults", [](test::Context& t) {
        MemoryBlock ram{0x100};
        AddressSpace space{ByteOrder::big};
        t.check(space.map_memory({ram_base, 0x100}, ram, 0, MemoryAccess::read_write).has_value());

        const auto unmapped = space.read(ram_base + 0x100, AccessWidth::bits8);
        t.check(!unmapped && unmapped.error() == AccessFault::unmapped, "just past the end");
        const auto below = space.read(PhysicalAddress{0x1000'0000}, AccessWidth::bits64);
        t.check(!below && below.error() == AccessFault::unmapped, "nothing mapped there");
        const auto misaligned = space.read(ram_base + 2, AccessWidth::bits32);
        t.check(!misaligned && misaligned.error() == AccessFault::misaligned);
        const auto misaligned_write = space.write(ram_base + 1, AccessWidth::bits16, 0);
        t.check(!misaligned_write && misaligned_write.error() == AccessFault::misaligned);
    }};

const test::Registration aliases_share_storage{
    "address_space.aliases_share_storage", [](test::Context& t) {
        MemoryBlock ram{0x2000};
        AddressSpace space{ByteOrder::big};
        const PhysicalAddress alias_base{0x8000'0000};
        t.check(space.map_memory({ram_base, 0x2000}, ram, 0, MemoryAccess::read_write).has_value());
        // The alias shows the upper half of the block.
        t.check(space.map_memory({alias_base, 0x1000}, ram, 0x1000, MemoryAccess::read_write)
                    .has_value());

        t.check(space.write(ram_base + 0x1010, AccessWidth::bits32, 0xcafef00d).has_value());
        t.check_equal(space.read(alias_base + 0x10, AccessWidth::bits32).value(),
                      std::uint64_t{0xcafef00d});
    }};

const test::Registration mmio_dispatch{
    "address_space.mmio_dispatch", [](test::Context& t) {
        RecordingTarget device;
        AddressSpace space{ByteOrder::big};
        t.check(space.map_mmio({mmio_base, 0x100}, device).has_value());

        t.check_equal(space.read(mmio_base + 0x10, AccessWidth::bits64).value(),
                      std::uint64_t{0x18});
        t.check(space.write(mmio_base + 0x24, AccessWidth::bits32, 0x1234).has_value());
        t.check(device.accesses ==
                    std::vector<RecordingTarget::Access>{{false, 0x10, AccessWidth::bits64, 0x18},
                                                         {true, 0x24, AccessWidth::bits32, 0x1234}},
                "the target sees window-relative offsets and the access width");

        const auto refused = space.read(mmio_base + 0x80, AccessWidth::bits8);
        t.check(!refused && refused.error() == AccessFault::unsupported,
                "target faults pass through unchanged");
    }};

const test::Registration mapping_errors{
    "address_space.mapping_errors", [](test::Context& t) {
        MemoryBlock ram{0x1000};
        RecordingTarget device;
        AddressSpace space{ByteOrder::big};
        t.check(
            space.map_memory({PhysicalAddress{0x1000}, 0x1000}, ram, 0, MemoryAccess::read_write)
                .has_value());

        auto error = [](std::expected<void, MapError> result) {
            return result ? std::optional<MapError>{} : std::optional{result.error()};
        };
        t.check(error(space.map_mmio({PhysicalAddress{0x1ff8}, 0x10}, device)) == MapError::overlap,
                "overlap with the end of an existing mapping");
        t.check(error(space.map_mmio({PhysicalAddress{0x0}, 0x1008}, device)) == MapError::overlap,
                "overlap with the start of an existing mapping");
        t.check(error(space.map_mmio({PhysicalAddress{0x1400}, 0x10}, device)) == MapError::overlap,
                "enclosed by an existing mapping");
        t.check(error(space.map_mmio({PhysicalAddress{0x0}, 0}, device)) ==
                MapError::invalid_range);
        t.check(error(space.map_mmio({PhysicalAddress{0x4004}, 0x10}, device)) ==
                MapError::misaligned);
        t.check(error(space.map_mmio({PhysicalAddress{0x4000}, 0x14}, device)) ==
                MapError::misaligned);
        t.check(error(space.map_memory({PhysicalAddress{0x8000}, 0x1000}, ram, 8,
                                       MemoryAccess::read_write)) == MapError::outside_block);
        t.check(error(space.map_memory({PhysicalAddress{0x8000}, 0x10}, ram, 0x2000,
                                       MemoryAccess::read_write)) == MapError::outside_block);

        // Adjacent mappings on both sides are fine.
        t.check(space.map_mmio({PhysicalAddress{0x0}, 0x1000}, device).has_value());
        t.check(space.map_mmio({PhysicalAddress{0x2000}, 0x1000}, device).has_value());
        t.check_equal(space.read(PhysicalAddress{0x0}, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x8});
        t.check_equal(space.read(PhysicalAddress{0x1ff8}, AccessWidth::bits64).value_or(1),
                      std::uint64_t{0});
        t.check_equal(space.read(PhysicalAddress{0x2000}, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x8});
    }};

class Recorder final : public MmioTarget {
  public:
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth /*width*/) override {
        return 0x100 + offset;
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth /*width*/,
                                                std::uint64_t value) override {
        last_offset = offset;
        last_value = value;
        return {};
    }
    std::uint64_t last_offset = 0;
    std::uint64_t last_value = 0;
};

const test::Registration write_target{
    "address_space.read_only_write_target_and_remap", [](test::Context& t) {
        AddressSpace space{ByteOrder::big};
        MemoryBlock block{0x100};
        block.bytes()[0x10] = std::byte{0x42};
        Recorder device;
        const PhysicalRange range{PhysicalAddress{0x1000}, 0x100};
        t.check(space.map_memory(range, block, 0, MemoryAccess::read_only, &device).has_value());
        // Reads come from memory; writes go to the target, offset from the range base.
        t.check_equal(space.read(PhysicalAddress{0x1010}, AccessWidth::bits8).value_or(0),
                      std::uint64_t{0x42});
        t.check(space.write(PhysicalAddress{0x1018}, AccessWidth::bits64, 7).has_value());
        t.check_equal(device.last_offset, std::uint64_t{0x18});
        t.check_equal(block.bytes()[0x18], std::byte{0});
        // Remapping the same range to the target changes where reads go.
        const auto before = space.generation();
        t.check(space.remap_mmio(range, device).has_value());
        t.check(space.generation() != before, "remap invalidates fast paths");
        t.check_equal(space.read(PhysicalAddress{0x1010}, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x110});
        t.check(!space.remap_mmio({PhysicalAddress{0x1000}, 0x80}, device), "exact range only");
    }};

} // namespace

namespace {

// A machine rebuilding its decoding (an IP27 node whose ID changed) clears the space; the
// generation tells cached translations to go.
const test::Registration clear{"address_space.clear", [](test::Context& t) {
                                   MemoryBlock ram{0x1000};
                                   AddressSpace space{ByteOrder::big};
                                   t.check(space.map_memory({ram_base, 0x1000}, ram, 0,
                                                            MemoryAccess::read_write)
                                               .has_value());
                                   const std::uint64_t before = space.generation();
                                   space.clear();
                                   t.check(space.generation() != before, "generation advances");
                                   t.check(!space.read(ram_base, AccessWidth::bits32),
                                           "nothing is mapped");
                                   t.check(space.map_memory({ram_base, 0x1000}, ram, 0,
                                                            MemoryAccess::read_write)
                                               .has_value(),
                                           "the range can be mapped again");
                               }};

// Two spaces that map the same memory count writes to its code frames together, so a CPU on
// either sees a write made through the other.
const test::Registration shared_code_writes{
    "address_space.shared_code_writes", [](test::Context& t) {
        MemoryBlock ram{0x2000};
        AddressSpace first{ByteOrder::big};
        AddressSpace second{ByteOrder::big};
        second.share_code_writes(first);
        for (AddressSpace* space : {&first, &second}) {
            t.check(space->map_memory({ram_base, 0x2000}, ram, 0, MemoryAccess::read_write)
                        .has_value());
        }
        ram.mark_code(0x1000);
        t.check(second.write(PhysicalAddress{0x1008}, AccessWidth::bits32, 1).has_value());
        t.check_equal(first.code_writes(), std::uint64_t{1});
        t.check_equal(second.code_writes(), std::uint64_t{1});
        t.check(first.write(PhysicalAddress{0x8}, AccessWidth::bits32, 1).has_value());
        t.check_equal(second.code_writes(), std::uint64_t{1}); // not a code frame
    }};

// A write target that changes its read-only block notes the write there; a write that
// reaches a code frame that way counts as a code write, as a store to RAM would.
class BlockWriter final : public MmioTarget {
  public:
    explicit BlockWriter(MemoryBlock& block) : block_{block} {}
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t /*offset*/,
                                                        AccessWidth /*width*/) override {
        return 0;
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth /*width*/,
                                                std::uint64_t value) override {
        // Somewhere else in the block, as a device's aliases may be.
        const std::uint64_t at = (offset + 0x1000) % block_.size();
        block_.note_write(at, 1);
        block_.bytes()[at] = static_cast<std::byte>(value);
        return {};
    }

  private:
    MemoryBlock& block_;
};

const test::Registration write_target_code{
    "address_space.write_target_code_writes", [](test::Context& t) {
        MemoryBlock rom{0x2000};
        BlockWriter device{rom};
        AddressSpace space{ByteOrder::big};
        t.check(space.map_memory({rom_base, 0x2000}, rom, 0, MemoryAccess::read_only, &device)
                    .has_value());
        rom.mark_code(0x1000);
        t.check(space.write(rom_base + 0x8, AccessWidth::bits8, 0x5a).has_value());
        t.check_equal(space.code_writes(), std::uint64_t{1});
        t.check_equal(space.read(rom_base + 0x1008, AccessWidth::bits8).value_or(0),
                      std::uint64_t{0x5a});
        t.check(space.write(rom_base + 0x1008, AccessWidth::bits8, 0x5b).has_value());
        t.check_equal(space.code_writes(), std::uint64_t{1}); // frame 0 holds no code
    }};

// Changes made in one update become visible together: one generation step at the end.
const test::Registration updates{
    "address_space.updates_publish_together", [](test::Context& t) {
        MemoryBlock ram{0x1000};
        AddressSpace space{ByteOrder::big};
        t.check(space.map_memory({ram_base, 0x1000}, ram, 0, MemoryAccess::read_write).has_value());
        for (const bool shared : {false, true}) {
            space.set_shared(shared);
            const std::uint64_t before = space.generation();
            space.begin_update();
            space.clear();
            t.check(
                space.map_memory({ram_base, 0x1000}, ram, 0, MemoryAccess::read_write).has_value());
            t.check_equal(space.generation(), before); // unchanged until the update ends
            space.end_update();
            t.check(space.generation() != before, "the update's end advances it");
            t.check(space.read(ram_base, AccessWidth::bits32).has_value(), "mapped again");
        }
    }};

// Shared (host-parallel CPUs, SMP.adoc "S4"): a thread decoding addresses while another
// replaces the mapping finds the old mapping or the new one, never a torn table.
const test::Registration shared_versions{
    "address_space.shared_versions", [](test::Context& t) {
        MemoryBlock first{0x1000};
        MemoryBlock second{0x1000};
        std::ranges::fill(first.bytes(), std::byte{0x11});
        std::ranges::fill(second.bytes(), std::byte{0x22});
        AddressSpace space{ByteOrder::big};
        t.check(
            space.map_memory({ram_base, 0x1000}, first, 0, MemoryAccess::read_write).has_value());
        space.set_shared(true);
        std::atomic<bool> done{false};
        std::atomic<unsigned> torn{0};
        std::jthread reader{[&] {
            while (!done.load()) {
                const auto value = space.read(ram_base, AccessWidth::bits32);
                if (!value || (*value != 0x1111'1111 && *value != 0x2222'2222)) {
                    ++torn;
                }
            }
        }};
        for (unsigned i = 0; i < 2000; ++i) {
            space.begin_update();
            space.clear();
            (void)space.map_memory({ram_base, 0x1000}, i % 2 == 0 ? second : first, 0,
                                   MemoryAccess::read_write);
            space.end_update();
        }
        done = true;
        reader.join();
        t.check_equal(torn.load(), 0u); // every read decoded a whole mapping
    }};

// Only a frame's first mark reports itself (Cpu::claim_code_frame tells other CPUs then);
// a write to a marked frame clears the mark and advances the frame's generation.
const test::Registration code_marks{
    "memory_block.code_marks", [](test::Context& t) {
        MemoryBlock ram{0x2000};
        const std::uint64_t before = ram.code_generation(0x1000)->load();
        t.check(ram.mark_code(0x1004), "first mark");
        t.check(!ram.mark_code(0x1ff0), "same frame again");
        t.check_equal(ram.marked_frames(), std::size_t{1});
        t.check(!ram.note_write(0x0, 8), "other frame");
        t.check(ram.note_write(0x1ff8, 8), "touches the frame");
        t.check(!ram.holds_code(0x1000), "mark cleared");
        t.check(ram.code_generation(0x1000)->load() != before, "generation advanced");
        t.check(ram.mark_code(0x1000), "a new first mark");
    }};

} // namespace
