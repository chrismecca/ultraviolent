#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/fpu.hpp>

#include <bit>
#include <cstdint>
#include <limits>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using testing::general_vector;
using testing::kseg0;
using testing::System;

constexpr std::uint32_t fpu_on = status::kx | status::cu1 | status::fr;

constexpr std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}
constexpr std::uint64_t bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

std::uint32_t cause_of(const System& s) {
    return (s.cpu.fpu().fcsr >> fcsr::cause_shift) & 0x3f;
}
std::uint32_t flags_of(const System& s) {
    return (s.cpu.fpu().fcsr >> fcsr::flag_shift) & 0x1f;
}
bool took_fpe(const System& s) {
    return s.pc() == general_vector &&
           s.exception_code() == static_cast<std::uint32_t>(ExceptionCode::floating_point);
}

constexpr double infinity = std::numeric_limits<double>::infinity();

// Runs `words` with the even f-registers preloaded, in order, from `fprs` (FR=1).
void run_fpu(System& s, std::initializer_list<std::uint64_t> fprs,
             std::initializer_list<std::uint32_t> words, std::uint32_t fcsr_value = 0,
             std::uint32_t extra_status = fpu_on) {
    unsigned n = 0;
    for (const std::uint64_t value : fprs) {
        s.cpu.fpu().fpr[n] = value;
        n += 2;
    }
    s.cpu.fpu().fcsr = fcsr_value;
    s.run_program(words, extra_status);
}

