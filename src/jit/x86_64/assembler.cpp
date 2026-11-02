#include <ultraviolent/jit/x86_64/assembler.hpp>

#include <ultraviolent/core/invariant.hpp>

namespace ultraviolent::jit::x86_64 {

namespace {

constexpr unsigned code(Reg r) {
    return static_cast<unsigned>(r);
}

constexpr bool fits_int8(std::int64_t v) {
    return v >= -128 && v <= 127;
}

// spl, bpl, sil, dil need a REX prefix to be addressed as bytes (otherwise ah..bh).
constexpr bool needs_rex_for_byte(Reg r) {
    return code(r) >= 4 && code(r) < 8;
}

constexpr bool has_index(Mem m) {
    return m.index != Reg::rsp;
}

// The index register's number for REX.X (0 when there is none).
constexpr unsigned index_code(Mem m) {
    return has_index(m) ? code(m.index) : 0;
}

} // namespace

void Assembler::bytes32(std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
        byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void Assembler::bytes64(std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) {
        byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void Assembler::rex(bool w, unsigned reg, unsigned index, unsigned base, bool force) {
    const unsigned value = (w ? 8u : 0u) | ((reg >> 3) << 2) | ((index >> 3) << 1) | (base >> 3);
    if (value != 0 || force) {
        byte(static_cast<std::uint8_t>(0x40 | value));
    }
}

void Assembler::modrm_register(unsigned reg, unsigned rm) {
    byte(static_cast<std::uint8_t>(0xc0 | ((reg & 7) << 3) | (rm & 7)));
}

void Assembler::modrm_memory(unsigned reg, Mem m) {
    const unsigned base = code(m.base) & 7;
    // rbp and r13 as a base always need a displacement (mod 00 means RIP or no base).
    const unsigned mod = (m.displacement == 0 && base != 5) ? 0 : fits_int8(m.displacement) ? 1 : 2;
    if (has_index(m)) {
        // SIB: scale 1, the index, the base.
        byte(static_cast<std::uint8_t>((mod << 6) | ((reg & 7) << 3) | 4));
        byte(static_cast<std::uint8_t>(((code(m.index) & 7) << 3) | base));
    } else {
        byte(static_cast<std::uint8_t>((mod << 6) | ((reg & 7) << 3) | base));
        if (base == 4) {
            byte(0x24); // SIB: no index, base rsp or r12
        }
    }
    if (mod == 1) {
        byte(static_cast<std::uint8_t>(m.displacement));
    } else if (mod == 2) {
        bytes32(static_cast<std::uint32_t>(m.displacement));
    }
}

void Assembler::op_rr(std::uint8_t opcode, Reg reg, Reg rm, bool wide) {
    rex(wide, code(reg), 0, code(rm));
    byte(opcode);
    modrm_register(code(reg), code(rm));
}

void Assembler::op_rm(std::uint8_t opcode, Reg reg, Mem m, bool wide) {
    rex(wide, code(reg), index_code(m), code(m.base));
    byte(opcode);
    modrm_memory(code(reg), m);
}

void Assembler::mov(Reg dst, Reg src) {
    op_rr(0x89, src, dst);
}
void Assembler::mov(Reg dst, Mem src) {
    op_rm(0x8b, dst, src);
}
void Assembler::mov(Mem dst, Reg src) {
    op_rm(0x89, src, dst);
}

void Assembler::mov(Reg dst, std::uint64_t immediate) {
    if (immediate <= 0xffff'ffff) {
        rex(false, 0, 0, code(dst));
        byte(static_cast<std::uint8_t>(0xb8 + (code(dst) & 7)));
        bytes32(static_cast<std::uint32_t>(immediate));
    } else if (static_cast<std::int64_t>(immediate) ==
               static_cast<std::int32_t>(static_cast<std::uint32_t>(immediate))) {
        rex(true, 0, 0, code(dst));
        byte(0xc7);
        modrm_register(0, code(dst));
        bytes32(static_cast<std::uint32_t>(immediate));
    } else {
        rex(true, 0, 0, code(dst));
        byte(static_cast<std::uint8_t>(0xb8 + (code(dst) & 7)));
        bytes64(immediate);
    }
}

void Assembler::mov(Mem dst, std::int32_t immediate) {
    rex(true, 0, index_code(dst), code(dst.base));
    byte(0xc7);
    modrm_memory(0, dst);
    bytes32(static_cast<std::uint32_t>(immediate));
}

void Assembler::mov32(Reg dst, Reg src) {
    op_rr(0x89, src, dst, false);
}

void Assembler::mov_byte(Mem dst, std::uint8_t immediate) {
    rex(false, 0, index_code(dst), code(dst.base));
    byte(0xc6);
    modrm_memory(0, dst);
    byte(immediate);
}

void Assembler::mov_byte(Mem dst, Reg src) {
    rex(false, code(src), index_code(dst), code(dst.base), needs_rex_for_byte(src));
    byte(0x88);
    modrm_memory(code(src), dst);
}

void Assembler::movzx_byte(Reg dst, Mem src) {
    rex(false, code(dst), index_code(src), code(src.base));
    byte(0x0f);
    byte(0xb6);
    modrm_memory(code(dst), src);
}

void Assembler::movzx_byte(Reg dst, Reg src) {
    rex(false, code(dst), 0, code(src), needs_rex_for_byte(src));
    byte(0x0f);
    byte(0xb6);
    modrm_register(code(dst), code(src));
}

void Assembler::movsxd(Reg dst, Reg src) {
    op_rr(0x63, dst, src);
}

void Assembler::mov32(Reg dst, Mem src) {
    op_rm(0x8b, dst, src, false);
}
void Assembler::mov32(Mem dst, Reg src) {
    op_rm(0x89, src, dst, false);
}
void Assembler::mov16(Mem dst, Reg src) {
    byte(0x66);
    op_rm(0x89, src, dst, false);
}

void Assembler::movzx_word(Reg dst, Mem src) {
    rex(false, code(dst), index_code(src), code(src.base));
    byte(0x0f);
    byte(0xb7);
    modrm_memory(code(dst), src);
}

void Assembler::movsx_byte(Reg dst, Mem src) {
    rex(true, code(dst), index_code(src), code(src.base));
    byte(0x0f);
    byte(0xbe);
    modrm_memory(code(dst), src);
}

void Assembler::movsx_word(Reg dst, Reg src) {
    rex(true, code(dst), 0, code(src));
    byte(0x0f);
    byte(0xbf);
    modrm_register(code(dst), code(src));
}

void Assembler::bswap32(Reg r) {
    rex(false, 0, 0, code(r));
    byte(0x0f);
    byte(static_cast<std::uint8_t>(0xc8 + (code(r) & 7)));
}

void Assembler::bswap64(Reg r) {
    rex(true, 0, 0, code(r));
    byte(0x0f);
    byte(static_cast<std::uint8_t>(0xc8 + (code(r) & 7)));
}

void Assembler::rol16(Reg r, std::uint8_t count) {
    byte(0x66);
    rex(false, 0, 0, code(r));
    byte(0xc1);
    modrm_register(0, code(r));
    byte(count);
}

void Assembler::lea(Reg dst, Mem src) {
    op_rm(0x8d, dst, src);
}

void Assembler::add(Reg dst, Reg src) {
    op_rr(0x01, src, dst);
}
void Assembler::sub(Reg dst, Reg src) {
    op_rr(0x29, src, dst);
}
void Assembler::and_(Reg dst, Reg src) {
    op_rr(0x21, src, dst);
}
void Assembler::or_(Reg dst, Reg src) {
    op_rr(0x09, src, dst);
}
void Assembler::xor_(Reg dst, Reg src) {
    op_rr(0x31, src, dst);
}
void Assembler::cmp(Reg a, Reg b) {
    op_rr(0x39, b, a);
}
void Assembler::test(Reg a, Reg b) {
    op_rr(0x85, b, a);
}
void Assembler::add32(Reg dst, Reg src) {
    op_rr(0x01, src, dst, false);
}
void Assembler::sub32(Reg dst, Reg src) {
    op_rr(0x29, src, dst, false);
}

void Assembler::group1(unsigned extension, Reg dst, std::int32_t immediate, bool wide) {
    rex(wide, 0, 0, code(dst));
    if (dst == Reg::rax && !fits_int8(immediate)) {
        // The accumulator form, one byte shorter (and what GNU as emits).
        byte(static_cast<std::uint8_t>(0x05 + 8 * extension));
        bytes32(static_cast<std::uint32_t>(immediate));
        return;
    }
    if (fits_int8(immediate)) {
        byte(0x83);
        modrm_register(extension, code(dst));
        byte(static_cast<std::uint8_t>(immediate));
    } else {
        byte(0x81);
        modrm_register(extension, code(dst));
        bytes32(static_cast<std::uint32_t>(immediate));
    }
}

void Assembler::add(Reg dst, std::int32_t immediate) {
    group1(0, dst, immediate);
}
void Assembler::or_(Reg dst, std::int32_t immediate) {
    group1(1, dst, immediate);
}
void Assembler::sub(Reg dst, std::int32_t immediate) {
    group1(5, dst, immediate);
}
void Assembler::and_(Reg dst, std::int32_t immediate) {
    group1(4, dst, immediate);
}
void Assembler::xor_(Reg dst, std::int32_t immediate) {
    group1(6, dst, immediate);
}
void Assembler::cmp(Reg a, std::int32_t immediate) {
    group1(7, a, immediate);
}
void Assembler::add32(Reg dst, std::int32_t immediate) {
    group1(0, dst, immediate, false);
}
void Assembler::sub32(Reg dst, std::int32_t immediate) {
    group1(5, dst, immediate, false);
}

void Assembler::cmp(Reg a, Mem b) {
    op_rm(0x3b, a, b);
}

void Assembler::add(Reg dst, Mem src) {
    op_rm(0x03, dst, src);
}

void Assembler::or_(Reg dst, Mem src) {
    op_rm(0x0b, dst, src);
}

void Assembler::movbe(Reg dst, Mem src, unsigned bytes) {
    invariant(bytes == 2 || bytes == 4 || bytes == 8, "movbe moves 2, 4, or 8 bytes");
    if (bytes == 2) {
        byte(0x66);
    }
    rex(bytes == 8, code(dst), index_code(src), code(src.base));
    byte(0x0f);
    byte(0x38);
    byte(0xf0);
    modrm_memory(code(dst), src);
}

void Assembler::movbe(Mem dst, Reg src, unsigned bytes) {
    invariant(bytes == 2 || bytes == 4 || bytes == 8, "movbe moves 2, 4, or 8 bytes");
    if (bytes == 2) {
        byte(0x66);
    }
    rex(bytes == 8, code(src), index_code(dst), code(dst.base));
    byte(0x0f);
    byte(0x38);
    byte(0xf1);
    modrm_memory(code(src), dst);
}

void Assembler::add(Mem dst, std::int32_t immediate) {
    rex(true, 0, index_code(dst), code(dst.base));
    if (fits_int8(immediate)) {
        byte(0x83);
        modrm_memory(0, dst);
        byte(static_cast<std::uint8_t>(immediate));
    } else {
        byte(0x81);
        modrm_memory(0, dst);
        bytes32(static_cast<std::uint32_t>(immediate));
    }
}

void Assembler::cmp_byte(Mem a, std::uint8_t immediate) {
    rex(false, 0, index_code(a), code(a.base));
    byte(0x80);
    modrm_memory(7, a);
    byte(immediate);
}

void Assembler::not_(Reg dst) {
    rex(true, 0, 0, code(dst));
    byte(0xf7);
    modrm_register(2, code(dst));
}

void Assembler::neg(Reg dst) {
    rex(true, 0, 0, code(dst));
    byte(0xf7);
    modrm_register(3, code(dst));
}

void Assembler::imul(Reg dst, Reg src) {
    rex(true, code(dst), 0, code(src));
    byte(0x0f);
    byte(0xaf);
    modrm_register(code(dst), code(src));
}

void Assembler::mul(Reg src) {
    rex(true, 0, 0, code(src));
    byte(0xf7);
    modrm_register(4, code(src));
}

void Assembler::imul(Reg src) {
    rex(true, 0, 0, code(src));
    byte(0xf7);
    modrm_register(5, code(src));
}

void Assembler::div(Reg src) {
    rex(true, 0, 0, code(src));
    byte(0xf7);
    modrm_register(6, code(src));
}

void Assembler::idiv(Reg src) {
    rex(true, 0, 0, code(src));
    byte(0xf7);
    modrm_register(7, code(src));
}

void Assembler::cqo() {
    byte(0x48);
    byte(0x99);
}

void Assembler::shift(unsigned extension, Reg dst, std::uint8_t count, bool wide) {
    rex(wide, 0, 0, code(dst));
    if (count == 1) {
        byte(0xd1);
        modrm_register(extension, code(dst));
        return;
    }
    byte(0xc1);
    modrm_register(extension, code(dst));
    byte(count);
}

void Assembler::shift_cl(unsigned extension, Reg dst, bool wide) {
    rex(wide, 0, 0, code(dst));
    byte(0xd3);
    modrm_register(extension, code(dst));
}

void Assembler::shl(Reg dst, std::uint8_t count) {
    shift(4, dst, count, true);
}
void Assembler::shr(Reg dst, std::uint8_t count) {
    shift(5, dst, count, true);
}
void Assembler::sar(Reg dst, std::uint8_t count) {
    shift(7, dst, count, true);
}
void Assembler::shl_cl(Reg dst) {
    shift_cl(4, dst, true);
}
void Assembler::shr_cl(Reg dst) {
    shift_cl(5, dst, true);
}
void Assembler::sar_cl(Reg dst) {
    shift_cl(7, dst, true);
}
void Assembler::shl32(Reg dst, std::uint8_t count) {
    shift(4, dst, count, false);
}
void Assembler::shr32(Reg dst, std::uint8_t count) {
    shift(5, dst, count, false);
}
void Assembler::sar32(Reg dst, std::uint8_t count) {
    shift(7, dst, count, false);
}
void Assembler::shl32_cl(Reg dst) {
    shift_cl(4, dst, false);
}
void Assembler::shr32_cl(Reg dst) {
    shift_cl(5, dst, false);
}
void Assembler::sar32_cl(Reg dst) {
    shift_cl(7, dst, false);
}

void Assembler::setcc(Cond c, Reg dst) {
    rex(false, 0, 0, code(dst), needs_rex_for_byte(dst));
    byte(0x0f);
    byte(static_cast<std::uint8_t>(0x90 | static_cast<unsigned>(c)));
    modrm_register(0, code(dst));
}

void Assembler::cmov(Cond c, Reg dst, Reg src) {
    rex(true, code(dst), 0, code(src));
    byte(0x0f);
    byte(static_cast<std::uint8_t>(0x40 | static_cast<unsigned>(c)));
    modrm_register(code(dst), code(src));
}

void Assembler::push(Reg r) {
    rex(false, 0, 0, code(r));
    byte(static_cast<std::uint8_t>(0x50 + (code(r) & 7)));
}

void Assembler::pop(Reg r) {
    rex(false, 0, 0, code(r));
    byte(static_cast<std::uint8_t>(0x58 + (code(r) & 7)));
}

void Assembler::ret() {
    byte(0xc3);
}

void Assembler::call(Reg target) {
    rex(false, 0, 0, code(target));
    byte(0xff);
    modrm_register(2, code(target));
}

void Assembler::jmp(Reg target) {
    rex(false, 0, 0, code(target));
    byte(0xff);
    modrm_register(4, code(target));
}

void Assembler::call_indirect(std::uint64_t slot) {
    // FF /2 with ModRM 00 010 101: [rip + disp32], relative to the end of the instruction.
    const std::int64_t displacement = static_cast<std::int64_t>(slot - (here() + 6));
    invariant(displacement == static_cast<std::int32_t>(displacement),
              "helper table within reach of rel32");
    byte(0xff);
    byte(0x15);
    bytes32(static_cast<std::uint32_t>(displacement));
}

void Assembler::jmp(Mem target) {
    rex(false, 0, index_code(target), code(target.base));
    byte(0xff);
    modrm_memory(4, target);
}

void Assembler::jmp_to(std::uint64_t target) {
    const std::int64_t displacement = static_cast<std::int64_t>(target - (here() + 5));
    invariant(displacement == static_cast<std::int32_t>(displacement), "jump within rel32");
    byte(0xe9);
    bytes32(static_cast<std::uint32_t>(displacement));
}

void Assembler::jcc_to(Cond c, std::uint64_t target) {
    const std::int64_t displacement = static_cast<std::int64_t>(target - (here() + 6));
    invariant(displacement == static_cast<std::int32_t>(displacement), "jump within rel32");
    byte(0x0f);
    byte(static_cast<std::uint8_t>(0x80 | static_cast<unsigned>(c)));
    bytes32(static_cast<std::uint32_t>(displacement));
}

Label Assembler::label() {
    labels_.push_back(-1);
    return Label{static_cast<std::uint32_t>(labels_.size() - 1)};
}

void Assembler::bind(Label label) {
    invariant(labels_.at(label.id) < 0, "label bound once");
    labels_[label.id] = static_cast<std::int64_t>(code_.size());
}

void Assembler::jump_to(Label target, std::uint8_t opcode1, std::int16_t opcode2) {
    byte(opcode1);
    if (opcode2 >= 0) {
        byte(static_cast<std::uint8_t>(opcode2));
    }
    fixups_.push_back({code_.size(), target.id});
    bytes32(0);
}

void Assembler::jmp(Label target) {
    jump_to(target, 0xe9, -1);
}

void Assembler::jcc(Cond c, Label target) {
    jump_to(target, 0x0f, static_cast<std::int16_t>(0x80 | static_cast<unsigned>(c)));
}

std::span<const std::uint8_t> Assembler::finish() {
    for (const Fixup& f : fixups_) {
        const std::int64_t target = labels_.at(f.label);
        invariant(target >= 0, "every label used is bound");
        const auto rel = static_cast<std::int32_t>(target - static_cast<std::int64_t>(f.at + 4));
        for (unsigned i = 0; i < 4; ++i) {
            code_[f.at + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(rel) >> (8 * i));
        }
    }
    fixups_.clear();
    return code_;
}

} // namespace ultraviolent::jit::x86_64
