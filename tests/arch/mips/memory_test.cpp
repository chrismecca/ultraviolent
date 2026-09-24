#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <cstdint>
#include <expected>
#include <vector>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using testing::kseg0;
using testing::kseg1;
using testing::System;

constexpr std::uint64_t u(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

constexpr std::uint64_t data = System::data;

const test::Registration load_widths{
    "mips.load_widths_and_extension", [](test::Context& t) {
        System s;
        s.poke(data, AccessWidth::bits64, 0x8182'8384'8586'8788);
        s.set(a0, kseg0(data));
        s.run_program({lb(t0, 0, a0), lbu(t1, 0, a0), lh(t2, 2, a0), lhu(t3, 2, a0), lw(t4, 4, a0),
                       lwu(t5, 4, a0), ld(t6, 0, a0)});
        t.check_equal(s.gpr(t0), u(-0x7f));
        t.check_equal(s.gpr(t1), std::uint64_t{0x81});
        t.check_equal(s.gpr(t2), 0xffff'ffff'ffff'8384u);
        t.check_equal(s.gpr(t3), std::uint64_t{0x8384});
        t.check_equal(s.gpr(t4), 0xffff'ffff'8586'8788u);
        t.check_equal(s.gpr(t5), std::uint64_t{0x8586'8788});
        t.check_equal(s.gpr(t6), 0x8182'8384'8586'8788u);
    }};

const test::Registration store_widths{
    "mips.store_widths", [](test::Context& t) {
        System s;
        s.set(a0, kseg0(data));
        s.set(a1, 0x1122'3344'5566'7788);
        s.run_program({sd(a1, 0, a0), sb(a1, 8, a0), sh(a1, 10, a0), sw(a1, 12, a0)});
        t.check_equal(s.peek(data, AccessWidth::bits64), 0x1122'3344'5566'7788u);
        t.check_equal(s.peek(data + 8, AccessWidth::bits64), 0x8800'7788'5566'7788u);
    }};

const test::Registration kseg_aliases{"mips.kseg0_and_kseg1_alias_physical", [](test::Context& t) {
                                          System s;
                                          s.poke(data, AccessWidth::bits32, 0xcafe'f00d);
                                          s.set(a0, kseg1(data));
                                          s.set(a1,
                                                0x9000'0000'0000'0000 | data); // xkphys, uncached
                                          s.run_program({lw(t0, 0, a0), lw(t1, 0, a1)});
                                          t.check_equal(s.gpr(t0), 0xffff'ffff'cafe'f00du);
                                          t.check_equal(s.gpr(t1), 0xffff'ffff'cafe'f00du);
                                      }};

// Unaligned word access through LWL/LWR must equal assembling the bytes one at a time.
const test::Registration unaligned_words{
    "mips.lwl_lwr_load_unaligned_words", [](test::Context& t) {
        System s;
        for (unsigned i = 0; i < 16; ++i) {
            s.poke(data + i, AccessWidth::bits8, 0x10 + i);
        }
        for (int offset = 0; offset < 4; ++offset) {
            s.set(a0, kseg0(data) + static_cast<std::uint64_t>(offset));
            s.set(t0, 0);
            s.run_program({lwl(t0, 0, a0), lwr(t0, 3, a0)});
            const std::uint64_t base = 0x10 + static_cast<std::uint64_t>(offset);
            const std::uint64_t expected =
                (base << 24) | ((base + 1) << 16) | ((base + 2) << 8) | (base + 3);
            t.check_equal(s.gpr(t0), expected);
        }
    }};

const test::Registration lwl_lwr_partial{"mips.lwl_lwr_merge_rules", [](test::Context& t) {
                                             System s;
                                             s.poke(data, AccessWidth::bits32, 0x8899'aabb);
                                             s.set(a0, kseg0(data));
                                             s.set(t0, 0x1111'1111'2233'4455);
                                             s.set(t1, 0x1111'1111'2233'4455);
                                             s.set(t2, 0x1111'1111'2233'4455);
                                             // Big-endian LWL at byte 1 loads 99 aa bb into the
                                             // high three bytes; LWR at byte 1 loads 88 99 into the
                                             // low two bytes (ISA Tables A-29, A-31).
                                             s.run_program(
                                                 {lwl(t0, 1, a0), lwr(t1, 1, a0), lwr(t2, 3, a0)});
                                             t.check_equal(s.gpr(t0), 0xffff'ffff'99aa'bb55u);
                                             // hypothesis: LWR that does not load bit 31 leaves
                                             // bits 63:32 unchanged.
                                             t.check_equal(s.gpr(t1), 0x1111'1111'2233'8899u);
                                             // LWR of the whole word sign-extends.
                                             t.check_equal(s.gpr(t2), 0xffff'ffff'8899'aabbu);
                                         }};

const test::Registration unaligned_doublewords{
    "mips.ldl_ldr_load_unaligned_doublewords", [](test::Context& t) {
        System s;
        for (unsigned i = 0; i < 24; ++i) {
            s.poke(data + i, AccessWidth::bits8, 0x20 + i);
        }
        for (int offset = 0; offset < 8; ++offset) {
            s.set(a0, kseg0(data) + static_cast<std::uint64_t>(offset));
            s.run_program({ldl(t0, 0, a0), ldr(t0, 7, a0)});
            std::uint64_t expected = 0;
            for (int i = 0; i < 8; ++i) {
                expected = (expected << 8) | static_cast<std::uint64_t>(0x20 + offset + i);
            }
            t.check_equal(s.gpr(t0), expected);
        }
    }};

const test::Registration unaligned_stores{
    "mips.swl_swr_sdl_sdr_store_unaligned", [](test::Context& t) {
        for (int offset = 0; offset < 8; ++offset) {
            System s;
            s.set(a0, kseg0(data) + static_cast<std::uint64_t>(offset));
            s.set(a1, 0x0102'0304'0506'0708);
            s.set(a2, kseg0(data + 16) + static_cast<std::uint64_t>(offset % 4));
            s.run_program({sdl(a1, 0, a0), sdr(a1, 7, a0), swl(a1, 0, a2), swr(a1, 3, a2)});
            for (int i = 0; i < 24; ++i) {
                const auto address = data + static_cast<std::uint64_t>(i);
                const std::uint64_t byte = s.peek(address, AccessWidth::bits8);
                std::uint64_t expected = 0;
                if (i >= offset && i < offset + 8) {
                    expected =
                        static_cast<std::uint64_t>(i) - static_cast<std::uint64_t>(offset) + 1;
                }
                const int word_start = 16 + offset % 4;
                if (i >= word_start && i < word_start + 4) {
                    expected =
                        static_cast<std::uint64_t>(i) - static_cast<std::uint64_t>(word_start) + 5;
                }
                t.check_equal(byte, expected);
            }
        }
    }};

const test::Registration load_linked{
    "mips.ll_sc", [](test::Context& t) {
        System s;
        s.poke(data, AccessWidth::bits32, 5);
        s.set(a0, kseg0(data));
        s.run_program({ll(t0, 0, a0), addiu(t0, t0, 1), sc(t0, 0, a0)});
        t.check_equal(s.gpr(t0), std::uint64_t{1});
        t.check_equal(s.peek(data, AccessWidth::bits32), std::uint64_t{6});

        // SC without a preceding LL fails and does not store.
        System fresh;
        fresh.poke(data, AccessWidth::bits32, 5);
        fresh.set(a0, kseg0(data));
        fresh.set(t0, 9);
        fresh.run_program({sc(t0, 0, a0)});
        t.check_equal(fresh.gpr(t0), std::uint64_t{0});
        t.check_equal(fresh.peek(data, AccessWidth::bits32), std::uint64_t{5});
    }};

const test::Registration eret_breaks_link{
    "mips.eret_clears_ll_bit", [](test::Context& t) {
        System s;
        s.poke(data, AccessWidth::bits64, 5);
        s.set(a0, kseg0(data));
        s.set(a1, kseg0(System::code + 12));
        // LLD, then an ERET (with EXL set so it returns to EPC), then SCD must fail (UM 14.30).
        s.load(System::code, {lld(t0, 0, a0), dmtc0(a1, cp0::epc), eret(), scd(t0, 0, a0)});
        s.start_kernel(kseg0(System::code), status::kx | status::exl);
        s.interpreter.run(4);
        t.check_equal(s.gpr(t0), std::uint64_t{0});
        t.check_equal(s.peek(data, AccessWidth::bits64), std::uint64_t{5});
    }};

const test::Registration reverse_endian{
    "mips.reverse_endian_user_mode", [](test::Context& t) {
        System s;
        // User mode with RE set sees memory little-endian (UM 14.10). Map user page 0 to physical
        // 0 through the TLB so a user program can run.
        s.cpu.mtc0(cp0::entry_hi, 0);
        s.cpu.mtc0(cp0::entry_lo0, (0 << 6) | (3 << 3) | entry_lo::d | entry_lo::v | entry_lo::g);
        s.cpu.mtc0(cp0::entry_lo1, (1 << 6) | (3 << 3) | entry_lo::d | entry_lo::v | entry_lo::g);
        s.cpu.mtc0(cp0::page_mask, 0);
        s.cpu.mtc0(cp0::index, 0);
        s.cpu.tlb_write_indexed();
        s.cpu.mtc0(cp0::entry_lo0, (8 << 6) | (3 << 3) | entry_lo::d | entry_lo::v | entry_lo::g);
        s.cpu.mtc0(cp0::entry_lo1, (9 << 6) | (3 << 3) | entry_lo::d | entry_lo::v | entry_lo::g);
        s.cpu.mtc0(cp0::entry_hi, 0x8000);
        s.cpu.mtc0(cp0::index, 1);
        s.cpu.tlb_write_indexed();

        // Instructions are words, so they are stored in the little-endian lane order too.
        const std::uint32_t program[] = {lw(t0, 0, a0), lbu(t1, 0, a0), lhu(t2, 0, a0),
                                         sb(t1, 5, a0)};
        for (unsigned i = 0; i < 4; ++i) {
            s.poke((System::code + 4 * std::uint64_t{i}) ^ 4, AccessWidth::bits32, program[i]);
        }
        s.poke(data, AccessWidth::bits64, 0x1122'3344'5566'7788);
        s.set(a0, data);
        s.cpu.mtc0(cp0::status, status::re | (2u << status::ksu_shift));
        s.jump(System::code);
        s.interpreter.run(4);
        // Little-endian view of the doubleword 11 22 33 44 55 66 77 88: byte 0 is 0x88.
        t.check_equal(s.gpr(t0), 0x5566'7788u);
        t.check_equal(s.gpr(t1), std::uint64_t{0x88});
        t.check_equal(s.gpr(t2), std::uint64_t{0x7788});
        // Little-endian byte 5 is big-endian byte 2.
        t.check_equal(s.peek(data + 2, AccessWidth::bits8), std::uint64_t{0x88});
    }};

const test::Registration xkphys_decoding{"mips.xkphys_decoding", [](test::Context& t) {
                                             System s;
                                             s.poke(data, AccessWidth::bits32, 0x600d);
                                             // Cacheable coherent (CCA 5) xkphys with bits 58:40
                                             // clear is fine.
                                             s.set(a0, 0xa800'0000'0000'0000 | data);
                                             // Cacheable with a bit in 58:40 set is an address
                                             // error (UM 16.2, Figure 16-4).
                                             s.set(a1, 0xa800'0100'0000'0000 | data);
                                             s.run_program({lw(t0, 0, a0), lw(t1, 0, a1)});
                                             t.check_equal(s.gpr(t0), std::uint64_t{0x600d});
                                             t.check_equal(s.exception_code(), std::uint32_t{4});
                                             t.check_equal(s.cpu.dmfc0(cp0::bad_vaddr),
                                                           0xa800'0100'0000'0000u | data);
                                         }};

// Records the system address of every access it sees.
class Probe final : public MmioTarget {
  public:
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth) override {
        last_offset = offset;
        return 0x1234;
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth,
                                                std::uint64_t) override {
        last_offset = offset;
        return {};
    }
    std::uint64_t last_offset{};
};

const test::Registration uncached_attribute{
    "mips.uncached_attribute_reaches_bus", [](test::Context& t) {
        // Uncached xkphys references carry VA[58:57] as the uncached attribute, which Ultraviolent
        // places in the system address beside the uncached-request bit (UM 6.23, ADR-019).
        System s;
        Probe probe;
        const std::uint64_t window = system_address::uncached_window(2) | 0x0010'0000'0000;
        if (!t.check(s.bus.map_mmio({PhysicalAddress{window}, 0x1000}, probe).has_value())) {
            return;
        }
        s.set(a0, 0x9000'0000'0000'0000 | (std::uint64_t{2} << 57) | 0x0010'0000'0040);
        s.run_program({ld(t0, 0, a0)});
        t.check_equal(s.gpr(t0), std::uint64_t{0x1234});
        t.check_equal(probe.last_offset, std::uint64_t{0x40});
    }};

// Records the system address of every read.
class AddressRecorder final : public MmioTarget {
  public:
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth) override {
        offsets.push_back(offset);
        return 0;
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t, AccessWidth,
                                                std::uint64_t) override {
        return {};
    }
    std::vector<std::uint64_t> offsets;
};

const test::Registration request_kinds{
    "mips.system_address_distinguishes_request_kinds", [](test::Context& t) {
        // The same physical address reached as cached, uncached (attribute 0), and uncached with
        // attribute 3 produces three distinct system addresses (ADR-019).
        VirtualClock clock;
        Tracer tracer{clock};
        AddressSpace bus{ByteOrder::big};
        AddressRecorder recorder;
        // One window covering every system address this test produces.
        if (!t.check(
                bus.map_mmio({PhysicalAddress{0}, std::uint64_t{1} << 60}, recorder).has_value())) {
            return;
        }
        Cpu cpu{bus, tracer, CpuConfig{}};
        cpu.mtc0(cp0::status, status::kx);
        cpu.mtc0(cp0::config, config_big_endian | 3); // kseg0 cacheable noncoherent
        for (const std::uint64_t address :
             {kseg0(0x40), kseg1(0x40), 0x9600'0000'0000'0040u, 0xa800'0000'0000'0040u}) {
            t.check(cpu.load(address, AccessWidth::bits64).has_value());
        }
        const std::uint64_t attribute3 = system_address::uncached_window(3);
        t.check(recorder.offsets == std::vector<std::uint64_t>{0x40,
                                                               system_address::uncached | 0x40,
                                                               attribute3 | 0x40, 0x40},
                "cached, uncached attribute 0, uncached attribute 3, cached");
    }};

const test::Registration bus_error_on_read{
    "mips.unmapped_read_is_bus_error", [](test::Context& t) {
        System s;
        s.set(a0, kseg1(0x0800'0000)); // no memory there
        s.run_program({lw(t0, 0, a0)});
        t.check_equal(s.exception_code(), std::uint32_t{7}); // DBE
        t.check_equal(s.cpu.dmfc0(cp0::epc), kseg0(System::code));
    }};

const test::Registration lost_write{"mips.unmapped_write_takes_no_exception", [](test::Context& t) {
                                        // Only reads take bus errors on the R10000 (UM 17 "Bus
                                        // Error Exception").
                                        System s;
                                        s.set(a0, kseg1(0x0800'0000));
                                        s.run_program({sw(zero, 0, a0), addiu(t0, zero, 1)});
                                        t.check_equal(s.gpr(t0), std::uint64_t{1});
                                        t.check_equal(s.pc(), kseg0(System::code + 8));
                                    }};

const test::Registration user_data_upper_bits{
    "mips.user32_data_ignores_upper_bits", [](test::Context& t) {
        // In 32-bit User mode, data references clear the upper 32 address bits before checking
        // (UM Table 16-2 note), but instruction fetches do not.
        System s;
        s.cpu.mtc0(cp0::entry_hi, 0);
        s.cpu.mtc0(cp0::entry_lo0, (0 << 6) | (3 << 3) | entry_lo::v | entry_lo::d | entry_lo::g);
        s.cpu.mtc0(cp0::entry_lo1, (1 << 6) | (3 << 3) | entry_lo::v | entry_lo::d | entry_lo::g);
        s.cpu.mtc0(cp0::index, 0);
        s.cpu.tlb_write_indexed();
        s.poke(0x0100, AccessWidth::bits32, 77);
        s.load(System::code, {lw(t0, 0, a0)});
        s.set(a0, 0x1234'5678'0000'0100);
        s.cpu.mtc0(cp0::status, 2u << status::ksu_shift);
        s.jump(System::code);
        s.interpreter.run(1);
        t.check_equal(s.gpr(t0), std::uint64_t{77});

        s.jump(0x0000'0001'0000'1000);
        s.interpreter.run(1);
        t.check_equal(s.exception_code(), std::uint32_t{4}); // AdEL on the fetch
    }};

} // namespace
