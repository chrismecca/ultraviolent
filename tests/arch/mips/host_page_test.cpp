#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <vector>

// The CPU's host page caches (cpu.hpp) must be invisible: every case where the full path
// would behave differently from the cached page must still take the full path.
namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using testing::kseg0;
using testing::System;

constexpr std::uint32_t code_of(ExceptionCode code) {
    return static_cast<std::uint32_t>(code);
}

constexpr std::uint64_t lo(std::uint64_t pfn, bool dirty) {
    return (pfn << entry_lo::pfn_shift) | (std::uint64_t{3} << entry_lo::c_shift) |
           (dirty ? entry_lo::d : 0) | entry_lo::v;
}

void write_entry(System& s, std::uint64_t hi, std::uint64_t lo0, std::uint64_t lo1,
                 unsigned index = 0) {
    s.cpu.dmtc0(cp0::entry_hi, hi);
    s.cpu.dmtc0(cp0::entry_lo0, lo0);
    s.cpu.dmtc0(cp0::entry_lo1, lo1);
    s.cpu.mtc0(cp0::page_mask, 0);
    s.cpu.mtc0(cp0::index, index);
    s.cpu.tlb_write_indexed();
}

// Records writes; reads return a fixed value.
class Recorder final : public MmioTarget {
  public:
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t /*offset*/,
                                                        AccessWidth /*width*/) override {
        return 0x5a5a'5a5a;
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth /*width*/,
                                                std::uint64_t value) override {
        writes.push_back({offset, value});
        return {};
    }
    struct Write {
        std::uint64_t offset;
        std::uint64_t value;
        bool operator==(const Write&) const = default;
    };
    std::vector<Write> writes;
};

const test::Registration watch_after_access{
    "mips.host_pages_watchpoint_armed_later", [](test::Context& t) {
        System s;
        s.set(a0, kseg0(System::data));
        s.set(a1, System::data | 2);
        // The first load makes the page known; arming a load watchpoint must still trap the
        // next load, with no translation change in between.
        s.run_program({lw(t0, 0, a0), mtc0(a1, cp0::watch_lo), lw(t0, 0, a0)});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::watch));
        t.check_equal(s.cpu.dmfc0(cp0::epc), kseg0(System::code + 8));
    }};

const test::Registration load_then_store{
    "mips.host_pages_store_after_load_of_clean_page", [](test::Context& t) {
        // A load makes the page known, but a store to a clean page is still TLB Modified.
        System s;
        const std::uint64_t va = 0xffff'ffff'e000'0000;
        write_entry(s, va, lo(8, false), lo(9, true));
        s.set(a0, va);
        s.run_program({lw(t0, 0, a0), sw(t0, 4, a0)});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::tlb_modified));
        t.check_equal(s.cpu.dmfc0(cp0::epc), kseg0(System::code + 4));
    }};

const test::Registration dirty_cleared{
    "mips.host_pages_store_after_dirty_cleared", [](test::Context& t) {
        System s;
        const std::uint64_t va = 0xffff'ffff'e000'0000;
        write_entry(s, va, lo(8, true), lo(9, true));
        s.set(a0, va);
        s.set(t0, 7);
        s.run_program({sw(t0, 0, a0)});
        t.check_equal(s.peek(0x8000, AccessWidth::bits32), std::uint64_t{7});
        // Rewriting the entry without D makes the same store a TLB Modified exception.
        write_entry(s, va, lo(8, false), lo(9, true));
        s.run_program({sw(t0, 4, a0)});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::tlb_modified));
        t.check_equal(s.peek(0x8004, AccessWidth::bits32), std::uint64_t{0});
    }};

