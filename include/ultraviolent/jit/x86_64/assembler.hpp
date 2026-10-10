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

// [base + displacement].
struct Mem {
    Reg base;
    std::int32_t displacement{};
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
    void add(Mem dst, std::int32_t immediate);
    void cmp_byte(Mem a, std::uint8_t immediate);
    void not_(Reg dst);
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
