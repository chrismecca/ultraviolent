#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <cstdint>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using testing::general_vector;
using testing::kseg0;
using testing::refill_vector;
using testing::System;
using testing::xrefill_vector;

constexpr std::uint32_t code_of(ExceptionCode code) {
    return static_cast<std::uint32_t>(code);
}

constexpr std::uint64_t lo(std::uint64_t pfn, bool dirty = true, bool valid = true,
                           bool global = false, unsigned coherency = 3) {
    return (pfn << entry_lo::pfn_shift) | (std::uint64_t{coherency} << entry_lo::c_shift) |
           (dirty ? entry_lo::d : 0) | (valid ? entry_lo::v : 0) | (global ? entry_lo::g : 0);
}

void write_entry(System& s, unsigned index, std::uint64_t hi, std::uint64_t lo0, std::uint64_t lo1,
                 std::uint64_t mask = 0) {
    s.cpu.dmtc0(cp0::entry_hi, hi);
    s.cpu.dmtc0(cp0::entry_lo0, lo0);
    s.cpu.dmtc0(cp0::entry_lo1, lo1);
    s.cpu.mtc0(cp0::page_mask, mask);
    s.cpu.mtc0(cp0::index, index);
    s.cpu.tlb_write_indexed();
}

const test::Registration mapped_load{
    "mips.tlb_mapped_load_and_store", [](test::Context& t) {
        System s;
        // Kernel-mapped kseg3 page pair at 0xffffffffe0000000 -> physical 0x8000/0x9000.
        write_entry(s, 5, 0xffff'ffff'e000'0000, lo(8, false), lo(9), 0);
        s.poke(0x8010, AccessWidth::bits32, 0x1357'9bdf);
        s.set(a0, 0xffff'ffff'e000'0010);
        s.set(a1, 0xffff'ffff'e000'1020);
        s.run_program({lw(t0, 0, a0), sw(t0, 0, a1)});
        t.check_equal(s.gpr(t0), std::uint64_t{0x1357'9bdf});
        t.check_equal(s.peek(0x9020, AccessWidth::bits32), std::uint64_t{0x1357'9bdf});
    }};

const test::Registration refill{
    "mips.tlb_refill_fills_registers", [](test::Context& t) {
        System s;
        s.cpu.mtc0(cp0::entry_hi, 0x42); // current ASID
        s.cpu.dmtc0(cp0::context, 0xffff'ffff'c000'0000);
        s.cpu.dmtc0(cp0::xcontext, 0xffff'ffe0'0000'0000);
        s.set(a0, 0x0000'0123'4567'8000); // xkuseg, needs UX for the 64-bit space
        s.run_program({lw(t0, 0, a0)}, status::kx | status::ux);
        t.check_equal(s.exception_code(), code_of(ExceptionCode::tlb_load));
        // User address with UX set: the XTLB refill vector (UM 17.3).
        t.check_equal(s.pc(), xrefill_vector);
        t.check_equal(s.cpu.dmfc0(cp0::bad_vaddr), 0x0000'0123'4567'8000u);
        const std::uint64_t vpn2 = 0x0000'0123'4567'8000u >> 13;
        // EntryHi keeps its ASID; Context gets VA 31:13; XContext gets R and VA 43:13 (UM 14.4,
        // 14.9, 14.17).
        t.check_equal(s.cpu.dmfc0(cp0::entry_hi), (vpn2 << 13) | 0x42);
        t.check_equal(s.cpu.dmfc0(cp0::context), 0xffff'ffff'c000'0000u | ((vpn2 & 0x7'ffff) << 4));
        t.check_equal(s.cpu.dmfc0(cp0::xcontext), 0xffff'ffe0'0000'0000u | (vpn2 << 4));
    }};

const test::Registration refill_vector_selection{
    "mips.tlb_refill_vector_selection", [](test::Context& t) {
        const struct {
            std::uint64_t address;
            std::uint32_t status;
            std::uint64_t vector;
        } cases[] = {
            {0x0000'0000'0040'0000, status::kx, refill_vector},               // user, UX=0
            {0x0000'0000'0040'0000, status::kx | status::ux, xrefill_vector}, // user, UX=1
            {0xffff'ffff'c000'0000, status::kx, xrefill_vector},              // ksseg follows KX
            {0xffff'ffff'e000'0000, 0, refill_vector},                        // kseg3, KX=0
            {0xc000'0000'0000'0000, status::kx, xrefill_vector},              // xkseg
            {0x4000'0000'0000'0000, status::kx | status::sx, xrefill_vector}, // xksseg
        };
        for (const auto& c : cases) {
            System s;
            s.set(a0, c.address);
            s.run_program({lw(t0, 0, a0)}, c.status);
            t.check_equal(s.pc(), c.vector);
        }
        // With EXL already set a refill uses the general vector.
        System s;
        s.set(a0, 0xffff'ffff'e000'0000);
        s.run_program({lw(t0, 0, a0)}, status::exl);
        t.check_equal(s.pc(), general_vector);
    }};

const test::Registration invalid_and_modified{
    "mips.tlb_invalid_and_modified", [](test::Context& t) {
        System s;
        write_entry(s, 0, 0xffff'ffff'e000'0000, lo(8, true, false), lo(9, false, true));
        s.set(a0, 0xffff'ffff'e000'0000);
        s.run_program({lw(t0, 0, a0)});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::tlb_load));
        t.check_equal(s.pc(), general_vector); // invalid is not a refill

        System m;
        write_entry(m, 0, 0xffff'ffff'e000'0000, lo(8, true, false), lo(9, false, true));
        m.set(a0, 0xffff'ffff'e000'1000);
        m.run_program({lw(t0, 0, a0), sw(t0, 0, a0)});
        t.check_equal(m.exception_code(), code_of(ExceptionCode::tlb_modified));
        t.check_equal(m.cpu.dmfc0(cp0::epc), kseg0(System::code + 4));
    }};

const test::Registration asid_and_global{
    "mips.tlb_asid_and_global", [](test::Context& t) {
        System s;
        write_entry(s, 0, 0xffff'ffff'e000'0000 | 7, lo(8), lo(9));
        write_entry(s, 1, 0xffff'ffff'e000'2000 | 7, lo(10, true, true, true),
                    lo(11, true, true, true));
        s.cpu.mtc0(cp0::entry_hi, 8); // a different ASID
        s.set(a0, 0xffff'ffff'e000'2000);
        s.set(a1, 0xffff'ffff'e000'0000);
        s.run_program({lw(t0, 0, a0), lw(t1, 0, a1)});
        // The global entry matches any ASID; the other does not.
        t.check_equal(s.cpu.dmfc0(cp0::epc), kseg0(System::code + 4));
        t.check_equal(s.pc(), xrefill_vector); // kseg3 refills follow KX, which is set
    }};

const test::Registration large_pages{"mips.tlb_large_pages", [](test::Context& t) {
                                         System s;
                                         // 16 KiB pages (MASK 0x3): the even page covers
                                         // 0x0000-0x3fff, the odd 0x4000-0x7fff.
                                         const std::uint64_t mask = 0x3u << 13;
                                         write_entry(s, 0, 0xffff'ffff'e000'0000, lo(0x10),
                                                     lo(0x20), mask);
                                         s.poke(0x10000 + 0x3ffc, AccessWidth::bits32, 1);
                                         s.poke(0x20000 + 0x0004, AccessWidth::bits32, 2);
                                         s.set(a0, 0xffff'ffff'e000'3ffc);
                                         s.set(a1, 0xffff'ffff'e000'4004);
                                         s.run_program({lw(t0, 0, a0), lw(t1, 0, a1)});
                                         t.check_equal(s.gpr(t0), std::uint64_t{1});
                                         t.check_equal(s.gpr(t1), std::uint64_t{2});
                                     }};

const test::Registration read_back{
    "mips.tlbr_reads_what_was_written", [](test::Context& t) {
        System s;
        s.cpu.mtc0(cp0::frame_mask, 0x1); // masks EntryLo bit 18 (PFN bit 12)
        const std::uint64_t mask = 0x3u << 13;
        // G is the AND of both G bits; PFN bits below a 16 KiB page are forced to zero; VPN2 bits
        // covered by the mask are cleared (UM 14.36, 14.18).
        write_entry(s, 9, 0xffff'ffff'e000'6000 | 3, lo(0x1003, true, true, true),
                    lo(0x2003, true, true, false), mask);
        s.cpu.mtc0(cp0::index, 9);
        s.cpu.tlb_read();
        t.check_equal(s.cpu.dmfc0(cp0::entry_hi), 0xc000'0fff'e000'0000u | 3);
        t.check_equal(s.cpu.dmfc0(cp0::entry_lo0), lo(0x0000, true, true, false));
        t.check_equal(s.cpu.dmfc0(cp0::entry_lo1), lo(0x2000, true, true, false));
        t.check_equal(s.cpu.mfc0(cp0::page_mask), mask);
    }};

const test::Registration probe{"mips.tlbp", [](test::Context& t) {
                                   System s;
                                   write_entry(s, 12, 0xffff'ffff'e000'4000 | 5, lo(8), lo(9));
                                   s.cpu.dmtc0(cp0::entry_hi, 0xffff'ffff'e000'4000 | 5);
                                   s.cpu.tlb_probe();
                                   t.check_equal(s.cpu.mfc0(cp0::index), std::uint64_t{12});
                                   s.cpu.dmtc0(cp0::entry_hi, 0xffff'ffff'e000'4000 | 6);
                                   s.cpu.tlb_probe();
                                   t.check_equal(s.cpu.mfc0(cp0::index) & 0x8000'0000,
                                                 std::uint64_t{0x8000'0000});
                               }};

const test::Registration write_random{"mips.tlbwr_uses_random", [](test::Context& t) {
                                          System s;
                                          s.cpu.dmtc0(cp0::entry_hi, 0xffff'ffff'e000'0000);
                                          s.cpu.dmtc0(cp0::entry_lo0, lo(8));
                                          s.cpu.dmtc0(cp0::entry_lo1, lo(9));
                                          s.cpu.mtc0(cp0::page_mask, 0);
                                          const std::uint64_t random = s.cpu.mfc0(cp0::random);
                                          s.cpu.tlb_write_random();
                                          s.cpu.tlb_probe();
                                          t.check_equal(s.cpu.mfc0(cp0::index), random);
                                      }};

const test::Registration conflicting_write{
    "mips.tlb_conflict_invalidates", [](test::Context& t) {
        System s;
        write_entry(s, 1, 0xffff'ffff'e000'0000, lo(8), lo(9));
        t.check((s.cpu.mfc0(cp0::status) & status::ts) == 0, "no conflict yet");
        // The same pair at another index: the old entry is invalidated and TS is set (UM 14.10).
        write_entry(s, 2, 0xffff'ffff'e000'0000, lo(10), lo(11));
        t.check((s.cpu.mfc0(cp0::status) & status::ts) != 0, "TS set");
        s.cpu.dmtc0(cp0::entry_hi, 0xffff'ffff'e000'0000);
        s.cpu.tlb_probe();
        t.check_equal(s.cpu.mfc0(cp0::index), std::uint64_t{2});
        // A later non-conflicting write clears TS.
        write_entry(s, 3, 0xffff'ffff'e001'0000, lo(12), lo(13));
        t.check((s.cpu.mfc0(cp0::status) & status::ts) == 0, "TS cleared");
    }};

const test::Registration erl_kuseg{"mips.erl_makes_kuseg_unmapped", [](test::Context& t) {
                                       System s;
                                       s.poke(0x0200, AccessWidth::bits32, 55);
                                       s.set(a0, 0x200);
                                       s.run_program({lw(t0, 0, a0)}, status::kx | status::erl);
                                       t.check_equal(s.gpr(t0), std::uint64_t{55});
                                   }};

const test::Registration fetch_after_remap{
    "mips.fetch_sees_remap_and_stores", [](test::Context& t) {
        // Instruction fetch must follow TLB changes and see code written by stores, whatever the
        // engine caches (MIPS.adoc "Execution and time").
        System s;
        const std::uint64_t code_va = 0xffff'ffff'e000'0000;
        s.load(0x8000, {addiu(t0, zero, 1), jr(ra), nop()});
        s.load(0x9000, {addiu(t0, zero, 2), jr(ra), nop()});
        write_entry(s, 0, code_va, lo(8), lo(9));
        s.set(ra, kseg0(System::code));
        s.load(System::code, {jal(0), nop()});
        s.start_kernel(code_va);
        s.interpreter.run(3);
        t.check_equal(s.gpr(t0), std::uint64_t{1});

        // Remap the even page to physical 0x9000 by rewriting only EntryLo0 and the entry, so
        // the TLB write itself must invalidate anything derived from the old translation.
        s.cpu.dmtc0(cp0::entry_lo0, lo(9));
        s.cpu.mtc0(cp0::index, 0);
        s.cpu.tlb_write_indexed();
        s.jump(code_va);
        s.interpreter.run(1);
        t.check_equal(s.gpr(t0), std::uint64_t{2});

        // A store into the code page is seen by the next fetch.
        s.poke(0x9000, AccessWidth::bits32, addiu(t0, zero, 3));
        s.jump(code_va);
        s.interpreter.run(1);
        t.check_equal(s.gpr(t0), std::uint64_t{3});
    }};

} // namespace