const test::Registration conflicting_entry{"mips.host_pages_conflicting_tlb_write",
                                           [](test::Context& t) {
                                               System s;
                                               const std::uint64_t va = 0xffff'ffff'e000'0000;
                                               s.poke(0x8000, AccessWidth::bits32, 0x1111);
                                               s.poke(0xa000, AccessWidth::bits32, 0x2222);
                                               write_entry(s, va, lo(8, true), lo(9, true), 0);
                                               s.set(a0, va);
                                               s.run_program({lw(t0, 0, a0)});
                                               t.check_equal(s.gpr(t0), std::uint64_t{0x1111});
                                               // Entry 5 maps the same pages elsewhere; the write
                                               // invalidates entry 0, so the page cached through it
                                               // must go too.
                                               write_entry(s, va, lo(0xa, true), lo(0xb, true), 5);
                                               s.run_program({lw(t0, 0, a0)});
                                               t.check_equal(s.gpr(t0), std::uint64_t{0x2222});
                                           }};

const test::Registration exception_round_trip{
    "mips.host_pages_survive_exception_round_trip", [](test::Context& t) {
        // A page cached in user mode stays correct after an exception and ERET; a kernel
        // address cached in kernel mode still faults in user mode.
        System s;
        s.map_user_low();
        s.poke(System::data, AccessWidth::bits32, 0x77);
        s.set(a0, System::data);
        s.run_user_program({lw(t0, 0, a0), syscall()}, 0);
        t.check_equal(s.gpr(t0), std::uint64_t{0x77});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::syscall));
        s.poke(System::data, AccessWidth::bits32, 0x78);
        s.run_user_program({lw(t0, 0, a0)}, 0);
        t.check_equal(s.gpr(t0), std::uint64_t{0x78});
    }};

const test::Registration mode_change{"mips.host_pages_mode_change", [](test::Context& t) {
                                         System s;
                                         s.map_user_low();
                                         s.set(a0, kseg0(System::data));
                                         s.run_program({lw(t0, 0, a0)});
                                         // The same kseg0 address is an address error in User mode.
                                         s.run_user_program({lw(t0, 0, a0)}, 0);
                                         t.check_equal(s.exception_code(),
                                                       code_of(ExceptionCode::address_error_load));
                                     }};

const test::Registration read_only_memory{
    "mips.host_pages_read_only_memory", [](test::Context& t) {
        // Memory a device controls writes to (flash): loads read it; stores reach the device.
        System s;
        MemoryBlock rom{0x1000};
        Recorder device;
        constexpr std::uint64_t base = 0x20'0000;
        t.check(s.bus
                    .map_memory({PhysicalAddress{base}, 0x1000}, rom, 0, MemoryAccess::read_only,
                                &device)
                    .has_value());
        s.set(a0, kseg0(base));
        s.set(t1, 0x99);
        s.run_program({lw(t0, 0, a0), sw(t1, 8, a0), lw(t2, 8, a0)});
        t.check(device.writes == std::vector<Recorder::Write>{{8, 0x99}});
        t.check_equal(s.gpr(t2), std::uint64_t{0});
    }};

