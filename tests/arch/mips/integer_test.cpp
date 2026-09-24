#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <cstdint>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using testing::kseg0;
using testing::System;

constexpr std::uint64_t u(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

const test::Registration reset_state{
    "mips.reset_state", [](test::Context& t) {
        System s{CpuConfig{.processor_id = 0x0926, .config = config_big_endian | 3}};
        // UM 17 "Cold Reset Exception".
        t.check_equal(s.pc(), Cpu::reset_vector);
        const std::uint64_t sr = s.cpu.mfc0(cp0::status);
        t.check((sr & status::erl) != 0 && (sr & status::bev) != 0, "ERL and BEV set");
        t.check((sr & (status::sr | status::ts)) == 0, "SR and TS clear");
        t.check_equal(s.cpu.mfc0(cp0::random), std::uint64_t{63});
        t.check_equal(s.cpu.mfc0(cp0::wired), std::uint64_t{0});
        t.check_equal(s.cpu.mfc0(cp0::prid), std::uint64_t{0x0926});
        t.check_equal(s.cpu.mfc0(cp0::config), std::uint64_t{config_big_endian | 3});
        t.check(s.cpu.mode() == OperatingMode::kernel);
    }};

const test::Registration executes_from_reset_vector{
    "mips.executes_from_reset_vector", [](test::Context& t) {
        System s;
        // The reset vector is kseg1: unmapped and uncached, physical 0x1fc00000.
        s.load(System::boot_base, {lui(t0, 0x1234), ori(t0, t0, 0x5678)});
        s.interpreter.run(2);
        t.check_equal(s.gpr(t0), std::uint64_t{0x1234'5678});
        t.check_equal(s.pc(), Cpu::reset_vector + 8);
    }};

const test::Registration register_zero{"mips.register_zero_is_constant", [](test::Context& t) {
                                           System s;
                                           s.run_program(
                                               {addiu(zero, zero, 5), or_(t0, zero, zero)});
                                           t.check_equal(s.gpr(zero), std::uint64_t{0});
                                           t.check_equal(s.gpr(t0), std::uint64_t{0});
                                       }};

const test::Registration word_results_sign_extend{
    "mips.word_results_are_sign_extended", [](test::Context& t) {
        System s;
        s.set(a0, 0x7fff'ffff);
        s.set(a1, 1);
        s.run_program({addu(t0, a0, a1), lui(t1, 0x8000), addiu(t2, zero, -1), sll(t3, a1, 31),
                       srl(t4, t1, 0), subu(t5, zero, a1)});
        t.check_equal(s.gpr(t0), u(-2147483648LL)); // 0x7fffffff + 1 wraps and sign-extends
        t.check_equal(s.gpr(t1), 0xffff'ffff'8000'0000u);
        t.check_equal(s.gpr(t2), ~std::uint64_t{0});
        t.check_equal(s.gpr(t3), 0xffff'ffff'8000'0000u);
        t.check_equal(s.gpr(t4), 0xffff'ffff'8000'0000u); // SRL by 0 still sign-extends bit 31
        t.check_equal(s.gpr(t5), ~std::uint64_t{0});
    }};

const test::Registration logical_and_compare{
    "mips.logical_and_compare", [](test::Context& t) {
        System s;
        s.set(a0, 0xf0f0);
        s.set(a1, 0x0ff0);
        s.set(a2, u(-5));
        s.run_program({and_(t0, a0, a1), or_(t1, a0, a1), xor_(t2, a0, a1), nor(t3, a0, a1),
                       andi(t4, a2, 0xffff), ori(t5, zero, 0x8000), xori(t6, a0, 0xffff),
                       slt(t7, a2, a0), sltu(s0, a2, a0), slti(s1, a2, -4), sltiu(s2, a0, -1)});
        t.check_equal(s.gpr(t0), std::uint64_t{0x00f0});
        t.check_equal(s.gpr(t1), std::uint64_t{0xfff0});
        t.check_equal(s.gpr(t2), std::uint64_t{0xff00});
        t.check_equal(s.gpr(t3), ~std::uint64_t{0xfff0});
        t.check_equal(s.gpr(t4), std::uint64_t{0xfffb}); // ANDI zero-extends its immediate
        t.check_equal(s.gpr(t5), std::uint64_t{0x8000});
        t.check_equal(s.gpr(t6), std::uint64_t{0x0f0f});
        t.check_equal(s.gpr(t7), std::uint64_t{1}); // -5 < 0xf0f0 signed
        t.check_equal(s.gpr(s0), std::uint64_t{0}); // but not unsigned
        t.check_equal(s.gpr(s1), std::uint64_t{1}); // -5 < -4
        t.check_equal(s.gpr(s2), std::uint64_t{1}); // SLTIU sign-extends then compares unsigned
    }};

const test::Registration shifts{
    "mips.shifts", [](test::Context& t) {
        System s;
        s.set(a0, 0x8000'0000'0000'0001);
        s.set(a1, 0xffff'ffff'8000'0000);
        s.set(a2, 33); // variable word shifts use the low 5 bits, doubleword shifts the low 6
        s.run_program({sra(t0, a1, 4), srl(t1, a1, 4), sllv(t2, a0, a2), srav(t3, a1, a2),
                       dsll(t4, a0, 1), dsrl(t5, a0, 1), dsra(t6, a0, 1), dsll32(t7, a0, 0),
                       dsrl32(s0, a0, 31), dsra32(s1, a0, 31), dsllv(s2, a0, a2),
                       dsrav(s3, a0, a2)});
        t.check_equal(s.gpr(t0), 0xffff'ffff'f800'0000u);
        t.check_equal(s.gpr(t1), std::uint64_t{0x0800'0000});
        t.check_equal(s.gpr(t2), std::uint64_t{2});
        t.check_equal(s.gpr(t3), 0xffff'ffff'c000'0000u);
        t.check_equal(s.gpr(t4), std::uint64_t{2});
        t.check_equal(s.gpr(t5), 0x4000'0000'0000'0000u);
        t.check_equal(s.gpr(t6), 0xc000'0000'0000'0000u);
        t.check_equal(s.gpr(t7), 0x0000'0001'0000'0000u);
        t.check_equal(s.gpr(s0), std::uint64_t{1});
        t.check_equal(s.gpr(s1), ~std::uint64_t{0});
        t.check_equal(s.gpr(s2), 0x0000'0002'0000'0000u);
        t.check_equal(s.gpr(s3), 0xffff'ffff'c000'0000u);
    }};

const test::Registration doubleword_arithmetic{
    "mips.doubleword_arithmetic", [](test::Context& t) {
        System s;
        s.set(a0, 0x0000'0001'ffff'ffff);
        s.run_program(
            {daddiu(t0, a0, 1), daddu(t1, a0, a0), dsubu(t2, zero, a0), daddi(t3, a0, -1)});
        t.check_equal(s.gpr(t0), 0x0000'0002'0000'0000u);
        t.check_equal(s.gpr(t1), 0x0000'0003'ffff'fffeu);
        t.check_equal(s.gpr(t2), u(-0x1'ffff'ffffLL));
        t.check_equal(s.gpr(t3), 0x0000'0001'ffff'fffeu);
    }};

const test::Registration multiply{
    "mips.multiply", [](test::Context& t) {
        System s;
        s.set(a0, u(-3));
        s.set(a1, 0x7fff'ffff);
        s.run_program({mult(a0, a1), mflo(t0), mfhi(t1), multu(a0, a1), mflo(t2), mfhi(t3)});
        // -3 * (2^31 - 1) = -6442450941 = 0xfffffffe_80000003
        t.check_equal(s.gpr(t0), 0xffff'ffff'8000'0003u);
        t.check_equal(s.gpr(t1), 0xffff'ffff'ffff'fffeu);
        // 0xfffffffd * 0x7fffffff = (2^32 - 3)(2^31 - 1) = 0x7ffffffd_80000003
        t.check_equal(s.gpr(t2), 0xffff'ffff'8000'0003u);
        t.check_equal(s.gpr(t3), std::uint64_t{0x7fff'fffd});
    }};

const test::Registration doubleword_multiply{
    "mips.doubleword_multiply", [](test::Context& t) {
        System s;
        s.set(a0, 0xffff'ffff'ffff'ffff);
        s.set(a1, 0x1234'5678'9abc'def0);
        s.run_program({dmultu(a0, a1), mflo(t0), mfhi(t1), dmult(a0, a1), mflo(t2), mfhi(t3)});
        // (2^64 - 1) * x = x * 2^64 - x
        t.check_equal(s.gpr(t0), u(-0x1234'5678'9abc'def0LL));
        t.check_equal(s.gpr(t1), 0x1234'5678'9abc'deefu);
        // -1 * x
        t.check_equal(s.gpr(t2), u(-0x1234'5678'9abc'def0LL));
        t.check_equal(s.gpr(t3), ~std::uint64_t{0});
    }};

const test::Registration divide{
    "mips.divide", [](test::Context& t) {
        System s;
        s.set(a0, u(-7));
        s.set(a1, 2);
        s.set(a2, 0xffff'ffff'8000'0000); // INT32_MIN
        s.set(a3, u(-1));
        s.run_program({div(a0, a1), mflo(t0), mfhi(t1), divu(a1, a1), mflo(t2), mfhi(t3),
                       div(a2, a3), mflo(t4), mfhi(t5), ddiv(a0, a1), mflo(t6), mfhi(t7)});
        t.check_equal(s.gpr(t0), u(-3)); // truncates toward zero
        t.check_equal(s.gpr(t1), u(-1)); // remainder has the dividend's sign
        t.check_equal(s.gpr(t2), std::uint64_t{1});
        t.check_equal(s.gpr(t3), std::uint64_t{0});
        t.check_equal(s.gpr(t4), 0xffff'ffff'8000'0000u); // overflow wraps, no exception
        t.check_equal(s.gpr(t5), std::uint64_t{0});
        t.check_equal(s.gpr(t6), u(-3));
        t.check_equal(s.gpr(t7), u(-1));
    }};

const test::Registration divide_by_zero_keeps_hi_lo{
    "mips.divide_by_zero_keeps_hi_lo", [](test::Context& t) {
        // hypothesis (MIPS.adoc): the ISA leaves the result UNPREDICTABLE; Ultraviolent keeps
        // HI and LO. No exception is taken.
        System s;
        s.set(a0, 5);
        s.set(a1, 11);
        s.run_program({mthi(a0), mtlo(a1), div(a0, zero), ddivu(a0, zero)});
        t.check_equal(s.cpu.state().hi, std::uint64_t{5});
        t.check_equal(s.cpu.state().lo, std::uint64_t{11});
        t.check_equal(s.pc(), kseg0(System::code + 16));
    }};

const test::Registration conditional_moves{"mips.conditional_moves", [](test::Context& t) {
                                               System s;
                                               s.set(a0, 42);
                                               s.set(t0, 1);
                                               s.set(t1, 1);
                                               s.run_program(
                                                   {movz(t0, a0, zero), movn(t1, a0, zero)});
                                               t.check_equal(s.gpr(t0), std::uint64_t{42});
                                               t.check_equal(s.gpr(t1), std::uint64_t{1});
                                           }};

const test::Registration delay_slot_executes{
    "mips.branch_delay_slot", [](test::Context& t) {
        System s;
        s.load(System::code,
               {beq(zero, zero, 8), addiu(t0, zero, 1), addiu(t1, zero, 1), addiu(t2, zero, 1)});
        s.start_kernel();
        s.interpreter.run(3);
        t.check_equal(s.gpr(t0), std::uint64_t{1}); // delay slot ran
        t.check_equal(s.gpr(t1), std::uint64_t{0}); // skipped by the branch
        t.check_equal(s.gpr(t2), std::uint64_t{1}); // branch target
    }};

const test::Registration branch_not_taken{
    "mips.branch_not_taken", [](test::Context& t) {
        System s;
        s.set(a0, 1);
        s.load(System::code, {beq(a0, zero, 8), addiu(t0, zero, 1), addiu(t1, zero, 1)});
        s.start_kernel();
        s.interpreter.run(3);
        t.check_equal(s.gpr(t0), std::uint64_t{1});
        t.check_equal(s.gpr(t1), std::uint64_t{1});
    }};

const test::Registration branch_likely_nullifies{
    "mips.branch_likely_nullifies_delay_slot", [](test::Context& t) {
        System s;
        s.set(a0, 1);
        s.load(System::code, {beql(a0, zero, 8), addiu(t0, zero, 1), addiu(t1, zero, 1),
                              beql(zero, zero, 4), addiu(t2, zero, 1), nop(), addiu(t3, zero, 1)});
        s.start_kernel();
        // beql (not taken), addiu t1, beql (taken), addiu t2, nop, addiu t3
        s.interpreter.run(6);
        t.check_equal(s.gpr(t0), std::uint64_t{0}); // not taken: delay slot nullified
        t.check_equal(s.gpr(t1), std::uint64_t{1});
        t.check_equal(s.gpr(t2), std::uint64_t{1}); // taken: delay slot runs
        t.check_equal(s.gpr(t3), std::uint64_t{1});
    }};

const test::Registration regimm_branches{"mips.regimm_branches_and_links", [](test::Context& t) {
                                             System s;
                                             s.set(a0, u(-1));
                                             s.load(System::code,
                                                    {bltzal(a0, 8), nop(), addiu(t0, zero, 1),
                                                     bgezal(a0, 8), nop(), addiu(t1, zero, 1)});
                                             s.start_kernel();
                                             s.interpreter.run(5);
                                             t.check_equal(s.gpr(t0), std::uint64_t{0});
                                             t.check_equal(s.gpr(t1), std::uint64_t{1});
                                             // BGEZAL writes the link even though it is not taken.
                                             t.check_equal(s.gpr(ra), kseg0(System::code + 12 + 8));
                                         }};

const test::Registration jumps_and_links{"mips.jumps_and_links", [](test::Context& t) {
                                             System s;
                                             const std::uint64_t target =
                                                 kseg0(System::code + 0x100);
                                             s.set(a0, kseg0(System::code + 0x200));
                                             s.load(System::code, {jal(target), nop()});
                                             s.load(System::code + 0x100, {jalr(t9, a0), nop()});
                                             s.load(System::code + 0x200, {jr(ra), nop()});
                                             s.start_kernel();
                                             s.interpreter.run(2);
                                             t.check_equal(s.pc(), target);
                                             t.check_equal(s.gpr(ra), kseg0(System::code + 8));
                                             s.interpreter.run(2);
                                             t.check_equal(s.pc(), kseg0(System::code + 0x200));
                                             t.check_equal(s.gpr(t9), target + 8);
                                             s.interpreter.run(2);
                                             t.check_equal(s.pc(), kseg0(System::code + 8));
                                         }};

const test::Registration jump_region{"mips.jump_keeps_upper_pc_bits", [](test::Context& t) {
                                         System s;
                                         // J replaces bits 27:0 of the delay slot address.
                                         s.load(System::code, {j(0x0000'0000'0000'0200), nop()});
                                         s.start_kernel();
                                         s.interpreter.run(2);
                                         t.check_equal(s.pc(), 0xffff'ffff'8000'0200u);
                                     }};

const test::Registration random_decrements{
    "mips.random_stays_between_wired_and_63", [](test::Context& t) {
        System s;
        s.set(a0, 60);
        s.load(System::code, {mtc0(a0, cp0::wired), nop(), nop(), nop(), nop(), nop(), nop(), nop(),
                              nop(), nop()});
        s.start_kernel();
        // Random decrements as instructions graduate and wraps from Wired back to 63 (UM 14.2).
        std::uint64_t previous = 64;
        bool wrapped = false;
        for (int i = 0; i < 10; ++i) {
            s.interpreter.step();
            const std::uint64_t random = s.cpu.mfc0(cp0::random);
            t.check(random >= 60 && random <= 63, "Random within [Wired, 63]");
            if (previous != 64 && random != previous - 1) {
                t.check(previous == 60 && random == 63, "wraps only from Wired to 63");
                wrapped = true;
            }
            previous = random;
        }
        t.check(wrapped, "wrapped at least once");
    }};

} // namespace
