#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/pcf8584.hpp>

#include <cstdint>
#include <vector>

namespace {

using namespace ultraviolent;
using devices::Pcf8584;

struct Bench {
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    devices::I2cBus bus;
    Pcf8584 chip{scheduler, tracer, bus};
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

// A target that records what it is sent and answers reads from a counter.
class Recorder final : public devices::I2cTarget {
  public:
    bool i2c_start(std::uint8_t address, bool read) override {
        starts.push_back(static_cast<std::uint8_t>(address << 1 | (read ? 1 : 0)));
        return address == 0x50;
    }
    bool i2c_write(std::uint8_t byte) override {
        written.push_back(byte);
        return true;
    }
    std::uint8_t i2c_read() override {
        return next++;
    }
    void i2c_stop() override {
        ++stops;
    }
    std::vector<std::uint8_t> starts;
    std::vector<std::uint8_t> written;
    std::uint8_t next = 0x10;
    int stops = 0;
};

void initialize(Bench& b) {
    b.chip.write(true, 0x80);
    b.chip.write(false, 0x08);
    b.chip.write(true, 0xa0);
    b.chip.write(false, 0x1c); // 90 kHz: 100 microseconds per byte
    b.chip.write(true, 0xc1);
}

void byte_time(Bench& b) {
    b.scheduler.advance_by(VirtualDuration{100'000});
}

const test::Registration master_transfers{
    "pcf8584.master_write_then_read", [](test::Context& t) {
        Bench b;
        Recorder target;
        b.bus.attach(target);
        initialize(b);
        // The IP27 PROM's random read: address + word address, repeated START with the read
        // bit, a dummy read, NAK before the last byte, then more reads.
        b.chip.write(false, 0xa0);
        b.chip.write(true, 0xc5); // START
        byte_time(b);
        t.check((b.chip.read(true) & (pin | lrb)) == 0, "address acknowledged");
        b.chip.write(false, 0x07);
        byte_time(b);
        t.check((b.chip.read(true) & lrb) == 0, "data acknowledged");
        b.chip.write(true, 0x45); // repeated START, sent with the next S0 write
        b.chip.write(false, 0xa1);
        byte_time(b);
        t.check((b.chip.read(true) & pin) == 0);
        b.chip.write(true, 0x40); // NAK the next byte
        (void)b.chip.read(false); // dummy read starts reception
        t.check((b.chip.read(true) & pin) != 0, "receiving");
        byte_time(b);
        t.check_equal(b.chip.read(false), std::uint8_t{0x10});
        byte_time(b);
        // After a NAKed byte reading S0 starts nothing more.
        t.check_equal(target.next, std::uint8_t{0x11});
        t.check(target.starts == std::vector<std::uint8_t>{0xa0, 0xa1});
        t.check(target.written == std::vector<std::uint8_t>{0x07});
        b.chip.write(true, 0xc3); // STOP
        t.check_equal(target.stops, 1);
        t.check((b.chip.read(true) & bb) != 0, "bus free");
    }};

const test::Registration monitoring{"pcf8584.monitors_other_masters", [](test::Context& t) {
                                        Bench b;
                                        initialize(b);
                                        // Another master's byte lands in the shift register, which
                                        // S0 reads.
                                        b.bus.broadcast(0xe0);
                                        t.check_equal(b.chip.read(false), std::uint8_t{0xe0});
                                        // A STOP from another agent ends this chip's transfer and
                                        // frees the bus.
                                        b.chip.write(false, 0xfe);
                                        b.chip.write(true, 0xc5);
                                        byte_time(b);
                                        t.check((b.chip.read(true) & bb) == 0, "busy while master");
                                        b.bus.release();
                                        t.check((b.chip.read(true) & bb) != 0, "freed");
                                        b.bus.broadcast(0xe1);
                                        t.check_equal(b.chip.read(false), std::uint8_t{0xe1});
                                    }};

} // namespace
