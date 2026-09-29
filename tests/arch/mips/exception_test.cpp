#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <ultraviolent/core/interrupt.hpp>

#include <cstdint>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using testing::general_vector;
using testing::kseg0;
using testing::System;

constexpr std::uint32_t code_of(ExceptionCode code) {
    return static_cast<std::uint32_t>(code);
}

const test::Registration overflow{
    "mips.overflow_leaves_destination", [](test::Context& t) {
        System s;
        s.set(a0, 0x7fff'ffff);
        s.set(t0, 99);
        s.run_program({addi(t0, a0, 1)});
        t.check_equal(s.gpr(t0), std::uint64_t{99});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::overflow));
        t.check_equal(s.pc(), general_vector);
        t.check_equal(s.cpu.dmfc0(cp0::epc), kseg0(System::code));
        t.check((s.cpu.mfc0(cp0::status) & status::exl) != 0, "EXL set");
        t.check((s.cpu.mfc0(cp0::cause) & cause::bd) == 0, "not in a delay slot");
    }};

const test::Registration doubleword_overflow{
    "mips.doubleword_overflow", [](test::Context& t) {
        System s;
        s.set(a0, 0x7fff'ffff'ffff'ffff);
        s.set(a1, 0x8000'0000'0000'0000);
        s.set(a2, 1);
        s.run_program({dadd(t0, a0, a2)});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::overflow));
        System d;
        d.set(a1, 0x8000'0000'0000'0000);
        d.set(a2, 1);
        d.run_program({dsub(t0, a1, a2)});
        t.check_equal(d.exception_code(), code_of(ExceptionCode::overflow));
    }};

const test::Registration delay_slot_exception{
    "mips.exception_in_delay_slot_reports_branch", [](test::Context& t) {
        System s;
        s.set(a0, 0x7fff'ffff);
        s.load(System::code, {nop(), beq(zero, zero, 16), addi(t0, a0, 1)});
        s.start_kernel();
        s.interpreter.run(3);
        // EPC is the branch and Cause.BD is set (UM 14.12).
        t.check_equal(s.cpu.dmfc0(cp0::epc), kseg0(System::code + 4));
        t.check((s.cpu.mfc0(cp0::cause) & cause::bd) != 0, "BD set");
    }};

const test::Registration nested_exception{
    "mips.exception_with_exl_keeps_epc", [](test::Context& t) {
        System s;
        s.load(System::code, {syscall()});
        s.cpu.mtc0(cp0::epc, 0x1234'5678);
        s.start_kernel(kseg0(System::code), status::kx | status::exl);
        s.interpreter.run(1);
        t.check_equal(s.cpu.dmfc0(cp0::epc), std::uint64_t{0x1234'5678});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::syscall));
        t.check_equal(s.pc(), general_vector);
    }};

const test::Registration boot_vectors{
    "mips.bev_selects_boot_vectors", [](test::Context& t) {
        System s;
        s.load(System::code, {break_(3)});
        s.start_kernel(kseg0(System::code), status::kx | status::bev);
        s.interpreter.run(1);
        t.check_equal(s.pc(), 0xffff'ffff'bfc0'0380u);
        t.check_equal(s.exception_code(), code_of(ExceptionCode::breakpoint));
    }};

const test::Registration traps{
    "mips.traps", [](test::Context& t) {
        const struct {
            std::uint32_t word;
            bool traps;
        } cases[] = {
            {teq(a0, a1), false},  {tne(a0, a1), true},  {tlt(a0, a1), true},
            {tltu(a0, a1), false}, {tge(a0, a1), false}, {tgeu(a0, a1), true},
            {teqi(a0, -1), true},  {tlti(a0, 0), true},  {tltiu(a0, 0), false},
        };
        for (const auto& c : cases) {
            System s;
            s.set(a0, ~std::uint64_t{0}); // -1
            s.set(a1, 1);
            s.run_program({c.word});
            t.check_equal(s.pc() == general_vector, c.traps);
        }
    }};

const test::Registration reserved_instruction{
    "mips.reserved_instructions", [](test::Context& t) {
        // Undefined opcode 0x1c, undefined SPECIAL function 5, undefined REGIMM 4, BC0F (UM 14.25).
        for (const std::uint32_t word : {0x7000'0000u, 0x0000'0005u, 0x0404'0000u, 0x4100'0000u}) {
            System s;
            s.run_program({word});
            t.check_equal(s.exception_code(), code_of(ExceptionCode::reserved_instruction));
        }
    }};

const test::Registration user_mode_gates{
    "mips.user_mode_gates", [](test::Context& t) {
        // In 32-bit User mode: 64-bit operations are reserved, CP0 is unusable, MIPS IV needs XX
        // (UM 16.1, 17 "Reserved Instruction", "Coprocessor Unusable").
        System s;
        s.cpu.mtc0(cp0::entry_hi, 0);
        s.cpu.mtc0(cp0::entry_lo0, (3 << 3) | entry_lo::v | entry_lo::d | entry_lo::g);
        s.cpu.mtc0(cp0::entry_lo1, (1 << 6) | (3 << 3) | entry_lo::v | entry_lo::d | entry_lo::g);
        s.cpu.mtc0(cp0::index, 0);
        s.cpu.tlb_write_indexed();
        const struct {
            std::uint32_t word;
            std::uint32_t status;
            ExceptionCode code;
            std::uint64_t ce;
        } cases[] = {
            {daddu(t0, t0, t0), 0, ExceptionCode::reserved_instruction, 0},
            {mfc0(t0, cp0::status), 0, ExceptionCode::coprocessor_unusable, 0},
            {movn(t0, t1, t2), 0, ExceptionCode::reserved_instruction, 0},
            {add_fmt(fmt_d, 0, 0, 0), status::xx, ExceptionCode::coprocessor_unusable, 1},
            {lwc2(t0, 0, zero), 0, ExceptionCode::coprocessor_unusable, 2},
            {lwc2(t0, 0, zero), status::cu2, ExceptionCode::reserved_instruction, 0},
        };
        for (const auto& c : cases) {
            s.load(System::code, {c.word});
            s.cpu.mtc0(cp0::status, (2u << status::ksu_shift) | c.status);
            s.jump(System::code);
            s.interpreter.run(1);
            t.check_equal(s.exception_code(), code_of(c.code));
            if (c.code == ExceptionCode::coprocessor_unusable) {
                t.check_equal((s.cpu.mfc0(cp0::cause) & cause::ce_mask) >> cause::ce_shift, c.ce);
            }
        }
        // With UX and XX set, the same 64-bit and MIPS IV instructions execute.
        s.load(System::code, {daddu(t0, zero, zero), movn(t0, t1, zero)});
        s.cpu.mtc0(cp0::status, (2u << status::ksu_shift) | status::ux | status::xx);
        s.jump(System::code);
        s.interpreter.run(2);
        t.check_equal(s.pc(), std::uint64_t{System::code + 8});
    }};

const test::Registration address_errors{
    "mips.address_errors", [](test::Context& t) {
        System s;
        s.set(a0, kseg0(System::data) + 2);
        s.run_program({lw(t0, 0, a0)});
        t.check_equal(s.exception_code(), code_of(ExceptionCode::address_error_load));
        t.check_equal(s.cpu.dmfc0(cp0::bad_vaddr), kseg0(System::data) + 2);

        System store;
        store.set(a0, kseg0(System::data) + 1);
        store.run_program({sh(t0, 0, a0)});
        t.check_equal(store.exception_code(), code_of(ExceptionCode::address_error_store));

        // A misaligned jump target faults on the fetch, with EPC at the target.
        System jump;
        jump.set(a0, kseg0(System::code) + 0x102);
        jump.run_program({jr(a0), nop(), nop()});
        t.check_equal(jump.exception_code(), code_of(ExceptionCode::address_error_load));
        t.check_equal(jump.cpu.dmfc0(cp0::epc), kseg0(System::code) + 0x102);

        // Kernel with KX clear cannot reach xkphys.
        System narrow;
        narrow.set(a0, 0x9000'0000'0000'0000);
        narrow.run_program({lw(t0, 0, a0)}, 0);
        t.check_equal(narrow.exception_code(), code_of(ExceptionCode::address_error_load));
    }};

const test::Registration eret_returns{
    "mips.eret", [](test::Context& t) {
        System s;
        s.set(a0, kseg0(System::code + 0x40));
        s.load(System::code, {dmtc0(a0, cp0::epc), eret(), addiu(t0, zero, 1)});
        s.load(System::code + 0x40, {addiu(t1, zero, 1)});
        s.start_kernel(kseg0(System::code), status::kx | status::exl);
        s.interpreter.run(3);
        t.check_equal(s.gpr(t0), std::uint64_t{0}); // ERET has no delay slot
        t.check_equal(s.gpr(t1), std::uint64_t{1});
        t.check((s.cpu.mfc0(cp0::status) & status::exl) == 0, "EXL cleared");

        // With ERL set, ERET uses ErrorEPC and clears ERL only.
        System e;
        e.set(a0, kseg0(System::code + 0x40));
        e.load(System::code, {dmtc0(a0, cp0::error_epc), eret()});
        e.start_kernel(kseg0(System::code), status::kx | status::exl | status::erl);
        e.interpreter.run(2);
        t.check_equal(e.pc(), kseg0(System::code + 0x40));
        const std::uint64_t sr = e.cpu.mfc0(cp0::status);
        t.check((sr & status::erl) == 0 && (sr & status::exl) != 0, "ERL cleared, EXL kept");
    }};

const test::Registration cp0_moves{
    "mips.cp0_move_widths", [](test::Context& t) {
        System s;
        s.set(a0, 0x0000'0001'8000'0000);
        s.set(a1, 0x0000'0000'0000'0003);
        s.run_program({dmtc0(a0, cp0::epc), mfc0(t0, cp0::epc), dmfc0(t1, cp0::epc),
                       mtc0(a0, cp0::epc), dmfc0(t2, cp0::epc), mtc0(a1, cp0::cause),
                       mfc0(t3, cp0::cause), mtc0(a0, cp0::entry_hi), dmfc0(t4, cp0::entry_hi),
                       dmfc0(t5, cp0::prid)});
        // UM Table 14-26.
        t.check_equal(s.gpr(t0), 0xffff'ffff'8000'0000u); // MFC0 sign-extends bit 31
        t.check_equal(s.gpr(t1), 0x0000'0001'8000'0000u);
        t.check_equal(s.gpr(t2), 0x0000'0001'8000'0000u); // MTC0 to a 64-bit register: rt63..0
        t.check_equal(s.gpr(t3), std::uint64_t{0});       // only IP[1:0] are writable
        // EntryHi keeps R, VPN2 43:13, and ASID of the full value.
        t.check_equal(s.gpr(t4), 0x0000'0001'8000'0000u);
        t.check_equal(s.gpr(t5), std::uint64_t{0x0900}); // 32-bit register zero-extended by DMFC0
    }};

const test::Registration software_interrupt{
    "mips.software_interrupt", [](test::Context& t) {
        System s;
        s.set(a0, 1u << cause::ip_shift);
        s.set(a1, status::kx | status::ie | (1u << status::im_shift));
        s.load(System::code, {mtc0(a0, cp0::cause), nop(), mtc0(a1, cp0::status), nop(), nop()});
        s.start_kernel();
        s.interpreter.run(3);
        t.check_equal(s.pc(), kseg0(System::code + 12)); // pending but not yet enabled
        s.interpreter.run(1);
        t.check_equal(s.pc(), general_vector);
        t.check_equal(s.exception_code(), code_of(ExceptionCode::interrupt));
        // EPC is the instruction that had not executed.
        t.check_equal(s.cpu.dmfc0(cp0::epc), kseg0(System::code + 12));
    }};

const test::Registration external_interrupt{
    "mips.external_interrupt_line", [](test::Context& t) {
        System s;
        InterruptLine line;
        line.connect(s.cpu, 3); // IP5
        s.load(System::code, {nop(), nop(), nop(), nop()});
        s.start_kernel(kseg0(System::code),
                       status::kx | status::ie | (1u << (status::im_shift + 5)));
        s.interpreter.run(1);
        line.raise();
        t.check((s.cpu.mfc0(cp0::cause) >> cause::ip_shift & 0xff) == 0x20, "IP5 pending");
        s.interpreter.run(1);
        t.check_equal(s.pc(), general_vector);
        // While EXL is set the interrupt is not taken again.
        s.load(0x180, {nop(), nop()});
        s.interpreter.run(2);
        t.check_equal(s.pc(), general_vector + 8);
        line.lower();
        t.check((s.cpu.mfc0(cp0::cause) >> cause::ip_shift & 0xff) == 0, "IP5 clear");
    }};

const test::Registration timer_interrupt{
    "mips.count_compare_timer", [](test::Context& t) {
        System s;
        s.set(a0, 0);
        s.set(a1, 5);
        s.load(System::code, {mtc0(a0, cp0::count), mtc0(a1, cp0::compare)});
        for (unsigned i = 0; i < 32; ++i) {
            s.load(System::code + 8 + 4 * std::uint64_t{i}, {nop()});
        }
        s.start_kernel(kseg0(System::code), status::kx | status::ie | (0x80u << status::im_shift));
        s.interpreter.run(2);
        const std::uint64_t count = s.cpu.mfc0(cp0::count);
        // Count advances every other cycle (UM 14.8).
        s.interpreter.run(4);
        t.check_equal(s.cpu.mfc0(cp0::count), count + 2);
        // Count reaches Compare=5 after ten cycles; the interrupt is taken at the next boundary.
        s.interpreter.run(16);
        t.check_equal(s.exception_code(), code_of(ExceptionCode::interrupt));
        t.check((s.cpu.mfc0(cp0::cause) & (0x80u << cause::ip_shift)) != 0, "IP7 set");
        // Writing Compare clears IP7.
        s.cpu.mtc0(cp0::compare, 0);
        t.check((s.cpu.mfc0(cp0::cause) & (0x80u << cause::ip_shift)) == 0, "IP7 cleared");
    }};

const test::Registration watch{"mips.watch_exception", [](test::Context& t) {
                                   System s;
                                   s.cpu.mtc0(cp0::watch_lo, System::data | 1); // trap on stores
                                   s.cpu.mtc0(cp0::watch_hi, 0);
                                   s.set(a0, kseg0(System::data) + 4);
                                   s.run_program({lw(t0, 0, a0), sw(t0, 0, a0)});
                                   t.check_equal(s.exception_code(), code_of(ExceptionCode::watch));
                                   t.check_equal(s.cpu.dmfc0(cp0::epc), kseg0(System::code + 4));

                                   // With EXL set, the reference completes and the exception waits
                                   // for EXL to clear.
                                   System d;
                                   d.cpu.mtc0(cp0::watch_lo, System::data | 2); // trap on loads
                                   d.set(a0, kseg0(System::data));
                                   d.set(a1, status::kx);
                                   d.load(System::code,
                                          {lw(t0, 0, a0), mtc0(a1, cp0::status), nop()});
                                   d.start_kernel(kseg0(System::code), status::kx | status::exl);
                                   d.interpreter.run(2);
                                   t.check_equal(d.pc(), kseg0(System::code + 8));
                                   d.interpreter.run(1);
                                   t.check_equal(d.exception_code(), code_of(ExceptionCode::watch));
                                   t.check_equal(d.cpu.dmfc0(cp0::epc), kseg0(System::code + 8));
                               }};

const test::Registration soft_reset{
    "mips.soft_reset_preserves_state", [](test::Context& t) {
        System s;
        s.run_program({addiu(s0, zero, 42), nop()});
        s.cpu.reset(ResetKind::warm);
        // UM 17 "Soft Reset Exception".
        t.check_equal(s.gpr(s0), std::uint64_t{42});
        t.check_equal(s.pc(), Cpu::reset_vector);
        t.check_equal(s.cpu.dmfc0(cp0::error_epc), kseg0(System::code + 8));
        const std::uint64_t sr = s.cpu.mfc0(cp0::status);
        t.check((sr & (status::erl | status::sr | status::bev)) ==
                    (status::erl | status::sr | status::bev),
                "ERL, SR, BEV set");
        t.check((sr & status::nmi) == 0, "NMI clear on soft reset");

        s.cpu.nonmaskable_interrupt();
        t.check((s.cpu.mfc0(cp0::status) & status::nmi) != 0, "NMI set");

        s.cpu.reset(ResetKind::cold);
        t.check_equal(s.gpr(s0), std::uint64_t{0});
        t.check((s.cpu.mfc0(cp0::status) & status::sr) == 0, "SR clear on cold reset");
    }};

} // namespace
