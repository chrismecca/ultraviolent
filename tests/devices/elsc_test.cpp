#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/elsc.hpp>
#include <ultraviolent/devices/i2c.hpp>

#include <cstdint>
#include <vector>

namespace {

using namespace ultraviolent;
using devices::Elsc;
using devices::I2cBus;

class Watcher final : public devices::I2cMonitor {
  public:
    void i2c_observed_byte(std::uint8_t byte) override {
        bytes.push_back(byte);
    }
    void i2c_observed_stop() override {
        ++stops;
    }
    std::vector<std::uint8_t> bytes;
    int stops = 0;
};

struct Bench {
    Bench() {
        bus.attach(watcher);
    }
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    I2cBus bus;
    Watcher watcher;
    Elsc elsc{scheduler, tracer, bus};
};

const test::Registration tokens{
    "elsc.rotates_tokens_while_idle", [](test::Context& t) {
        Bench b;
        // One token per period for node slots n1-n4, CPUs A and B: 0xe0-0xe7, then again.
        b.scheduler.advance_by(VirtualDuration{9 * Elsc::token_period.nanoseconds});
        t.check(b.watcher.bytes ==
                std::vector<std::uint8_t>{0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe0});
        // None while a master holds the bus.
        (void)b.bus.start(0x57 << 1);
        b.scheduler.advance_by(VirtualDuration{3 * Elsc::token_period.nanoseconds});
        t.check_equal(b.watcher.bytes.size(), std::size_t{9});
    }};

const test::Registration release{"elsc.takes_bus_back_at_0x7f", [](test::Context& t) {
                                     Bench b;
                                     // A node ends its transaction by addressing 0x7f; the
                                     // controller frees the bus.
                                     t.check(b.bus.start(0xfe), "0x7f acknowledges");
                                     t.check(b.bus.busy());
                                     b.scheduler.advance_by(Elsc::release_delay);
                                     t.check(!b.bus.busy(), "released");
                                     t.check_equal(b.watcher.stops, 1);
                                     // The command interface is not modeled: its address does not
                                     // answer.
                                     t.check(!b.bus.start(0x20 << 1));
                                 }};

} // namespace