const test::Registration moves_fr1{
    "mips.fpu.moves_32_registers", [](test::Context& t) {
        System s;
        s.set(a0, 0x1122'3344'8899'aabb);
        s.set(a1, 0x7777'0000);
        s.cpu.fpu().fpr[2] = 0xdead'beef'dead'beef;
        s.run_program({dmtc1(a0, 1), mfc1(t0, 1), mtc1(a1, 2), dmfc1(t1, 2)}, fpu_on);
        t.check_equal(s.cpu.fpu().fpr[1], 0x1122'3344'8899'aabbu);
        t.check_equal(s.gpr(t0), 0xffff'ffff'8899'aabbu); // MFC1 sign-extends
        // With FR=1 the R10000 clears the upper half on MTC1 (UM 15.5).
        t.check_equal(s.gpr(t1), std::uint64_t{0x7777'0000});
    }};

const test::Registration moves_fr0{"mips.fpu.moves_16_registers", [](test::Context& t) {
                                       // FR=0: odd logical registers are the upper halves of the
                                       // even registers (UM 15.3).
                                       System s;
                                       s.set(a0, 0x0102'0304);
                                       s.set(a1, 0x0506'0708);
                                       s.run_program(
                                           {mtc1(a0, 0), mtc1(a1, 1), dmfc1(t0, 0), mfc1(t1, 1)},
                                           status::kx | status::cu1);
                                       t.check_equal(s.gpr(t0), 0x0506'0708'0102'0304u);
                                       t.check_equal(s.gpr(t1), std::uint64_t{0x0506'0708});
                                       t.check_equal(s.cpu.fpu().fpr[1], std::uint64_t{0});
                                   }};

const test::Registration loads_and_stores{
    "mips.fpu.loads_and_stores", [](test::Context& t) {
        System s;
        s.poke(System::data, AccessWidth::bits64, 0x4000'0000'0000'0000);
        s.poke(System::data + 8, AccessWidth::bits32, 0x3f80'0000);
        s.set(a0, kseg0(System::data));
        s.set(a1, 16);
        s.run_program({ldc1(2, 0, a0), lwc1(4, 8, a0), sdc1(2, 24, a0), swc1(4, 32, a0),
                       ldxc1(6, a1, a0), sdxc1(6, a1, a0)},
                      fpu_on);
        t.check_equal(s.cpu.fpu().fpr[2], bits(2.0));
        t.check_equal(s.cpu.fpu().fpr[4], std::uint64_t{0x3f80'0000});
        t.check_equal(s.peek(System::data + 24, AccessWidth::bits64), bits(2.0));
        t.check_equal(s.peek(System::data + 32, AccessWidth::bits32), std::uint64_t{0x3f80'0000});
    }};

const test::Registration rounding_modes{
    "mips.fpu.rounding_modes", [](test::Context& t) {
        // 1/3 rounds down to ...555 to nearest and up to ...556 toward +infinity.
        System nearest;
        run_fpu(nearest, {bits(1.0), bits(3.0)}, {div_fmt(fmt_d, 4, 0, 2)}, 0);
        t.check_equal(nearest.cpu.fpu().fpr[4], std::uint64_t{0x3fd5'5555'5555'5555});
        System up;
        run_fpu(up, {bits(1.0), bits(3.0)}, {div_fmt(fmt_d, 4, 0, 2)}, 2);
        t.check_equal(up.cpu.fpu().fpr[4], std::uint64_t{0x3fd5'5555'5555'5556});
        System single_down;
        run_fpu(single_down, {bits(-1.0f), bits(3.0f)}, {div_fmt(fmt_s, 4, 0, 2)}, 1);
        // Toward zero, -1/3 keeps the smaller magnitude (nearest would give ...aab).
        t.check_equal(single_down.cpu.fpu().fpr[4], std::uint64_t{0xbeaa'aaaa});
    }};

const test::Registration cause_and_flags{
    "mips.fpu.cause_and_sticky_flags", [](test::Context& t) {
        System s;
        run_fpu(s, {bits(1.0), bits(3.0)}, {div_fmt(fmt_d, 4, 0, 2), add_fmt(fmt_d, 6, 0, 0)});
        // The exact add leaves Cause empty but the inexact flag from the divide remains.
        t.check_equal(cause_of(s), std::uint32_t{0});
        t.check_equal(flags_of(s), fcsr::inexact);
        t.check_equal(s.cpu.fpu().fpr[6], bits(2.0));
    }};

const test::Registration divide_by_zero{
    "mips.fpu.divide_by_zero", [](test::Context& t) {
        System quiet;
        run_fpu(quiet, {bits(1.0), bits(0.0)}, {div_fmt(fmt_d, 4, 0, 2)});
        t.check_equal(quiet.cpu.fpu().fpr[4], bits(infinity));
        t.check_equal(flags_of(quiet), fcsr::divide_by_zero);

        // Enabled: the exception is taken, the result is not stored, and flags do not change.
        const std::uint32_t enable_z = fcsr::divide_by_zero << fcsr::enable_shift;
        System trapped;
        run_fpu(trapped, {bits(1.0), bits(0.0), bits(7.0)}, {div_fmt(fmt_d, 4, 0, 2)}, enable_z);
        t.check(took_fpe(trapped), "floating-point exception");
        t.check_equal(trapped.cpu.fpu().fpr[4], bits(7.0));
        t.check_equal(cause_of(trapped), fcsr::divide_by_zero);
        t.check_equal(flags_of(trapped), std::uint32_t{0});
    }};

const test::Registration nan_rules{
    "mips.fpu.nan_rules", [](test::Context& t) {
        // MIPS signaling NaNs have the fraction MSB set (ISA Table B-2); default NaNs are
        // 7fbfffff and 7ff7ffffffffffff (Table B-3).
        System signaling;
        run_fpu(signaling, {0x7fc0'0000, bits(1.0f)}, {add_fmt(fmt_s, 4, 0, 2)});
        t.check_equal(signaling.cpu.fpu().fpr[4], std::uint64_t{0x7fbf'ffff});
        t.check_equal(cause_of(signaling), fcsr::invalid);

        System quiet;
        run_fpu(quiet, {bits(1.0f), 0x7f80'0001}, {mul_fmt(fmt_s, 4, 0, 2)});
        t.check_equal(quiet.cpu.fpu().fpr[4], std::uint64_t{0x7f80'0001});
        t.check_equal(cause_of(quiet), std::uint32_t{0});

        System invalid;
        run_fpu(invalid, {bits(infinity)}, {sub_fmt(fmt_d, 4, 0, 0)});
        t.check_equal(invalid.cpu.fpu().fpr[4], std::uint64_t{0x7ff7'ffff'ffff'ffff});
        t.check_equal(cause_of(invalid), fcsr::invalid);

        // MOV is not arithmetic: it copies a signaling NaN without signaling.
        System moved;
        run_fpu(moved, {0x7fc0'0000}, {mov_fmt(fmt_s, 4, 0)});
        t.check_equal(moved.cpu.fpu().fpr[4], std::uint64_t{0x7fc0'0000});
        t.check_equal(cause_of(moved), std::uint32_t{0});
    }};

const test::Registration denormal_results{
    "mips.fpu.denormal_results", [](test::Context& t) {
        // 2^-1022 * 0.5 is exactly the denormal 2^-1023. With FS=0 the R10000 takes an
        // Unimplemented Operation exception; with FS=1 the result is flushed to zero with no
        // exception flagged (UM 15.4 "Flush (FS) bit").
        const std::uint64_t min_normal = 0x0010'0000'0000'0000;
        System trapped;
        run_fpu(trapped, {min_normal, bits(0.5), bits(9.0)}, {mul_fmt(fmt_d, 4, 0, 2)});
        t.check(took_fpe(trapped), "unimplemented operation");
        t.check_equal(cause_of(trapped), fcsr::unimplemented);
        t.check_equal(trapped.cpu.fpu().fpr[4], bits(9.0));

        System flushed;
        run_fpu(flushed, {min_normal | (std::uint64_t{1} << 63), bits(0.5)},
                {mul_fmt(fmt_d, 4, 0, 2)}, fcsr::flush);
        t.check_equal(flushed.cpu.fpu().fpr[4], std::uint64_t{1} << 63); // -0
        t.check_equal(cause_of(flushed), std::uint32_t{0});
    }};

const test::Registration conversions{
    "mips.fpu.conversions", [](test::Context& t) {
        System s;
        run_fpu(
            s, {0xffff'fffb, bits(2.5), bits(3.5), bits(-2.7), bits(1.1f), bits(-1.1), bits(3e9)},
            {cvt_d(fmt_w, 16, 0), cvt_w(fmt_d, 18, 2), round_w(fmt_d, 20, 4), trunc_w(fmt_d, 22, 6),
             ceil_w(fmt_s, 24, 8), floor_w(fmt_d, 26, 10), cvt_w(fmt_d, 28, 12)});
        const auto& f = s.cpu.fpu().fpr;
        t.check_equal(f[16], bits(-5.0));
        t.check_equal(f[18], std::uint64_t{2}); // round to nearest even
        t.check_equal(f[20], std::uint64_t{4});
        t.check_equal(f[22], std::uint64_t{0xffff'fffe});
        t.check_equal(f[24], std::uint64_t{2});
        t.check_equal(f[26], std::uint64_t{0xffff'fffe});
        t.check_equal(f[28], std::uint64_t{0x7fff'ffff}); // out of range: invalid, 2^31-1
        t.check((flags_of(s) & fcsr::invalid) != 0, "invalid flagged");

        System single;
        run_fpu(single, {bits(1.0 / 3.0)}, {cvt_s(fmt_d, 4, 0), cvt_d(fmt_s, 6, 4)});
        t.check_equal(single.cpu.fpu().fpr[4], bits(static_cast<float>(1.0 / 3.0)));
        t.check_equal(single.cpu.fpu().fpr[6],
                      bits(static_cast<double>(static_cast<float>(1.0 / 3.0))));
    }};

const test::Registration long_conversions{
    "mips.fpu.long_conversions_limited_to_51_bits", [](test::Context& t) {
        // UM 15.5: the R10000 converts to longword only within 51 bits.
        System inside;
        run_fpu(inside, {bits(-1125899906842623.0)}, {cvt_l(fmt_d, 4, 0)});
        t.check_equal(inside.cpu.fpu().fpr[4], static_cast<std::uint64_t>(-1125899906842623LL));
        System outside;
        run_fpu(outside, {bits(4503599627370496.0)}, {cvt_l(fmt_d, 4, 0)});
        t.check(took_fpe(outside), "unimplemented beyond 51 bits");
        t.check_equal(cause_of(outside), fcsr::unimplemented);
    }};

const test::Registration compare_and_branch{
    "mips.fpu.compare_and_branch", [](test::Context& t) {
        System s;
        s.cpu.fpu().fpr[0] = bits(1.0);
        s.cpu.fpu().fpr[2] = bits(2.0);
        s.cpu.fpu().fpr[4] = 0x7ff0'0000'0000'0001; // quiet NaN (MIPS sense)
        s.load(System::code,
               {c_cond(fmt_d, cond_olt, 0, 0, 2), bc1t(0, 8), nop(), addiu(t0, zero, 1),
                c_cond(fmt_d, cond_eq, 3, 0, 2), movt(t1, a0, 3), movf(t2, a0, 3),
                c_cond(fmt_d, cond_un, 1, 0, 4), c_cond(fmt_d, cond_ngle, 2, 0, 4)});
        s.set(a0, 77);
        s.start_kernel(kseg0(System::code), fpu_on);
        // c.olt, bc1t, delay slot, c.eq, movt, movf, c.un
        s.interpreter.run(7);
        t.check_equal(s.gpr(t0), std::uint64_t{0});  // branch taken over the addiu
        t.check_equal(s.gpr(t1), std::uint64_t{0});  // cc3 false: MOVT does not move
        t.check_equal(s.gpr(t2), std::uint64_t{77}); // MOVF does
        const std::uint32_t csr = s.cpu.fpu().fcsr;
        t.check((csr & fcsr::condition0) != 0, "cc0 set by 1 < 2");
        t.check((csr & (1u << 27)) == 0, "cc3 clear: 1 != 2");
        t.check((csr & (1u << 25)) != 0, "cc1 set: unordered");
        t.check_equal(cause_of(s), std::uint32_t{0}); // C.UN with a quiet NaN does not signal
        s.interpreter.run(1);
        t.check_equal(cause_of(s), fcsr::invalid); // C.NGLE signals on unordered operands
    }};

const test::Registration conditional_moves{
    "mips.fpu.conditional_moves", [](test::Context& t) {
        System s;
        s.cpu.fpu().fpr[0] = bits(1.5);
        s.cpu.fpu().fpr[2] = 0xaaaa'aaaa'4000'0000;
        s.cpu.fpu().fpr[4] = 0xbbbb'bbbb'0000'0000;
        s.set(a0, 1);
        s.run_program({movz_fmt(fmt_d, 6, 0, zero), movn_fmt(fmt_s, 4, 2, zero)}, fpu_on);
        t.check_equal(s.cpu.fpu().fpr[6], bits(1.5));
        // No move, but the R10000 clears the upper half of a single destination (UM 15.5).
        t.check_equal(s.cpu.fpu().fpr[4], std::uint64_t{0});
    }};

const test::Registration multiply_add{"mips.fpu.multiply_add_is_not_fused", [](test::Context& t) {
                                          // x = 1 + 2^-27: x*x = 1 + 2^-26 + 2^-54 rounds to 1 +
                                          // 2^-26, so x*x - (1 + 2^-26) is 0 when the product is
                                          // rounded first and 2^-54 when fused.
                                          const double x = 1.0 + 0x1p-27;
                                          System s;
                                          run_fpu(s, {bits(x), bits(1.0 + 0x1p-26)},
                                                  {msub_fmt(fmt_d, 4, 2, 0, 0),
                                                   madd_fmt(fmt_d, 6, 2, 0, 0),
                                                   nmadd_fmt(fmt_d, 8, 2, 0, 0)});
                                          t.check_equal(s.cpu.fpu().fpr[4], bits(0.0));
                                          t.check_equal(s.cpu.fpu().fpr[6], bits(2.0 + 0x1p-25));
                                          t.check_equal(s.cpu.fpu().fpr[8], bits(-(2.0 + 0x1p-25)));
                                      }};

const test::Registration sign_and_root{
    "mips.fpu.sqrt_abs_neg", [](test::Context& t) {
        System s;
        run_fpu(s, {bits(2.0), bits(-3.0), bits(4.0f)},
                {sqrt_fmt(fmt_d, 6, 0), abs_fmt(fmt_d, 8, 2), neg_fmt(fmt_s, 10, 4),
                 recip_fmt(fmt_d, 12, 0), rsqrt_fmt(fmt_s, 14, 4)});
        t.check_equal(s.cpu.fpu().fpr[6], std::uint64_t{0x3ff6'a09e'667f'3bcd});
        t.check_equal(s.cpu.fpu().fpr[8], bits(3.0));
        t.check_equal(s.cpu.fpu().fpr[10], bits(-4.0f));
        t.check_equal(s.cpu.fpu().fpr[12], bits(0.5));
        t.check_equal(s.cpu.fpu().fpr[14], bits(0.5f));
    }};

const test::Registration control_registers{
    "mips.fpu.control_registers", [](test::Context& t) {
        System s;
        s.set(a0, (fcsr::invalid << fcsr::cause_shift) | (fcsr::invalid << fcsr::enable_shift) |
                      (0x1fu << 18) | 1);
        s.run_program({cfc1(t0, 0), ctc1(a0, 31)}, fpu_on);
        t.check_equal(s.gpr(t0), std::uint64_t{0x0900});
        // A CTC1 that sets a Cause bit and its Enable traps after writing FCSR (UM 15.4); bits
        // 22:18 read as zero.
        t.check(took_fpe(s), "trap on CTC1");
        t.check_equal(s.cpu.fpu().fcsr, (fcsr::invalid << fcsr::cause_shift) |
                                            (fcsr::invalid << fcsr::enable_shift) | 1u);
        t.check_equal(s.cpu.dmfc0(cp0::epc), kseg0(System::code + 4));
    }};

const test::Registration user_mips2{
    "mips.fpu.user_mips2_restrictions", [](test::Context& t) {
        // In 32-bit User mode the L format is unimplemented and DMFC1 is reserved (UM 17.6).
        System s;
        s.map_user_low();
        s.run_user_program({cvt_l(fmt_d, 2, 0)}, status::cu1 | status::fr);
        t.check(took_fpe(s), "L format unimplemented");
        t.check_equal(cause_of(s), fcsr::unimplemented);
        s.run_user_program({dmfc1(t0, 0)}, status::cu1 | status::fr);
        t.check_equal(s.exception_code(),
                      static_cast<std::uint32_t>(ExceptionCode::reserved_instruction));
        // Without CU1 every FPU instruction is unusable.
        s.run_user_program({mfc1(t0, 0)}, 0);
        t.check_equal(s.exception_code(),
                      static_cast<std::uint32_t>(ExceptionCode::coprocessor_unusable));
    }};

} // namespace
