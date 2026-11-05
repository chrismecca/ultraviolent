#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/elsc.hpp>
#include <ultraviolent/devices/i2c.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
        elsc.connect_message_interrupt([this] { ++interrupts; });
        elsc.connect_power_off([this] { ++power_offs; });
    }
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    I2cBus bus;
    Watcher watcher;
    Elsc elsc{scheduler, tracer, bus};
    int interrupts = 0;
    int power_offs = 0;
};

// IRIX's elsc_command: a packet of length, text, and a checksum that makes the bytes sum to
// zero, written to 0x08; the answer read back with a repeated START in the same form. None
// when the controller does not acknowledge the read.
std::optional<std::string> command(I2cBus& bus, std::string_view text,
                                   std::uint8_t corrupt = 0) {
    if (!bus.start(Elsc::command_address << 1)) {
        return std::nullopt;
    }
    auto sum = static_cast<std::uint8_t>(text.size());
    (void)bus.write(static_cast<std::uint8_t>(text.size()));
    for (const char c : text) {
        sum = static_cast<std::uint8_t>(sum + static_cast<std::uint8_t>(c));
        (void)bus.write(static_cast<std::uint8_t>(c));
    }
    (void)bus.write(static_cast<std::uint8_t>(-sum + corrupt));
    std::optional<std::string> answer;
    if (bus.start(Elsc::command_address << 1 | 1)) {
        const std::uint8_t length = bus.read();
        answer.emplace();
        std::uint8_t check = length;
        for (unsigned i = 0; i < length; ++i) {
            const std::uint8_t byte = bus.read();
            check = static_cast<std::uint8_t>(check + byte);
            answer->push_back(static_cast<char>(byte));
        }
        check = static_cast<std::uint8_t>(check + bus.read());
        if (check != 0) {
            answer = "(bad checksum)";
        }
    }
    (void)bus.start(Elsc::release_address << 1);
    bus.release();
    return answer;
}

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
                                     // IRIX's elsc_ust_read and elsc_ust_write address is not
                                     // modeled: it does not answer.
                                     t.check(!b.bus.start(0x20 << 1));
                                 }};

const test::Registration power_down{
    "elsc.pwr_d_removes_power_after_n_seconds", [](test::Context& t) {
        Bench b;
        t.check_equal(command(b.bus, "pwr d a"), std::optional<std::string>{"ok"});
        b.scheduler.advance_by(VirtualDuration{9'999'000'000});
        t.check_equal(b.power_offs, 0); // on for 10 s (hexadecimal a)
        // A later request replaces the time.
        t.check_equal(command(b.bus, "pwr d 1"), std::optional<std::string>{"ok"});
        b.scheduler.advance_by(VirtualDuration{999'000'000});
        t.check_equal(b.power_offs, 0);
        b.scheduler.advance_by(VirtualDuration{1'000'000});
        t.check_equal(b.power_offs, 1); // off 1 s after the last request
        t.check(b.elsc.powered_off());
        // A controller without power offers no tokens.
        const std::size_t seen = b.watcher.bytes.size();
        b.scheduler.advance_by(VirtualDuration{3 * Elsc::token_period.nanoseconds});
        t.check_equal(b.watcher.bytes.size(), seen);
    }};

const test::Registration unmodeled{
    "elsc.commands_not_modeled_are_not_answered", [](test::Context& t) {
        Bench b;
        // The command is taken, but the read address is not acknowledged: the node sees
        // I2C_ERROR_NAK, as it did before commands were modeled.
        t.check(!command(b.bus, "dsp HELLO").has_value());
        // A modeled command with a bad argument: "err arg" (ELSC_ERROR_CMD_ARGS).
        t.check_equal(command(b.bus, "pwr d xyz"), std::optional<std::string>{"err arg"});
        // A packet whose checksum is wrong: "err csum" (ELSC_ERROR_CMD_CHECKSUM in IRIX).
        t.check_equal(command(b.bus, "pwr d 8", 1), std::optional<std::string>{"err csum"});
        t.check_equal(b.power_offs, 0);
    }};

const test::Registration keyswitch{
    "elsc.keyswitch_off_message_and_grace", [](test::Context& t) {
        Bench b;
        b.elsc.turn_keyswitch_off();
        t.check_equal(b.interrupts, 1); // the MSC interrupt
        // Every token carries the message flag (plus 8) until the node fetches the message.
        b.scheduler.advance_by(VirtualDuration{2 * Elsc::token_period.nanoseconds});
        t.check(b.watcher.bytes == std::vector<std::uint8_t>{0xe8, 0xe9});
        t.check_equal(command(b.bus, "get"), std::optional<std::string>{"pd off"});
        b.scheduler.advance_by(Elsc::token_period);
        t.check_equal(b.watcher.bytes.back(), std::uint8_t{0xe2}); // flag cleared
        // Nothing waits now: an empty answer ("MSC interrupt without message" in IRIX).
        t.check_equal(command(b.bus, "get"), std::optional<std::string>{""});
        // A second turn of the key changes nothing while power-off is pending.
        b.elsc.turn_keyswitch_off();
        t.check_equal(b.interrupts, 1);
        // With no word from the system the power goes off after the grace time.
        b.scheduler.advance_by(Elsc::keyswitch_grace);
        t.check_equal(b.power_offs, 1);
    }};

const test::Registration snapshot{
    "elsc.snapshot_keeps_message_and_power_off", [](test::Context& t) {
        Bench b;
        b.elsc.turn_keyswitch_off();
        StateImage image;
        b.elsc.save_state(image);
        b.scheduler.save_state(image);
        Bench c;
        c.scheduler.load_state(image);
        c.elsc.load_state(image);
        t.check_equal(command(c.bus, "get"), std::optional<std::string>{"pd off"});
        c.scheduler.advance_by(Elsc::keyswitch_grace);
        t.check_equal(c.power_offs, 1); // the power-off deadline came with the snapshot
    }};

} // namespace
