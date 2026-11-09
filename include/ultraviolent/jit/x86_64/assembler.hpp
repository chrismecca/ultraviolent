#pragma once

// A minimal x86-64 encoder for generated code (JIT.adoc "x86-64 encoding"): only the forms
// tier 1 emits. Host-specific and guest-agnostic. Encodings are checked against GNU as
// (tests/jit/x86_64_encoding_test.cpp). References: Intel 64 and IA-32 Architectures
// Software Developer's Manual, volume 2 (instruction formats, ModR/M and SIB, REX).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ultraviolent::jit::x86_64 {

enum class Reg : std::uint8_t {
    rax,
    rcx,
    rdx,
    rbx,
    rsp,
    rbp,
    rsi,
    rdi,
    r8,
    r9,
    r10,
    r11,
    r12,
    r13,
    r14,
    r15,
};

// SSE registers; generated code uses them only within one operation.
enum class Xmm : std::uint8_t { xmm0, xmm1 };

// [base + index + displacement]; rsp as the index means none (it cannot be one).
struct Mem {
    Reg base;
    std::int32_t displacement{};
    Reg index{Reg::rsp};
};

// Condition codes, in their encoding order (Jcc, SETcc, CMOVcc).
enum class Cond : std::uint8_t {
    o,
    no,
    b, // unsigned below
    ae,
    e,
    ne,
    be,
    a,
    s,
    ns,
    p,
    np,
    l, // signed less
    ge,
    le,
    g,
};

[[nodiscard]] constexpr Cond invert(Cond c) {
    return static_cast<Cond>(static_cast<std::uint8_t>(c) ^ 1);
}

// A jump target in the code being assembled.
struct Label {
    std::uint32_t id{~std::uint32_t{0}};
};

// Assembles into a byte buffer that will execute at `origin` (needed for RIP-relative
// operands). Jumps to labels always use 32-bit displacements, resolved by finish().
class Assembler {
  public:
    explicit Assembler(std::uint64_t origin = 0) : origin_{origin} {}

    [[nodiscard]] std::size_t size() const {
        return code_.size();
    }
    // The address the next instruction will execute at.
    [[nodiscard]] std::uint64_t here() const {
        return origin_ + code_.size();
    }

    // 64-bit operand size unless the name says otherwise.
    void mov(Reg dst, Reg src);
    void mov(Reg dst, Mem src);
    void mov(Mem dst, Reg src);
    // The shortest form: mov r32, imm32 (zero-extending), mov r64, simm32, or movabs.
    void mov(Reg dst, std::uint64_t immediate);
    // mov qword [mem], simm32 (sign-extended).
    void mov(Mem dst, std::int32_t immediate);
    // mov r32, r32: the low 32 bits, zero-extended.
    void mov32(Reg dst, Reg src);
    void mov_byte(Mem dst, std::uint8_t immediate);
    void mov_byte(Mem dst, Reg src);
    void movzx_byte(Reg dst, Mem src);
    void movzx_byte(Reg dst, Reg src);
    void movsxd(Reg dst, Reg src); // sign-extend bits 31:0
    // Memory loads and stores by width (the register's low bytes); 32-bit and narrower loads
    // zero-extend into the 64-bit register, except the movsx forms (64-bit destination).
    void mov32(Reg dst, Mem src);
    void mov32(Mem dst, Reg src);
    void mov16(Mem dst, Reg src);
    void movzx_word(Reg dst, Mem src);
    void movsx_byte(Reg dst, Mem src);
    void movsx_word(Reg dst, Reg src);
    void bswap32(Reg r);
    void bswap64(Reg r);
    void rol16(Reg r, std::uint8_t count);
    void lea(Reg dst, Mem src);

    void add(Reg dst, Reg src);
    void sub(Reg dst, Reg src);
    void and_(Reg dst, Reg src);
    void or_(Reg dst, Reg src);
    void xor_(Reg dst, Reg src);
    void cmp(Reg a, Reg b);
    void test(Reg a, Reg b);
    void add(Reg dst, std::int32_t immediate);
    void sub(Reg dst, std::int32_t immediate);
    void and_(Reg dst, std::int32_t immediate);
    void or_(Reg dst, std::int32_t immediate);
    void xor_(Reg dst, std::int32_t immediate);
    void cmp(Reg a, std::int32_t immediate);
    void cmp(Reg a, Mem b);
    void add(Reg dst, Mem src);
    void or_(Reg dst, Mem src);
    // Byte-swapping loads and stores (MOVBE; the host must support it), by width in bytes
    // (2, 4, 8). A 2-byte load leaves bits 63:16 unchanged; a 4-byte load zero-extends.
    void movbe(Reg dst, Mem src, unsigned bytes);
    void movbe(Mem dst, Reg src, unsigned bytes);
    void add(Mem dst, std::int32_t immediate);
    void cmp_byte(Mem a, std::uint8_t immediate);
    void not_(Reg dst);
    void neg(Reg dst);
    void imul(Reg dst, Reg src); // dst = low 64 bits of dst * src
    // rdx:rax = rax * src (unsigned, signed); rax = rdx:rax / src and rdx the remainder
    // (unsigned, signed); rdx = the sign of rax (cqo).
    void mul(Reg src);
    void imul(Reg src);
    void div(Reg src);
    void idiv(Reg src);
    void cqo();

