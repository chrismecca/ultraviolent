#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/pcf8584.hpp>

#include <cstdint>

namespace {

using namespace ultraviolent;
using devices::Pcf8584;

struct Bench {
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    Pcf8584 chip{scheduler, tracer};
};

// Status bits (Linux i2c-algo-pcf.h).
constexpr std::uint8_t pin = 0x80;
constexpr std::uint8_t ini = 0x40;
constexpr std::uint8_t lrb = 0x08;
constexpr std::uint8_t bb = 0x01;

const test::Registration initialization{
    "pcf8584.initialization", [](test::Context& t) {
        Bench b;
        t.check_equal(b.chip.read(true), std::uint8_t{pin | ini | bb});
        // The IP27 PROM's sequence: select own address, write it, select the clock register,
        // write it, then enable the serial interface.
        b.chip.write(true, 0x80);
        b.chip.write(false, 0x7f);
        t.check_equal(b.chip.read(false), std::uint8_t{0x7f}); // own address reads back
        b.chip.write(true, 0xa0);
        b.chip.write(false, 0x1c);
        t.check_equal(b.chip.read(false), std::uint8_t{0x1c}); // clock register
        b.chip.write(true, 0xc0);
        t.check_equal(b.chip.read(true), std::uint8_t{pin | bb}); // initialized, idle, bus free
    }};

const test::Registration unacknowledged{
    "pcf8584.address_without_device", [](test::Context& t) {
        Bench b;
        b.chip.write(true, 0x80);
        b.chip.write(false, 0x7f);
        b.chip.write(true, 0xa0);
        b.chip.write(false, 0x1c); // 90 kHz SCL
        b.chip.write(true, 0xc1);
        b.chip.write(false, 0xa0); // slave address byte
        b.chip.write(true, 0xc5);  // START
        t.check((b.chip.read(true) & pin) != 0, "transfer in progress");
        t.check((b.chip.read(true) & bb) == 0, "bus busy");
        // Nine bit times at 90 kHz: 100 microseconds.
        b.scheduler.advance_by(VirtualDuration{99'999});
        t.check((b.chip.read(true) & pin) != 0, "not yet done");
        b.scheduler.advance_by(VirtualDuration{1});
        const std::uint8_t status = b.chip.read(true);
        t.check((status & pin) == 0, "byte complete");
        t.check((status & lrb) != 0, "no device acknowledged");
        b.chip.write(true, 0xc3); // STOP
        t.check_equal(b.chip.read(true), std::uint8_t{pin | bb});
    }};

const test::Registration interrupt{
    "pcf8584.interrupt_output", [](test::Context& t) {
        Bench b;
        b.chip.write(true, 0x80);
        b.chip.write(false, 0x7f);
        b.chip.write(true, 0xcd); // ESO | ENI | STA | ACK
        t.check(!b.chip.interrupt_asserted(), "pending until the byte completes");
        b.scheduler.advance_by(VirtualDuration::from_milliseconds(10));
        t.check(b.chip.interrupt_asserted());
    }};

} // namespace
