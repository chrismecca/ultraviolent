#pragma once

// Instruction encoders for building synthetic test programs. Operand order follows assembler
// syntax. tools/check_mips_encodings.sh compares these encodings with GNU as
// (-march=r10000); run it after adding or changing an encoder.

#include <cstdint>

namespace ultraviolent::mips::assembler {

// Conventional register names (o32/n64 share these numbers).
enum Register : unsigned {
    zero, at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7,
    s0, s1, s2, s3, s4, s5, s6, s7, t8, t9, k0, k1, gp, sp, fp, ra,
};

constexpr std::uint32_t r_type(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
constexpr std::uint32_t i_type(unsigned op, unsigned rs, unsigned rt, std::int32_t immediate) {
    return (op << 26) | (rs << 21) | (rt << 16) | (static_cast<std::uint32_t>(immediate) & 0xffff);
}
// Branch offsets are in bytes from the delay slot, as in "beq a0, a1, .+4+offset".
constexpr std::uint32_t branch_type(unsigned op, unsigned rs, unsigned rt, std::int32_t offset) {
    return i_type(op, rs, rt, offset / 4);
}

// SPECIAL
constexpr std::uint32_t sll(unsigned rd, unsigned rt, unsigned sa) { return r_type(0, rt, rd, sa, 0); }
constexpr std::uint32_t srl(unsigned rd, unsigned rt, unsigned sa) { return r_type(0, rt, rd, sa, 2); }
constexpr std::uint32_t sra(unsigned rd, unsigned rt, unsigned sa) { return r_type(0, rt, rd, sa, 3); }
constexpr std::uint32_t sllv(unsigned rd, unsigned rt, unsigned rs) { return r_type(rs, rt, rd, 0, 4); }
constexpr std::uint32_t srlv(unsigned rd, unsigned rt, unsigned rs) { return r_type(rs, rt, rd, 0, 6); }
constexpr std::uint32_t srav(unsigned rd, unsigned rt, unsigned rs) { return r_type(rs, rt, rd, 0, 7); }
constexpr std::uint32_t jr(unsigned rs) { return r_type(rs, 0, 0, 0, 8); }
constexpr std::uint32_t jalr(unsigned rd, unsigned rs) { return r_type(rs, 0, rd, 0, 9); }
constexpr std::uint32_t movz(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 10); }
constexpr std::uint32_t movn(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 11); }
constexpr std::uint32_t movf(unsigned rd, unsigned rs, unsigned cc) { return r_type(rs, cc << 2, rd, 0, 1); }
constexpr std::uint32_t movt(unsigned rd, unsigned rs, unsigned cc) { return r_type(rs, (cc << 2) | 1, rd, 0, 1); }
constexpr std::uint32_t syscall(unsigned code = 0) { return (code << 6) | 12; }
// GNU as places a single break code in bits 25:16.
constexpr std::uint32_t break_(unsigned code = 0) { return (code << 16) | 13; }
constexpr std::uint32_t sync() { return 15; }
constexpr std::uint32_t mfhi(unsigned rd) { return r_type(0, 0, rd, 0, 16); }
constexpr std::uint32_t mthi(unsigned rs) { return r_type(rs, 0, 0, 0, 17); }
constexpr std::uint32_t mflo(unsigned rd) { return r_type(0, 0, rd, 0, 18); }
constexpr std::uint32_t mtlo(unsigned rs) { return r_type(rs, 0, 0, 0, 19); }
constexpr std::uint32_t dsllv(unsigned rd, unsigned rt, unsigned rs) { return r_type(rs, rt, rd, 0, 20); }
constexpr std::uint32_t dsrlv(unsigned rd, unsigned rt, unsigned rs) { return r_type(rs, rt, rd, 0, 22); }
constexpr std::uint32_t dsrav(unsigned rd, unsigned rt, unsigned rs) { return r_type(rs, rt, rd, 0, 23); }
constexpr std::uint32_t mult(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 24); }
constexpr std::uint32_t multu(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 25); }
constexpr std::uint32_t div(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 26); }
constexpr std::uint32_t divu(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 27); }
constexpr std::uint32_t dmult(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 28); }
constexpr std::uint32_t dmultu(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 29); }
constexpr std::uint32_t ddiv(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 30); }
constexpr std::uint32_t ddivu(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 31); }
constexpr std::uint32_t add(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 32); }
constexpr std::uint32_t addu(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 33); }
constexpr std::uint32_t sub(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 34); }
constexpr std::uint32_t subu(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 35); }
constexpr std::uint32_t and_(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 36); }
constexpr std::uint32_t or_(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 37); }
constexpr std::uint32_t xor_(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 38); }
constexpr std::uint32_t nor(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 39); }
constexpr std::uint32_t slt(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 42); }
constexpr std::uint32_t sltu(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 43); }
constexpr std::uint32_t dadd(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 44); }
constexpr std::uint32_t daddu(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 45); }
constexpr std::uint32_t dsub(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 46); }
constexpr std::uint32_t dsubu(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 47); }
constexpr std::uint32_t tge(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 48); }
constexpr std::uint32_t tgeu(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 49); }
constexpr std::uint32_t tlt(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 50); }
constexpr std::uint32_t tltu(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 51); }
constexpr std::uint32_t teq(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 52); }
constexpr std::uint32_t tne(unsigned rs, unsigned rt) { return r_type(rs, rt, 0, 0, 54); }
constexpr std::uint32_t dsll(unsigned rd, unsigned rt, unsigned sa) { return r_type(0, rt, rd, sa, 56); }
constexpr std::uint32_t dsrl(unsigned rd, unsigned rt, unsigned sa) { return r_type(0, rt, rd, sa, 58); }
constexpr std::uint32_t dsra(unsigned rd, unsigned rt, unsigned sa) { return r_type(0, rt, rd, sa, 59); }
constexpr std::uint32_t dsll32(unsigned rd, unsigned rt, unsigned sa) { return r_type(0, rt, rd, sa, 60); }
constexpr std::uint32_t dsrl32(unsigned rd, unsigned rt, unsigned sa) { return r_type(0, rt, rd, sa, 62); }
constexpr std::uint32_t dsra32(unsigned rd, unsigned rt, unsigned sa) { return r_type(0, rt, rd, sa, 63); }
constexpr std::uint32_t nop() { return 0; }

