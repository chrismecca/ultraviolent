#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/am29f080.hpp>

#include <cstdint>
#include <vector>

namespace {

using namespace ultraviolent;
using devices::Am29f080;

struct Bench {
    Bench() : array(Am29f080::size, std::byte{0xff}) {}
    void command(std::uint8_t code) {
        flash.write(0x5555, 0xaa);
        flash.write(0x2aaa, 0x55);
        flash.write(0x5555, code);
    }
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    std::vector<std::byte> array;
    int mode_changes = 0;
    Am29f080 flash{scheduler, tracer, array, [this] { ++mode_changes; }};
};

const test::Registration autoselect{"am29f080.autoselect", [](test::Context& t) {
                                        Bench b;
                                        b.array[0] = std::byte{0x0b};
                                        b.command(0x90);
                                        t.check(!b.flash.array_mode());
                                        t.check_equal(b.flash.read(0), std::uint8_t{0x01});
                                        t.check_equal(b.flash.read(1), std::uint8_t{0xd5});
                                        t.check_equal(b.flash.read(0x10002),
                                                      std::uint8_t{0x00}); // sector unprotected
                                        b.flash.write(0, 0xf0);
                                        t.check(b.flash.array_mode());
                                        t.check_equal(b.flash.read(0), std::uint8_t{0x0b});
                                        t.check_equal(b.mode_changes, 2);
                                    }};

const test::Registration program{"am29f080.program_and_poll", [](test::Context& t) {
                                     Bench b;
                                     b.command(0xa0);
                                     b.flash.write(0xe0010, 0x5a);
                                     // While programming: DQ7 is the complement of bit 7, DQ6
                                     // toggles.
                                     const std::uint8_t first = b.flash.read(0xe0010);
                                     const std::uint8_t second = b.flash.read(0xe0010);
                                     t.check_equal(first & 0x80, 0x80);
                                     t.check(((first ^ second) & 0x40) != 0, "DQ6 toggles");
                                     b.scheduler.advance_by(Am29f080::program_time);
                                     t.check(b.flash.array_mode());
                                     t.check_equal(b.flash.read(0xe0010), std::uint8_t{0x5a});
                                     t.check(b.flash.dirty());
                                     // Programming cannot set bits.
                                     b.command(0xa0);
                                     b.flash.write(0xe0010, 0xff);
                                     b.scheduler.advance_by(Am29f080::program_time);
                                     t.check_equal(b.flash.read(0xe0010) & 0x20,
                                                   0x20); // DQ5: failed
                                     b.flash.write(0, 0xf0);
                                     t.check_equal(b.flash.read(0xe0010), std::uint8_t{0x5a});
                                 }};

const test::Registration erase{
    "am29f080.sector_erase", [](test::Context& t) {
        Bench b;
        b.array[0xe0000] = std::byte{0};
        b.array[0xd0000] = std::byte{0};
        b.command(0x80);
        b.flash.write(0x5555, 0xaa);
        b.flash.write(0x2aaa, 0x55);
        b.flash.write(0xe0000, 0x30);                      // sector 14
        t.check_equal(b.flash.read(0xe0000) & 0x88, 0x08); // DQ7 0, DQ3 1
        b.scheduler.advance_by(Am29f080::sector_erase_time);
        t.check(b.flash.array_mode());
        t.check_equal(b.flash.read(0xe0000), std::uint8_t{0xff});
        t.check_equal(b.flash.read(0xd0000), std::uint8_t{0x00}); // other sectors keep data
    }};

} // namespace