    // Scalar SSE: loads, arithmetic and unordered compares with a memory operand, moves to
    // general registers (movq 64 bits, movd 32 bits zero-extended), int32 to double, and
    // MXCSR.
    void movsd(Xmm dst, Mem src);
    void movss(Xmm dst, Mem src);
    void addsd(Xmm dst, Mem src);
    void subsd(Xmm dst, Mem src);
    void mulsd(Xmm dst, Mem src);
    void divsd(Xmm dst, Mem src);
    void addss(Xmm dst, Mem src);
    void subss(Xmm dst, Mem src);
    void mulss(Xmm dst, Mem src);
    void divss(Xmm dst, Mem src);
    void ucomisd(Xmm a, Mem b);
    void ucomiss(Xmm a, Mem b);
    void movq(Reg dst, Xmm src);
    void movd(Reg dst, Xmm src);
    void cvtsi2sd(Xmm dst, Reg src32);
    void ldmxcsr(Mem src);
    void stmxcsr(Mem dst);
    void add32(Reg dst, Reg src);
    void sub32(Reg dst, Reg src);
    void add32(Reg dst, std::int32_t immediate);
    void sub32(Reg dst, std::int32_t immediate);

    void shl(Reg dst, std::uint8_t count);
    void shr(Reg dst, std::uint8_t count);
    void sar(Reg dst, std::uint8_t count);
    void shl_cl(Reg dst);
    void shr_cl(Reg dst);
    void sar_cl(Reg dst);
    void shl32(Reg dst, std::uint8_t count);
    void shr32(Reg dst, std::uint8_t count);
    void sar32(Reg dst, std::uint8_t count);
    void shl32_cl(Reg dst);
    void shr32_cl(Reg dst);
    void sar32_cl(Reg dst);

    void setcc(Cond c, Reg dst); // the low byte of dst
    void cmov(Cond c, Reg dst, Reg src);

    void push(Reg r);
    void pop(Reg r);
    void ret();
    void call(Reg target);
    void jmp(Reg target);
    // call [rip + d], d chosen so the memory operand is at `slot`.
    void call_indirect(std::uint64_t slot);
    void jmp(Mem target); // jmp qword [mem]
    // call rel32, jmp rel32, and jcc rel32 to code outside this assembly, at `target`.
    void call_to(std::uint64_t target);
    void jmp_to(std::uint64_t target);
    void jcc_to(Cond c, std::uint64_t target);

    [[nodiscard]] Label label();
    void bind(Label label);
    void jmp(Label target);
    void jcc(Cond c, Label target);

    // Resolves jumps to labels; every label used must be bound.
    std::span<const std::uint8_t> finish();

  private:
    void byte(std::uint8_t b) {
        code_.push_back(b);
    }
    void bytes32(std::uint32_t v);
    void bytes64(std::uint64_t v);
    void rex(bool w, unsigned reg, unsigned index, unsigned base, bool force = false);
    void modrm_register(unsigned reg, unsigned rm);
    void modrm_memory(unsigned reg, Mem m);
    // opcode reg/rm forms, 64-bit or 32-bit
    void op_rr(std::uint8_t opcode, Reg reg, Reg rm, bool wide = true);
    void op_rm(std::uint8_t opcode, Reg reg, Mem m, bool wide = true);
    void group1(unsigned extension, Reg dst, std::int32_t immediate, bool wide = true);
    void shift(unsigned extension, Reg dst, std::uint8_t count, bool wide);
    void shift_cl(unsigned extension, Reg dst, bool wide);
    void jump_to(Label target, std::uint8_t opcode1, std::int16_t opcode2);
    // [prefix] [REX] 0F opcode with ModRM reg `reg` and memory `m` (prefix 0: none).
    void sse(std::uint8_t prefix, std::uint8_t opcode, unsigned reg, Mem m);

    struct Fixup {
        std::size_t at; // the rel32 field
        std::uint32_t label;
    };

    std::uint64_t origin_;
    std::vector<std::uint8_t> code_;
    std::vector<std::int64_t> labels_; // bound offset, or -1
    std::vector<Fixup> fixups_;
};

} // namespace ultraviolent::jit::x86_64
