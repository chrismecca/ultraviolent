#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/machines/ip27/hub.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace ultraviolent;
using ip27::Hub;

class Lines final : public TraceSink {
  public:
    void write(const TraceRecord& record) override {
        lines.emplace_back(record.message);
    }
    std::vector<std::string> lines;
};

// A Hub with its own clock, scheduler, and a counter of node resets.
struct Bench {
    Bench() {
        tracer.set_sink(&sink);
        tracer.enable(TraceCategory::firmware);
        tracer.enable(TraceCategory::hub);
    }

    std::uint64_t read(std::uint64_t offset) {
        return hub.mmio_read(offset, AccessWidth::bits64).value_or(0xdead);
    }
    bool write(std::uint64_t offset, std::uint64_t value) {
        return hub.mmio_write(offset, AccessWidth::bits64, value).has_value();
    }

    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    Lines sink;
    int node_resets = 0;
    devices::Pcf8584 i2c{scheduler, tracer};
    Hub hub{scheduler, tracer, Hub::default_revision, i2c, [this] { ++node_resets; }};
};

const test::Registration cpu_number{"ip27.hub.pi_cpu_num", [](test::Context& t) {
                                        Bench b;
                                        t.check_equal(b.read(0x20), std::uint64_t{0}); // slice A
                                        t.check(b.write(0x20, 0x1234), "writes are accepted");
                                        t.check_equal(b.read(0x20),
                                                      std::uint64_t{0}); // and have no effect
                                    }};

const test::Registration real_time_counter{"ip27.hub.pi_rt_count", [](test::Context& t) {
                                               Bench b;
                                               // 1.25 MHz: one tick per 800 ns of virtual time.
                                               b.scheduler.advance_to(VirtualTime{8'000});
                                               t.check_equal(b.read(0x030100), std::uint64_t{10});
                                               t.check(b.write(0x030100, 1000));
                                               b.scheduler.advance_by(VirtualDuration{1'600});
                                               t.check_equal(b.read(0x030100), std::uint64_t{1002});
                                               // 52 bits wide.
                                               t.check(b.write(0x030100, ~std::uint64_t{0}));
                                               t.check_equal(b.read(0x030100),
                                                             (std::uint64_t{1} << 52) - 1);
                                           }};

const test::Registration leds{
    "ip27.hub.md_leds", [](test::Context& t) {
        Bench b;
        t.check(b.write(0x220050, 0x123));
        t.check(b.write(0x220058, 0x05));
        t.check_equal(b.read(0x220050), std::uint64_t{0x23}); // eight bits
        t.check(b.sink.lines == std::vector<std::string>{"led A 0x23", "led B 0x05"});
    }};

const test::Registration local_reset{
    "ip27.hub.local_reset", [](test::Context& t) {
        Bench b;
        t.check(b.write(0x420, 0x5273'7430)); // PI_ERR_STACK_ADDR_B, as the PROM does
        t.check(b.write(0x600100, 7));        // NI_SCRATCH_REG0
        t.check(b.write(0x600008, 0x81));     // NI_PORT_RESET: local reset
        // The reset waits for the requesting store to complete, then resets the node.
        t.check_equal(b.node_resets, 0);
        b.scheduler.advance_by(VirtualDuration{0});
        t.check_equal(b.node_resets, 1);
        t.check_equal(b.read(0x420), std::uint64_t{0x5273'7430}); // survives
        t.check_equal(b.read(0x600100), std::uint64_t{0});        // reinitialized
    }};

class Pins final : public InterruptSink {
  public:
    void set_interrupt_level(std::uint32_t input, bool asserted) override {
        level[input] = asserted;
    }
    bool level[5]{};
};

const test::Registration interrupts{
    "ip27.hub.pi_interrupts", [](test::Context& t) {
        Bench b;
        Pins cpu;
        b.hub.connect_cpu(0, cpu);
        // Set level 113 (INT_PEND1 bit 49) the way the PROM does.
        t.check(b.write(0x90, 0x171));
        t.check_equal(b.read(0xa0), std::uint64_t{1} << 49);
        t.check(!cpu.level[1], "masked");
        t.check(b.write(0xb0, std::uint64_t{1} << 49)); // PI_INT_MASK1_A
        t.check(cpu.level[1], "INT_PEND1 raises the CPU's second input (IP3)");
        t.check(!cpu.level[0]);
        t.check(b.write(0x90, 0x71)); // clear
        t.check_equal(b.read(0xa0), std::uint64_t{0});
        t.check(!cpu.level[1], "cleared");
        // Levels below 64 are INT_PEND0 and reach input 0 (IP2).
        t.check(b.write(0xa8, 1u << 5));
        t.check(b.write(0x90, 0x100 | 5));
        t.check_equal(b.read(0x98), std::uint64_t{1} << 5);
        t.check(cpu.level[0]);
    }};

const test::Registration slot_status{
    "ip27.hub.md_slotid_ustat", [](test::Context& t) {
        Bench b;
        t.check_equal(b.read(0x220048), std::uint64_t{0x10}); // FPROMRDY, slot 0
        // The junk bus reaches the PCF8584: S1 at UREG0_1, S0 at UREG0_0.
        t.check(b.write(0x220008, 0x80));
        t.check(b.write(0x220000, 0x7f));
        t.check_equal(b.read(0x220000), std::uint64_t{0x7f});
        // A completed byte with ENI set raises I2CINTR.
        t.check(b.write(0x220008, 0xcd));
        b.scheduler.advance_by(VirtualDuration::from_milliseconds(1));
        t.check_equal(b.read(0x220048), std::uint64_t{0x18});
    }};

const test::Registration unmodeled{
    "ip27.hub.unmodeled_accesses_fault", [](test::Context& t) {
        Bench b;
        const auto read = b.hub.mmio_read(0x400130, AccessWidth::bits64);
        t.check(!read && read.error() == AccessFault::unsupported);
        const auto narrow = b.hub.mmio_read(0x20, AccessWidth::bits32);
        t.check(!narrow, "only doubleword accesses are modeled");
        t.check(!b.sink.lines.empty() && b.sink.lines.back().starts_with("unmodeled read"),
                "traced");
    }};

} // namespace