const test::Registration remap{
    "mips.host_pages_follow_bus_remap", [](test::Context& t) {
        System s;
        MemoryBlock block{0x1000};
        Recorder device;
        constexpr std::uint64_t base = 0x20'0000;
        const PhysicalRange range{PhysicalAddress{base}, 0x1000};
        t.check(s.bus.map_memory(range, block, 0, MemoryAccess::read_write).has_value());
        s.set(a0, kseg0(base));
        s.set(t1, 0x42);
        s.run_program({sw(t1, 0, a0), lw(t0, 0, a0)});
        t.check_equal(s.gpr(t0), std::uint64_t{0x42});
        // The range now decodes to a device.
        t.check(s.bus.remap_mmio(range, device).has_value());
        s.run_program({sw(t1, 4, a0), lw(t0, 0, a0)});
        t.check(device.writes == std::vector<Recorder::Write>{{4, 0x42}});
        t.check_equal(s.gpr(t0), std::uint64_t{0x5a5a'5a5a});
    }};

// The device page cache (cpu.hpp "Device pages") must be invisible too.

// Reads return the device's tag plus the offset; the doubleword at 0xff8 answers with an
// error; writes are recorded.
class Echo final : public MmioTarget {
  public:
    explicit Echo(std::uint64_t tag) : tag_{tag} {}
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth width) override {
        ++reads;
        if ((offset & 0xfff) == 0xff8) {
            return std::unexpected(AccessFault::unmapped);
        }
        const std::uint64_t value = tag_ + offset;
        return width == AccessWidth::bits64 ? value
                                            : value & ((std::uint64_t{1} << (8 * byte_count(width))) - 1);
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth /*width*/,
                                                std::uint64_t value) override {
        writes.push_back({offset, value});
        return {};
    }
    std::uint64_t reads{};
    std::vector<Recorder::Write> writes;

  private:
    std::uint64_t tag_;
};

constexpr std::uint64_t device_a = 0x20'0000;
constexpr std::uint64_t device_b = 0x30'0000;

void map_devices(test::Context& t, System& s, Echo& a, Echo& b) {
    t.check(s.bus.map_mmio({PhysicalAddress{device_a}, 0x2000}, a).has_value());
    t.check(s.bus.map_mmio({PhysicalAddress{device_b}, 0x2000}, b).has_value());
}

const test::Registration device_offsets{
    "mips.device_pages_offsets_and_bus_errors", [](test::Context& t) {
        System s;
        Echo a{0x1000'0000}, b{0x2000'0000};
        map_devices(t, s, a, b);
        s.set(a0, kseg0(device_a + 0x1000));
        s.set(t3, 0x66);
        // The first load fills the page; the others use it, at other offsets and widths.
        s.run_program({lw(t0, 0x10, a0), ld(t1, 0x20, a0), lhu(t2, 0x32, a0), sb(t3, 0x41, a0)});
        t.check_equal(s.gpr(t0), std::uint64_t{0x1000'1010});
        t.check_equal(s.gpr(t1), std::uint64_t{0x1000'1020});
        t.check_equal(s.gpr(t2), std::uint64_t{0x1032});
        t.check(a.writes == std::vector<Recorder::Write>{{0x1041, 0x66}});
        // An error response is a bus error, as on the full path.
        s.run_program({ld(t0, 0xff8, a0)});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::bus_error_data));
        t.check_equal(s.cpu.dmfc0(cp0::epc), testing::kseg0(System::code));
    }};

const test::Registration device_neighbors{
    "mips.device_pages_hold_registers_64k_apart", [](test::Context& t) {
        // A Hub's register regions are 64 KB apart (IP27.adoc "Hub"), and the firmware's delay
        // loops alternate between them: their pages must not displace each other (measured
        // 2026-11-05: with the index taken from address bits 15:12 alone, PI_RT_COUNT's page
        // and two others shared one entry, and 38 million `prom` loads took the full path).
        System s;
        Echo a{0x1000'0000};
        t.check(s.bus.map_mmio({PhysicalAddress{device_a}, 0x10'0000}, a).has_value());
        s.set(a0, kseg0(device_a + 0x3'0000));
        s.set(a1, kseg0(device_a + 0x6'0000));
        s.set(a2, kseg0(device_a + 0xa'2000));
        const auto loads = [&] {
            s.run_program({ld(t0, 0x100, a0), ld(t1, 0x8, a1), ld(t2, 0x10, a2)});
        };
        loads();
        const std::uint64_t fills = s.cpu.device_page_fills();
        t.check_equal(fills, std::uint64_t{3});
        loads();
        loads();
        t.check_equal(s.cpu.device_page_fills(), fills);
        t.check_equal(s.gpr(t0), std::uint64_t{0x1003'0100});
        t.check_equal(s.gpr(t1), std::uint64_t{0x1006'0008});
        t.check_equal(s.gpr(t2), std::uint64_t{0x100a'2010});
        t.check_equal(a.reads, std::uint64_t{9});
    }};

// A device that promises its reads hold for `hold` cycles until it is written (its stamp;
// MmioTarget::mmio_read_held), but returns a new value on every read that reaches it, so
// that a held read is visible.
class Ticker final : public MmioTarget {
  public:
    Ticker(const Cpu& cpu, std::uint64_t hold) : cpu_{cpu}, hold_{hold} {}
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t /*offset*/,
                                                        AccessWidth width) override {
        ++reads;
        const std::uint64_t value = 0x7000 + reads;
        return width == AccessWidth::bits64 ? value : value & 0xff;
    }
    std::expected<HeldRead, AccessFault> mmio_read_held(std::uint64_t offset,
                                                        AccessWidth width) override {
        const auto value = mmio_read(offset, width);
        if (hold_ == 0) {
            return HeldRead{.value = *value};
        }
        return HeldRead{.value = *value,
                        .until = cpu_.cycles() + hold_,
                        .stamp = &stamp_,
                        .stamp_value = stamp_.load()};
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t /*offset*/, AccessWidth /*width*/,
                                                std::uint64_t /*value*/) override {
        ++writes;
        ++stamp_;
        return {};
    }
    std::uint64_t reads{};
    std::uint64_t writes{};

  private:
    const Cpu& cpu_;
    std::uint64_t hold_;
    std::atomic<std::uint64_t> stamp_{};
};

const test::Registration device_held_reads{
    "mips.device_reads_held", [](test::Context& t) {
        // A held read is answered by the CPU for the same offset and width until the device's
        // stamp changes or a new run starts (JIT.adoc "Device time"); other bus accesses do
        // not end it.
        System s;
        Ticker ticker{s.cpu, 100};
        Echo other{0x1000'0000};
        t.check(s.bus.map_mmio({PhysicalAddress{device_a}, 0x1000}, ticker).has_value());
        t.check(s.bus.map_mmio({PhysicalAddress{device_b}, 0x1000}, other).has_value());
        s.set(a0, kseg0(device_a));
        s.set(a1, kseg0(device_b));
        // Read 1 takes the full path and fills the device page, and the first store makes it
        // a page for stores too (a store that fills a page drops what it held); reads through
        // the page can be held.
        s.run_program_in_one_run({
            ld(t7, 32, a0),               // read 1
            sd(zero, 24, a0),             // write 1
            ld(t0, 0, a0),                // read 2
            lw(t8, 0, a1),                // another device
            ld(t1, 0, a0),                // held
            lb(t2, 0, a0),                // another width: read 3
            ld(t3, 0, a0),                // the page holds only the last: read 4
            ld(t4, 8, a0),                // another offset: read 5
            sd(zero, 16, a0),             // write 2: the stamp changes
            ld(t5, 8, a0), ld(t6, 8, a0), // read 6, then held
        });
        t.check_equal(s.gpr(t0), std::uint64_t{0x7002});
        t.check_equal(s.gpr(t1), std::uint64_t{0x7002});
        t.check_equal(s.gpr(t2), std::uint64_t{0x03});
        t.check_equal(s.gpr(t3), std::uint64_t{0x7004});
        t.check_equal(s.gpr(t4), std::uint64_t{0x7005});
        t.check_equal(s.gpr(t5), std::uint64_t{0x7006});
        t.check_equal(s.gpr(t6), std::uint64_t{0x7006});
        t.check_equal(ticker.reads, std::uint64_t{6});
        t.check_equal(ticker.writes, std::uint64_t{2});
        s.run_program_in_one_run({ld(t0, 8, a0)}); // a new run: events may have run
        t.check_equal(s.gpr(t0), std::uint64_t{0x7007});
    }};

const test::Registration device_held_until{
    "mips.device_reads_held_until", [](test::Context& t) {
        // Held for two cycles: the load one cycle later is answered, the one two cycles later
        // reaches the device. A device that promises nothing is read every time.
        System s;
        Ticker held{s.cpu, 2};
        Ticker plain{s.cpu, 0};
        t.check(s.bus.map_mmio({PhysicalAddress{device_a}, 0x1000}, held).has_value());
        t.check(s.bus.map_mmio({PhysicalAddress{device_b}, 0x1000}, plain).has_value());
        s.set(a0, kseg0(device_a));
        s.set(a1, kseg0(device_b));
        s.run_program_in_one_run({ld(t7, 32, a0), ld(t7, 32, a1), // fill both device pages
                                  ld(t0, 0, a0), ld(t1, 0, a0), ld(t2, 0, a0), ld(t3, 0, a1),
                                  ld(t3, 0, a1)});
        t.check_equal(s.gpr(t0), std::uint64_t{0x7002});
        t.check_equal(s.gpr(t1), std::uint64_t{0x7002});
        t.check_equal(s.gpr(t2), std::uint64_t{0x7003});
        t.check_equal(held.reads, std::uint64_t{3});
        t.check_equal(plain.reads, std::uint64_t{3});
    }};

const test::Registration device_tlb{
    "mips.device_pages_follow_tlb_writes", [](test::Context& t) {
        System s;
        Echo a{0x1000'0000}, b{0x2000'0000};
        map_devices(t, s, a, b);
        const std::uint64_t va = 0xffff'ffff'e000'0000;
        write_entry(s, va, lo(device_a >> 12, true), lo((device_a >> 12) + 1, true));
        s.set(a0, va);
        s.run_program({lw(t0, 4, a0), lw(t0, 8, a0)});
        t.check_equal(s.gpr(t0), std::uint64_t{0x1000'0008});
        write_entry(s, va, lo(device_b >> 12, true), lo((device_b >> 12) + 1, true));
        s.run_program({lw(t0, 8, a0)});
        t.check_equal(s.gpr(t0), std::uint64_t{0x2000'0008});
        // A clean page: a load fills the cache, and a store is still TLB Modified.
        write_entry(s, va, lo(device_a >> 12, false), lo((device_a >> 12) + 1, false));
        s.run_program({lw(t0, 0, a0), sw(t0, 4, a0)});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::tlb_modified));
        t.check(a.writes.empty(), "the store did not reach the device");
    }};

const test::Registration device_remap{
    "mips.device_pages_follow_bus_remap", [](test::Context& t) {
        System s;
        Echo a{0x1000'0000}, b{0x2000'0000};
        map_devices(t, s, a, b);
        s.set(a0, kseg0(device_a));
        s.run_program({lw(t0, 0x10, a0), lw(t0, 0x14, a0)});
        t.check_equal(s.gpr(t0), std::uint64_t{0x1000'0014});
        t.check(s.bus.remap_mmio({PhysicalAddress{device_a}, 0x2000}, b).has_value());
        s.run_program({lw(t0, 0x18, a0)});
        t.check_equal(s.gpr(t0), std::uint64_t{0x2000'0018});
    }};

const test::Registration device_watch{
    "mips.device_pages_watchpoint_armed_later", [](test::Context& t) {
        System s;
        Echo a{0x1000'0000}, b{0x2000'0000};
        map_devices(t, s, a, b);
        s.set(a0, kseg0(device_a));
        s.set(a1, device_a | 0x10 | 2);
        s.run_program({lw(t0, 0x10, a0), mtc0(a1, cp0::watch_lo), lw(t0, 0x10, a0)});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::watch));
        t.check_equal(s.cpu.dmfc0(cp0::epc), testing::kseg0(System::code + 8));
        t.check_equal(a.reads, std::uint64_t{1});
    }};

const test::Registration device_mode{
    "mips.device_pages_mode_change", [](test::Context& t) {
        System s;
        Echo a{0x1000'0000}, b{0x2000'0000};
        map_devices(t, s, a, b);
        s.map_user_low();
        s.set(a0, kseg0(device_a));
        s.run_program({lw(t0, 0x10, a0)});
        // The same kseg0 address is an address error in User mode.
        s.run_user_program({lw(t0, 0x10, a0)}, 0);
        t.check_equal(s.exception_code(), code_of(ExceptionCode::address_error_load));
        t.check_equal(a.reads, std::uint64_t{1});
    }};

} // namespace
