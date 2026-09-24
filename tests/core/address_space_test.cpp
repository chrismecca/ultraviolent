#include "support/test.hpp"

#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/memory_block.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
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

} // namespace
