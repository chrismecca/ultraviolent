#include "support/test.hpp"

#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/xbow.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace ultraviolent;
using devices::Xbow;

// A widget that answers reads with its offset and records interrupts.
class Probe final : public xtalk::Widget {
  public:
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth /*width*/) override {
        return offset;
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t /*offset*/, AccessWidth /*width*/,
                                                std::uint64_t /*value*/) override {
        return {};
    }
    void xtalk_interrupt(std::uint64_t address, std::uint8_t vector) override {
        interrupts.emplace_back(address, vector);
    }
    bool xtalk_dma_read(std::uint64_t address, std::span<std::byte> bytes) override {
        dma_address = address;
        std::ranges::fill(bytes, std::byte{0x5a});
        return true;
    }
    void xtalk_reset() override {
        ++resets;
    }
    std::vector<std::pair<std::uint64_t, std::uint8_t>> interrupts;
    std::uint64_t dma_address = 0;
    int resets = 0;
};

struct Bench {
    Bench() {
        xbow.attach(9, hub);
    }
    std::uint64_t read(std::uint64_t offset) {
        return xbow.xtalk_read(0, offset, AccessWidth::bits32).value_or(0xdead);
    }
    bool write(std::uint64_t offset, std::uint32_t value) {
        return xbow.xtalk_write(0, offset, AccessWidth::bits32, value).has_value();
    }
    VirtualClock clock;
    Tracer tracer{clock};
    Xbow xbow{tracer};
    Probe hub;
};

const test::Registration dma{"xbow.dma_routing", [](test::Context& t) {
                                 Bench b;
                                 std::array<std::byte, 4> bytes{};
                                 t.check(b.xbow.dma_read(9, 0x1500, bytes));
                                 t.check_equal(b.hub.dma_address, std::uint64_t{0x1500});
                                 t.check(bytes[3] == std::byte{0x5a});
                                 t.check(!b.xbow.dma_read(8, 0x1500, bytes), "empty port");
                                 t.check(!b.xbow.dma_write(9, 0x1500, bytes), "no DMA writes");
                             }};

const test::Registration link_reset{"xbow.link_reset_resets_widget", [](test::Context& t) {
                                        Bench b;
                                        // XB_LINK_RESET for port 9 (link registers 0x140).
                                        t.check(b.write(0x140 + 0x34, 0));
                                        t.check_equal(b.hub.resets, 1);
                                        t.check(b.write(0x100 + 0x34, 0)); // empty port 8
                                        t.check_equal(b.hub.resets, 1);
                                    }};

const test::Registration identity{
    "xbow.identity_and_links", [](test::Context& t) {
        Bench b;
        // Part 0, manufacturer 0, LSB 1; the doubleword form carries it in the low half.
        t.check_equal(b.read(0x04), std::uint64_t{0x4000'0001});
        t.check_equal(b.xbow.xtalk_read(0, 0x0, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x4000'0001});
        // Port 9 (link registers at 0x140) is alive and present; port 8 is empty.
        t.check_equal(b.read(0x154), std::uint64_t{0x8000'0000});
        t.check_equal(b.read(0x17c), std::uint64_t{0x20});
        t.check_equal(b.read(0x114), std::uint64_t{0});
        // Requests for port widgets go to what is attached; others fail.
        t.check_equal(b.xbow.xtalk_read(9, 0x1234, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x1234});
        t.check(!b.xbow.xtalk_read(8, 0, AccessWidth::bits64), "nothing on port 8");
        t.check(!b.xbow.xtalk_read(2, 0, AccessWidth::bits64), "no widget 2");
    }};

const test::Registration registers{
    "xbow.register_storage", [](test::Context& t) {
        Bench b;
        b.write(0x3c, 0x5a5a'5a5a);
        b.write(0x34, 0xa5a5'a5a5);
        t.check_equal(b.read(0x3c), std::uint64_t{0x5a5a'5a5a});
        // INTDEST_UPPER fields, with target bit 3 reading 1 (xbow_sanity's expectation).
        t.check_equal(b.read(0x34), std::uint64_t{0xa50d'a5a5});
        b.write(0x2dc, 2); // link F arbitration register, used by the PROM as a lock
        t.check_equal(b.read(0x2dc), std::uint64_t{2});
    }};

const test::Registration access_errors{
    "xbow.access_error_and_interrupt", [](test::Context& t) {
        Bench b;
        // Writing a read-only register sets REG_ACC_ERR; WID_STAT keeps it, STAT_CLR clears.
        // INTDEST names the Hub, but REG_ACC_IE is clear, so there is no interrupt.
        b.write(0x34, 0x1009'8000);
        b.write(0x1c, 0xdead'beef);
        t.check_equal(b.read(0x0c), std::uint64_t{0x20});
        t.check_equal(b.read(0x0c), std::uint64_t{0x20});
        t.check_equal(b.read(0x54), std::uint64_t{0x20});
        t.check_equal(b.read(0x0c), std::uint64_t{0});
        t.check(b.hub.interrupts.empty(), "interrupt not enabled");
        // With REG_ACC_IE, the error interrupts the INTDEST target: the PROM's values.
        b.write(0x34, 0x1009'8000); // vector 0x10, target 9, address 47:32 = 0x8000
        b.write(0x3c, 0x0180'0090);
        b.write(0x24, 0x20);
        b.write(0x14, 0);
        t.check(b.hub.interrupts == std::vector<std::pair<std::uint64_t, std::uint8_t>>{
                                        {0x8000'0180'0090, std::uint8_t{0x10}}});
        // The target field picks the port.
        Probe other;
        b.xbow.attach(0xa, other);
        b.write(0x34, 0x200a'0000);
        b.write(0x14, 0);
        t.check_equal(other.interrupts.size(), std::size_t{1});
        t.check_equal(b.hub.interrupts.size(), std::size_t{1});
    }};

} // namespace