// REGIMM
constexpr std::uint32_t bltz(unsigned rs, std::int32_t offset) { return branch_type(1, rs, 0, offset); }
constexpr std::uint32_t bgez(unsigned rs, std::int32_t offset) { return branch_type(1, rs, 1, offset); }
constexpr std::uint32_t bltzl(unsigned rs, std::int32_t offset) { return branch_type(1, rs, 2, offset); }
constexpr std::uint32_t bgezl(unsigned rs, std::int32_t offset) { return branch_type(1, rs, 3, offset); }
constexpr std::uint32_t tgei(unsigned rs, std::int32_t immediate) { return i_type(1, rs, 8, immediate); }
constexpr std::uint32_t tgeiu(unsigned rs, std::int32_t immediate) { return i_type(1, rs, 9, immediate); }
constexpr std::uint32_t tlti(unsigned rs, std::int32_t immediate) { return i_type(1, rs, 10, immediate); }
constexpr std::uint32_t tltiu(unsigned rs, std::int32_t immediate) { return i_type(1, rs, 11, immediate); }
constexpr std::uint32_t teqi(unsigned rs, std::int32_t immediate) { return i_type(1, rs, 12, immediate); }
constexpr std::uint32_t tnei(unsigned rs, std::int32_t immediate) { return i_type(1, rs, 14, immediate); }
constexpr std::uint32_t bltzal(unsigned rs, std::int32_t offset) { return branch_type(1, rs, 16, offset); }
constexpr std::uint32_t bgezal(unsigned rs, std::int32_t offset) { return branch_type(1, rs, 17, offset); }
constexpr std::uint32_t bltzall(unsigned rs, std::int32_t offset) { return branch_type(1, rs, 18, offset); }
constexpr std::uint32_t bgezall(unsigned rs, std::int32_t offset) { return branch_type(1, rs, 19, offset); }

