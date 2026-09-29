#include "support/test.hpp"

#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/memory_block.hpp>
#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/machines/ip27/directory_memory.hpp>
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
        mapped =
            bus.map_memory({PhysicalAddress{0}, memory.size()}, memory, 0, MemoryAccess::read_write)
                .has_value();
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
    devices::I2cBus i2c_bus;
    devices::Pcf8584 i2c{scheduler, tracer, i2c_bus};
    ip27::DirectoryMemory directory{
        tracer, {16u << 20, 0, 0, 0, 0, 0, 0, 0}, ip27::DirectoryDimms::premium};
    // Node memory the block-transfer engines reach.
    AddressSpace bus{ByteOrder::big};
    MemoryBlock memory{1u << 20};
    bool mapped = false;
    // Slot ID 7: node slot n1.
    Hub hub{scheduler, tracer, Hub::default_revision,    7, i2c,
            directory, bus,    [this] { ++node_resets; }};
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

const test::Registration comparators{
    "ip27.hub.rt_and_profile_comparators", [](test::Context& t) {
        Bench b;
        Pins cpu;
        b.hub.connect_cpu(0, cpu);
        // RT compare A at count 10 (8 us at 1.25 MHz), enabled for CPU A.
        t.check(b.write(0x140, 1));  // PI_RT_EN_A
        t.check(b.write(0x108, 10)); // PI_RT_COMPARE_A
        b.scheduler.advance_to(VirtualTime{7'200});
        t.check_equal(b.read(0x120), std::uint64_t{0});
        t.check(!cpu.level[2]);
        b.scheduler.advance_to(VirtualTime{8'000});
        t.check_equal(b.read(0x120), std::uint64_t{1}); // PI_RT_PEND_A latched
        t.check(cpu.level[2], "HUB_IP_RT is the CPU's third input (IP4)");
        // It stays latched after the counter moves on, until software clears it.
        b.scheduler.advance_to(VirtualTime{20'000});
        t.check(cpu.level[2]);
        t.check(b.write(0x120, 0));
        t.check(!cpu.level[2]);
        // A compare value already passed does not fire.
        t.check(b.write(0x108, 5));
        b.scheduler.advance_to(VirtualTime{100'000});
        t.check_equal(b.read(0x120), std::uint64_t{0});
        // The profile comparator pends both CPUs; only enabled ones see IP5.
        t.check(b.write(0x150, 1));   // PI_PROF_EN_A
        t.check(b.write(0x118, 200)); // PI_PROFILE_COMPARE
        b.scheduler.advance_to(VirtualTime{160'000});
        t.check_equal(b.read(0x130), std::uint64_t{1});
        t.check_equal(b.read(0x138), std::uint64_t{1});
        t.check(cpu.level[3]);
        // Rewriting the counter moves the deadline.
        t.check(b.write(0x108, 1'000));
        t.check(b.write(0x030100, 990));
        b.scheduler.advance_by(VirtualDuration{8'000});
        t.check_equal(b.read(0x120), std::uint64_t{1});
    }};

const test::Registration slot_status{
    "ip27.hub.md_slotid_ustat", [](test::Context& t) {
        Bench b;
        t.check_equal(b.read(0x220048), std::uint64_t{0x17}); // FPROMRDY, slot ID 7
        // The junk bus reaches the PCF8584: S1 at UREG0_1, S0 at UREG0_0.
        t.check(b.write(0x220008, 0x80));
        t.check(b.write(0x220000, 0x7f));
        t.check_equal(b.read(0x220000), std::uint64_t{0x7f});
        // A completed byte with ENI set raises I2CINTR.
        t.check(b.write(0x220008, 0xcd));
        b.scheduler.advance_by(VirtualDuration::from_milliseconds(1));
        t.check_equal(b.read(0x220048), std::uint64_t{0x1f});
    }};

const test::Registration unmodeled{
    "ip27.hub.unmodeled_accesses_fault", [](test::Context& t) {
        Bench b;
        const auto read = b.hub.mmio_read(0x2000b0, AccessWidth::bits64);
        t.check(!read && read.error() == AccessFault::unsupported);
        const auto narrow = b.hub.mmio_read(0x20, AccessWidth::bits32);
        t.check(!narrow, "only doubleword accesses are modeled");
        t.check(!b.sink.lines.empty() && b.sink.lines.back().starts_with("unmodeled read"),
                "traced");
    }};

const test::Registration memory_config{
    "ip27.hub.md_memory_config", [](test::Context& t) {
        Bench b;
        // MMC_RESET_DEFAULTS from the IRIX-derived hubmd.h: every bank at 512 MB.
        const std::uint64_t reset = b.read(0x200018);
        t.check_equal(reset & 0xff'ffff, std::uint64_t{0xff'ffff});
        t.check_equal(reset, std::uint64_t{0x001e'7fbc'3fff'ffff});
        // The PROM's SPEEDUP write is stored as written (observed value).
        t.check(b.write(0x200018, 0x0010'1fa8'1fff'ffff));
        t.check_equal(b.read(0x200018), std::uint64_t{0x0010'1fa8'1fff'ffff});
    }};

const test::Registration error_block{
    "ip27.hub.no_errors_reported", [](test::Context& t) {
        Bench b;
        t.check_equal(b.read(0x400), std::uint64_t{0}); // PI_ERR_INT_PEND
        t.check(b.write(0x400, 0xaa'aaaa));             // PI_ERR_CLEAR_ALL_A
        for (std::uint64_t offset = 0x430; offset <= 0x468; offset += 8) {
            t.check_equal(b.read(offset), std::uint64_t{0}); // ERR_STATUS*
        }
        t.check(b.write(0x408, 0x1234)); // PI_ERR_INT_MASK_A is stored
        t.check_equal(b.read(0x408), std::uint64_t{0x1234});
        t.check_equal(b.read(0x608008), std::uint64_t{0}); // NI_PORT_ERROR
        t.check(b.write(0x608000, 7));                     // NI_PORT_PARMS is stored
        t.check_equal(b.read(0x608000), std::uint64_t{7});
    }};

const test::Registration dimm_init{
    "ip27.hub.md_dimm_init", [](test::Context& t) {
        Bench b;
        // MD_MEM_DIMM_INIT and MD_DIR_DIMM_INIT: DIMM select 35:32, mode 11:0. MDIRINIT
        // writes (n << 32) | 0x29 for each n (observed).
        t.check(b.write(0x200090, 0xf'0000'0029));
        t.check(b.write(0x200098, 0xffff'fff3'ffff'f029));
        t.check_equal(b.read(0x200090), std::uint64_t{0xf'0000'0029});
        t.check_equal(b.read(0x200098), std::uint64_t{0x3'0000'0029});
    }};

const test::Registration directory_mode{
    "ip27.hub.dir_premium_mode", [](test::Context& t) {
        Bench b;
        const auto entry = [&b] {
            return b.directory.mmio_read(0, AccessWidth::bits64).value_or(0xdead);
        };
        // DIR_PREMIUM is set at reset: premium DIMMs hold 48-bit entries.
        t.check(b.directory.mmio_write(0, AccessWidth::bits64, 0xaaaa'bbbb'cccc).has_value());
        t.check_equal(entry(), std::uint64_t{0xaaaa'bbbb'cccc});
        // Clearing it in MD_MEMORY_CONFIG narrows the directory to standard entries.
        t.check(b.write(0x200018, b.read(0x200018) & ~(std::uint64_t{1} << 28)));
        t.check_equal(entry(), std::uint64_t{0xcccc});
        // A Hub local reset restores MMC_RESET_DEFAULTS; directory contents survive.
        t.check(b.write(0x600008, 1));
        b.scheduler.advance_by(VirtualDuration{0});
        t.check_equal(entry(), std::uint64_t{0xaaaa'bbbb'cccc});
    }};

const test::Registration microlan{
    "ip27.hub.md_mlan_ctl", [](test::Context& t) {
        Bench b;
        // MLAN_RESET_DEFAULTS: PHI1 = PHI0 = 0x31; idle, so DONE.
        t.check_equal(b.read(0x2000a8) & ~std::uint64_t{3},
                      (std::uint64_t{0x31} << 27) | (std::uint64_t{0x31} << 20));
        // The PROM's first operation (observed): a 520-unit pulse sampled at 65, a 1-Wire
        // reset and presence detect. Nothing is on the bus, so the pulled-up line reads 1.
        t.check(b.write(0x2000a8, 0x1'62c8'2104));
        t.check_equal(b.read(0x2000a8), std::uint64_t{0x1'62c8'2104} | 0x3);
    }};

const test::Registration block_transfer{
    "ip27.hub.bte_copy_and_fill", [](test::Context& t) {
        Bench b;
        t.check(b.mapped);
        for (std::uint64_t i = 0; i < 0x100; i += 8) {
            t.check(b.bus.write(PhysicalAddress{0x1'0000 + i}, AccessWidth::bits64, 0x1111 * i + 1)
                        .has_value());
            t.check(
                b.bus.write(PhysicalAddress{0x3'0000 + i}, AccessWidth::bits64, ~i).has_value());
        }
        // The PROM's sequence (observed): length/status with BUSY and a count of 128-byte
        // lines, cached source and destination addresses, interrupt level 63 for node 0, then
        // IBCT = 0 (copy) starts it.
        t.check(b.write(0x410000, 0x10'0002));
        t.check(b.write(0x410008, 0xa800'0000'0001'0000));
        t.check(b.write(0x410010, 0xa800'0000'0002'0000));
        t.check(b.write(0x410028, 0x3f'0000));
        t.check(b.write(0x410018, 0));
        t.check_equal(b.bus.read(PhysicalAddress{0x2'00f8}, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x1111 * 0xf8 + 1});
        t.check_equal(b.bus.read(PhysicalAddress{0x2'0100}, AccessWidth::bits64).value_or(1),
                      std::uint64_t{0});                     // two lines only
        t.check_equal(b.read(0x410000), std::uint64_t{0});   // done: not busy
        t.check_equal(b.read(0x98), std::uint64_t{1} << 63); // level 63 pending
        // BTE 1 zero-fills (IBCT ZFIL_MODE) and writes its status to the notification
        // address (IBCT NOTIFY).
        t.check(b.write(0x420000, 0x10'0001));
        t.check(b.write(0x420008, 0x1'0000)); // a copy would bring nonzero data
        t.check(b.write(0x420010, 0x3'0000));
        t.check(b.write(0x420020, 0x4'0000));
        t.check(b.write(0x420028, 0x3e'0000));
        t.check(b.bus.write(PhysicalAddress{0x4'0000}, AccessWidth::bits64, 0xffff).has_value());
        t.check(b.write(0x420018, 0x11));
        t.check_equal(b.bus.read(PhysicalAddress{0x3'0078}, AccessWidth::bits64).value_or(1),
                      std::uint64_t{0});
        t.check_equal(b.bus.read(PhysicalAddress{0x3'0080}, AccessWidth::bits64).value_or(0),
                      ~std::uint64_t{0x80});
        t.check_equal(b.bus.read(PhysicalAddress{0x4'0000}, AccessWidth::bits64).value_or(1),
                      std::uint64_t{0});
        t.check_equal(b.read(0x98), (std::uint64_t{1} << 63) | (std::uint64_t{1} << 62));
        // Without BUSY, writing IBCT starts nothing (INITII zeroes every BTE register).
        t.check(b.write(0x410000, 0x2));
        t.check(b.write(0x410010, 0x5'0000));
        t.check(b.write(0x410018, 0x1));
        t.check_equal(b.read(0x410000), std::uint64_t{0x2});
    }};

const test::Registration crosstalk_widget{
    "ip27.hub.crosstalk_widget", [](test::Context& t) {
        Bench b;
        // The II registers are the Hub's widget space: WIDGET_CONTROL (0x24) is IIO_WCR.
        t.check(b.write(0x400020, 0x8019));
        t.check_equal(b.hub.widget().mmio_read(0x24, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0x8019});
        t.check_equal(b.hub.widget().mmio_read(0x04, AccessWidth::bits32).value_or(0) >> 12 &
                          0xffff,
                      std::uint64_t{0xc101});
        // An interrupt aimed at PI_INT_PEND_MOD through the remote alias sets its level.
        b.hub.widget().xtalk_interrupt(0x8000'0180'0090, 0x10);
        t.check_equal(b.read(0x98), std::uint64_t{1} << 16);
        // Without a crosstalk link, PIO to another widget fails.
        t.check(!b.hub.io_window(0).mmio_read(4, AccessWidth::bits32));
    }};

} // namespace

namespace {

// Records crosstalk requests.
class Recorder final : public xtalk::Link {
  public:
    std::expected<std::uint64_t, AccessFault> xtalk_read(unsigned widget, std::uint64_t offset,
                                                         AccessWidth /*width*/) override {
        last_widget = widget;
        last_offset = offset;
        return 0x1234;
    }
    std::expected<void, AccessFault> xtalk_write(unsigned widget, std::uint64_t offset,
                                                 AccessWidth /*width*/,
                                                 std::uint64_t /*value*/) override {
        last_widget = widget;
        last_offset = offset;
        return {};
    }
    unsigned last_widget = 99;
    std::uint64_t last_offset = 0;
};

const test::Registration big_windows{
    "ip27.hub.big_window_itte", [](test::Context& t) {
        Bench b;
        Recorder link;
        b.hub.connect_xtalk(link);
        // Reset: widget 0, offset 0.
        t.check_equal(b.hub.big_window(7).mmio_read(0x24, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0x1234});
        t.check_equal(link.last_widget, 0u);
        t.check_equal(link.last_offset, std::uint64_t{0x24});
        // ITTE 2 names widget 8 at the second 512 MB of its space.
        t.check(b.write(0x40'0168, 0x801));
        (void)b.hub.big_window(2).mmio_write(0xc0'0000, AccessWidth::bits8, 0xaa);
        t.check_equal(link.last_widget, 8u);
        t.check_equal(link.last_offset, (std::uint64_t{1} << 29) + 0xc0'0000);
    }};

} // namespace