// Jumps take the full target address; only bits 27:2 are encoded.
constexpr std::uint32_t j(std::uint64_t target) { return (2u << 26) | ((target >> 2) & 0x03ff'ffff); }
constexpr std::uint32_t jal(std::uint64_t target) { return (3u << 26) | ((target >> 2) & 0x03ff'ffff); }

// Immediate and branch opcodes
constexpr std::uint32_t beq(unsigned rs, unsigned rt, std::int32_t offset) { return branch_type(4, rs, rt, offset); }
constexpr std::uint32_t bne(unsigned rs, unsigned rt, std::int32_t offset) { return branch_type(5, rs, rt, offset); }
constexpr std::uint32_t blez(unsigned rs, std::int32_t offset) { return branch_type(6, rs, 0, offset); }
constexpr std::uint32_t bgtz(unsigned rs, std::int32_t offset) { return branch_type(7, rs, 0, offset); }
constexpr std::uint32_t addi(unsigned rt, unsigned rs, std::int32_t immediate) { return i_type(8, rs, rt, immediate); }
constexpr std::uint32_t addiu(unsigned rt, unsigned rs, std::int32_t immediate) { return i_type(9, rs, rt, immediate); }
constexpr std::uint32_t slti(unsigned rt, unsigned rs, std::int32_t immediate) { return i_type(10, rs, rt, immediate); }
constexpr std::uint32_t sltiu(unsigned rt, unsigned rs, std::int32_t immediate) { return i_type(11, rs, rt, immediate); }
constexpr std::uint32_t andi(unsigned rt, unsigned rs, std::uint32_t immediate) { return i_type(12, rs, rt, static_cast<std::int32_t>(immediate)); }
constexpr std::uint32_t ori(unsigned rt, unsigned rs, std::uint32_t immediate) { return i_type(13, rs, rt, static_cast<std::int32_t>(immediate)); }
constexpr std::uint32_t xori(unsigned rt, unsigned rs, std::uint32_t immediate) { return i_type(14, rs, rt, static_cast<std::int32_t>(immediate)); }
constexpr std::uint32_t lui(unsigned rt, std::uint32_t immediate) { return i_type(15, 0, rt, static_cast<std::int32_t>(immediate)); }
constexpr std::uint32_t beql(unsigned rs, unsigned rt, std::int32_t offset) { return branch_type(20, rs, rt, offset); }
constexpr std::uint32_t bnel(unsigned rs, unsigned rt, std::int32_t offset) { return branch_type(21, rs, rt, offset); }
constexpr std::uint32_t blezl(unsigned rs, std::int32_t offset) { return branch_type(22, rs, 0, offset); }
constexpr std::uint32_t bgtzl(unsigned rs, std::int32_t offset) { return branch_type(23, rs, 0, offset); }
constexpr std::uint32_t daddi(unsigned rt, unsigned rs, std::int32_t immediate) { return i_type(24, rs, rt, immediate); }
constexpr std::uint32_t daddiu(unsigned rt, unsigned rs, std::int32_t immediate) { return i_type(25, rs, rt, immediate); }

// Loads and stores: operands are (rt, offset, base).
constexpr std::uint32_t ldl(unsigned rt, std::int32_t offset, unsigned base) { return i_type(26, base, rt, offset); }
constexpr std::uint32_t ldr(unsigned rt, std::int32_t offset, unsigned base) { return i_type(27, base, rt, offset); }
constexpr std::uint32_t lb(unsigned rt, std::int32_t offset, unsigned base) { return i_type(32, base, rt, offset); }
constexpr std::uint32_t lh(unsigned rt, std::int32_t offset, unsigned base) { return i_type(33, base, rt, offset); }
constexpr std::uint32_t lwl(unsigned rt, std::int32_t offset, unsigned base) { return i_type(34, base, rt, offset); }
constexpr std::uint32_t lw(unsigned rt, std::int32_t offset, unsigned base) { return i_type(35, base, rt, offset); }
constexpr std::uint32_t lbu(unsigned rt, std::int32_t offset, unsigned base) { return i_type(36, base, rt, offset); }
constexpr std::uint32_t lhu(unsigned rt, std::int32_t offset, unsigned base) { return i_type(37, base, rt, offset); }
constexpr std::uint32_t lwr(unsigned rt, std::int32_t offset, unsigned base) { return i_type(38, base, rt, offset); }
constexpr std::uint32_t lwu(unsigned rt, std::int32_t offset, unsigned base) { return i_type(39, base, rt, offset); }
constexpr std::uint32_t sb(unsigned rt, std::int32_t offset, unsigned base) { return i_type(40, base, rt, offset); }
constexpr std::uint32_t sh(unsigned rt, std::int32_t offset, unsigned base) { return i_type(41, base, rt, offset); }
constexpr std::uint32_t swl(unsigned rt, std::int32_t offset, unsigned base) { return i_type(42, base, rt, offset); }
constexpr std::uint32_t sw(unsigned rt, std::int32_t offset, unsigned base) { return i_type(43, base, rt, offset); }
constexpr std::uint32_t sdl(unsigned rt, std::int32_t offset, unsigned base) { return i_type(44, base, rt, offset); }
constexpr std::uint32_t sdr(unsigned rt, std::int32_t offset, unsigned base) { return i_type(45, base, rt, offset); }
constexpr std::uint32_t swr(unsigned rt, std::int32_t offset, unsigned base) { return i_type(46, base, rt, offset); }
constexpr std::uint32_t cache(unsigned operation, std::int32_t offset, unsigned base) { return i_type(47, base, operation, offset); }
constexpr std::uint32_t ll(unsigned rt, std::int32_t offset, unsigned base) { return i_type(48, base, rt, offset); }
constexpr std::uint32_t lwc1(unsigned ft, std::int32_t offset, unsigned base) { return i_type(49, base, ft, offset); }
constexpr std::uint32_t pref(unsigned hint, std::int32_t offset, unsigned base) { return i_type(51, base, hint, offset); }
constexpr std::uint32_t lld(unsigned rt, std::int32_t offset, unsigned base) { return i_type(52, base, rt, offset); }
constexpr std::uint32_t ldc1(unsigned ft, std::int32_t offset, unsigned base) { return i_type(53, base, ft, offset); }
constexpr std::uint32_t ld(unsigned rt, std::int32_t offset, unsigned base) { return i_type(55, base, rt, offset); }
constexpr std::uint32_t sc(unsigned rt, std::int32_t offset, unsigned base) { return i_type(56, base, rt, offset); }
constexpr std::uint32_t swc1(unsigned ft, std::int32_t offset, unsigned base) { return i_type(57, base, ft, offset); }
constexpr std::uint32_t scd(unsigned rt, std::int32_t offset, unsigned base) { return i_type(60, base, rt, offset); }
constexpr std::uint32_t sdc1(unsigned ft, std::int32_t offset, unsigned base) { return i_type(61, base, ft, offset); }
constexpr std::uint32_t sd(unsigned rt, std::int32_t offset, unsigned base) { return i_type(63, base, rt, offset); }
constexpr std::uint32_t lwc2(unsigned rt, std::int32_t offset, unsigned base) { return i_type(50, base, rt, offset); }

// COP0
constexpr std::uint32_t mfc0(unsigned rt, unsigned rd) { return 0x4000'0000u | (rt << 16) | (rd << 11); }
constexpr std::uint32_t dmfc0(unsigned rt, unsigned rd) { return 0x4020'0000u | (rt << 16) | (rd << 11); }
constexpr std::uint32_t mtc0(unsigned rt, unsigned rd) { return 0x4080'0000u | (rt << 16) | (rd << 11); }
constexpr std::uint32_t dmtc0(unsigned rt, unsigned rd) { return 0x40a0'0000u | (rt << 16) | (rd << 11); }
constexpr std::uint32_t tlbr() { return 0x4200'0001u; }
constexpr std::uint32_t tlbwi() { return 0x4200'0002u; }
constexpr std::uint32_t tlbwr() { return 0x4200'0006u; }
constexpr std::uint32_t tlbp() { return 0x4200'0008u; }
constexpr std::uint32_t eret() { return 0x4200'0018u; }

// COP1
constexpr std::uint32_t mfc1(unsigned rt, unsigned fs) { return 0x4400'0000u | (rt << 16) | (fs << 11); }
constexpr std::uint32_t dmfc1(unsigned rt, unsigned fs) { return 0x4420'0000u | (rt << 16) | (fs << 11); }
constexpr std::uint32_t cfc1(unsigned rt, unsigned fs) { return 0x4440'0000u | (rt << 16) | (fs << 11); }
constexpr std::uint32_t mtc1(unsigned rt, unsigned fs) { return 0x4480'0000u | (rt << 16) | (fs << 11); }
constexpr std::uint32_t dmtc1(unsigned rt, unsigned fs) { return 0x44a0'0000u | (rt << 16) | (fs << 11); }
constexpr std::uint32_t ctc1(unsigned rt, unsigned fs) { return 0x44c0'0000u | (rt << 16) | (fs << 11); }
constexpr std::uint32_t bc1(unsigned cc, bool likely, bool on_true, std::int32_t offset) {
    return 0x4500'0000u | (cc << 18) | (likely ? 1u << 17 : 0) | (on_true ? 1u << 16 : 0) |
           (static_cast<std::uint32_t>(offset / 4) & 0xffff);
}
constexpr std::uint32_t bc1f(unsigned cc, std::int32_t offset) { return bc1(cc, false, false, offset); }
constexpr std::uint32_t bc1t(unsigned cc, std::int32_t offset) { return bc1(cc, false, true, offset); }
constexpr std::uint32_t bc1fl(unsigned cc, std::int32_t offset) { return bc1(cc, true, false, offset); }
constexpr std::uint32_t bc1tl(unsigned cc, std::int32_t offset) { return bc1(cc, true, true, offset); }

enum Format : unsigned { fmt_s = 16, fmt_d = 17, fmt_w = 20, fmt_l = 21 };

constexpr std::uint32_t fp_op(Format fmt, unsigned funct, unsigned fd, unsigned fs, unsigned ft = 0) {
    return 0x4400'0000u | (unsigned{fmt} << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
constexpr std::uint32_t add_fmt(Format f, unsigned fd, unsigned fs, unsigned ft) { return fp_op(f, 0, fd, fs, ft); }
constexpr std::uint32_t sub_fmt(Format f, unsigned fd, unsigned fs, unsigned ft) { return fp_op(f, 1, fd, fs, ft); }
constexpr std::uint32_t mul_fmt(Format f, unsigned fd, unsigned fs, unsigned ft) { return fp_op(f, 2, fd, fs, ft); }
constexpr std::uint32_t div_fmt(Format f, unsigned fd, unsigned fs, unsigned ft) { return fp_op(f, 3, fd, fs, ft); }
constexpr std::uint32_t sqrt_fmt(Format f, unsigned fd, unsigned fs) { return fp_op(f, 4, fd, fs); }
constexpr std::uint32_t abs_fmt(Format f, unsigned fd, unsigned fs) { return fp_op(f, 5, fd, fs); }
constexpr std::uint32_t mov_fmt(Format f, unsigned fd, unsigned fs) { return fp_op(f, 6, fd, fs); }
constexpr std::uint32_t neg_fmt(Format f, unsigned fd, unsigned fs) { return fp_op(f, 7, fd, fs); }
constexpr std::uint32_t round_l(Format f, unsigned fd, unsigned fs) { return fp_op(f, 8, fd, fs); }
constexpr std::uint32_t trunc_l(Format f, unsigned fd, unsigned fs) { return fp_op(f, 9, fd, fs); }
constexpr std::uint32_t round_w(Format f, unsigned fd, unsigned fs) { return fp_op(f, 12, fd, fs); }
constexpr std::uint32_t trunc_w(Format f, unsigned fd, unsigned fs) { return fp_op(f, 13, fd, fs); }
constexpr std::uint32_t ceil_w(Format f, unsigned fd, unsigned fs) { return fp_op(f, 14, fd, fs); }
constexpr std::uint32_t floor_w(Format f, unsigned fd, unsigned fs) { return fp_op(f, 15, fd, fs); }
constexpr std::uint32_t movf_fmt(Format f, unsigned fd, unsigned fs, unsigned cc) { return fp_op(f, 17, fd, fs, cc << 2); }
constexpr std::uint32_t movt_fmt(Format f, unsigned fd, unsigned fs, unsigned cc) { return fp_op(f, 17, fd, fs, (cc << 2) | 1); }
constexpr std::uint32_t movz_fmt(Format f, unsigned fd, unsigned fs, unsigned rt) { return fp_op(f, 18, fd, fs, rt); }
constexpr std::uint32_t movn_fmt(Format f, unsigned fd, unsigned fs, unsigned rt) { return fp_op(f, 19, fd, fs, rt); }
constexpr std::uint32_t recip_fmt(Format f, unsigned fd, unsigned fs) { return fp_op(f, 21, fd, fs); }
constexpr std::uint32_t rsqrt_fmt(Format f, unsigned fd, unsigned fs) { return fp_op(f, 22, fd, fs); }
constexpr std::uint32_t cvt_s(Format f, unsigned fd, unsigned fs) { return fp_op(f, 32, fd, fs); }
constexpr std::uint32_t cvt_d(Format f, unsigned fd, unsigned fs) { return fp_op(f, 33, fd, fs); }
constexpr std::uint32_t cvt_w(Format f, unsigned fd, unsigned fs) { return fp_op(f, 36, fd, fs); }
constexpr std::uint32_t cvt_l(Format f, unsigned fd, unsigned fs) { return fp_op(f, 37, fd, fs); }
// C.cond.fmt cc, fs, ft
constexpr std::uint32_t c_cond(Format f, unsigned condition, unsigned cc, unsigned fs, unsigned ft) {
    return fp_op(f, 48 | condition, cc << 2, fs, ft);
}
inline constexpr unsigned cond_f = 0, cond_un = 1, cond_eq = 2, cond_ueq = 3, cond_olt = 4,
                          cond_ult = 5, cond_ole = 6, cond_ule = 7, cond_sf = 8, cond_ngle = 9,
                          cond_seq = 10, cond_ngl = 11, cond_lt = 12, cond_nge = 13, cond_le = 14,
                          cond_ngt = 15;

// COP1X
constexpr std::uint32_t cop1x(unsigned rs, unsigned rt, unsigned fs, unsigned fd, unsigned funct) {
    return 0x4c00'0000u | (rs << 21) | (rt << 16) | (fs << 11) | (fd << 6) | funct;
}
constexpr std::uint32_t lwxc1(unsigned fd, unsigned index, unsigned base) { return cop1x(base, index, 0, fd, 0); }
constexpr std::uint32_t ldxc1(unsigned fd, unsigned index, unsigned base) { return cop1x(base, index, 0, fd, 1); }
constexpr std::uint32_t swxc1(unsigned fs, unsigned index, unsigned base) { return cop1x(base, index, fs, 0, 8); }
constexpr std::uint32_t sdxc1(unsigned fs, unsigned index, unsigned base) { return cop1x(base, index, fs, 0, 9); }
constexpr std::uint32_t madd_fmt(Format f, unsigned fd, unsigned fr, unsigned fs, unsigned ft) { return cop1x(fr, ft, fs, fd, (4u << 3) | (f == fmt_d ? 1u : 0u)); }
constexpr std::uint32_t msub_fmt(Format f, unsigned fd, unsigned fr, unsigned fs, unsigned ft) { return cop1x(fr, ft, fs, fd, (5u << 3) | (f == fmt_d ? 1u : 0u)); }
constexpr std::uint32_t nmadd_fmt(Format f, unsigned fd, unsigned fr, unsigned fs, unsigned ft) { return cop1x(fr, ft, fs, fd, (6u << 3) | (f == fmt_d ? 1u : 0u)); }
constexpr std::uint32_t nmsub_fmt(Format f, unsigned fd, unsigned fr, unsigned fs, unsigned ft) { return cop1x(fr, ft, fs, fd, (7u << 3) | (f == fmt_d ? 1u : 0u)); }

} // namespace ultraviolent::mips::assembler
