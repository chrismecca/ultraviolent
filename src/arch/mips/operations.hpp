#pragma once

// MIPS operations written once against a builder (JIT.adoc "Semantics: one source, more than
// one consumer"). A builder either executes (DirectBuilder, semantics.hpp: the reference
// interpreter and tier 0) or translates (tier 1's emitting builder). An operation here is the
// only implementation of its semantics; operations not here stay in Executor::execute and
// reach tier 1 through call_semantics.
//
// The builder interface:
//
//   Value, Condition, Result            a guest value, a truth value, an operation's outcome
//   gpr(r), set_gpr(r, v)               guest registers; writes to register 0 are dropped
//   hi(), lo(), set_hi(v), set_lo(v)
//   constant(x)                         a value known when the operation is built
//   pc()                                the instruction's address (std::uint64_t)
//   add, sub, and_, or_, xor_, nor      64-bit
//   shift_left(v, n), shift_right_logical, shift_right_arithmetic     n a constant, 0-63
//   shift_left_by(v, a), shift_right_logical_by, shift_right_arithmetic_by
//                                       a a value, already masked to 0-63
//   sign_extend_32(v), zero_extend_32(v)    from bits 31:0
//   equal, not_equal, less_signed, less_equal_signed, greater_signed,
//   greater_equal_signed, less_unsigned                     -> Condition
//   from_condition(c)                   0 or 1
//   add_overflows_32, sub_overflows_32  signed overflow of the bits 31:0 operation -> Condition
//   add_overflows_64, sub_overflows_64  signed 64-bit overflow -> Condition
//   raise_if(c, code)                   takes the exception when c holds; true if it did
//   raised()                            the Result after raise_if returned true
//   sequential(), branch(c, target, likely), jump(target_value)
//
// Semantics follow the MIPS IV Instruction Set, Revision 3.2 (ISA). An operation never tests
// which processor runs (ADR-022); MIPS III operations are gated on Status before they are
// reached (Executor::execute, and tier 1 at translation time).

#include <ultraviolent/arch/mips/cp0.hpp>
#include <ultraviolent/arch/mips/decoded.hpp>
#include <ultraviolent/arch/mips/instruction.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace ultraviolent::mips::operations {

// Shifts by an immediate (SLL, SRL, SRA and the D forms), 32-bit results sign-extended.
template <class B> typename B::Result sll(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.sign_extend_32(b.shift_left(b.gpr(i.rt()), i.sa())));
    return b.sequential();
}
template <class B> typename B::Result srl(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.sign_extend_32(b.shift_right_logical(b.zero_extend_32(b.gpr(i.rt())), i.sa())));
    return b.sequential();
}
template <class B> typename B::Result sra(B& b, Instruction i) {
    // The sign-extended word shifted arithmetically is already sign-extended (sa < 32).
    b.set_gpr(i.rd(), b.shift_right_arithmetic(b.sign_extend_32(b.gpr(i.rt())), i.sa()));
    return b.sequential();
}
template <class B> typename B::Result sllv(B& b, Instruction i) {
    const auto amount = b.and_(b.gpr(i.rs()), b.constant(31));
    b.set_gpr(i.rd(), b.sign_extend_32(b.shift_left_by(b.gpr(i.rt()), amount)));
    return b.sequential();
}
template <class B> typename B::Result srlv(B& b, Instruction i) {
    const auto amount = b.and_(b.gpr(i.rs()), b.constant(31));
    b.set_gpr(i.rd(),
              b.sign_extend_32(b.shift_right_logical_by(b.zero_extend_32(b.gpr(i.rt())), amount)));
    return b.sequential();
}
template <class B> typename B::Result srav(B& b, Instruction i) {
    const auto amount = b.and_(b.gpr(i.rs()), b.constant(31));
    b.set_gpr(i.rd(), b.shift_right_arithmetic_by(b.sign_extend_32(b.gpr(i.rt())), amount));
    return b.sequential();
}
template <class B> typename B::Result dsll(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.shift_left(b.gpr(i.rt()), i.sa()));
    return b.sequential();
}
template <class B> typename B::Result dsrl(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.shift_right_logical(b.gpr(i.rt()), i.sa()));
    return b.sequential();
}
template <class B> typename B::Result dsra(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.shift_right_arithmetic(b.gpr(i.rt()), i.sa()));
    return b.sequential();
}
template <class B> typename B::Result dsll32(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.shift_left(b.gpr(i.rt()), i.sa() + 32));
    return b.sequential();
}
template <class B> typename B::Result dsrl32(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.shift_right_logical(b.gpr(i.rt()), i.sa() + 32));
    return b.sequential();
}
template <class B> typename B::Result dsra32(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.shift_right_arithmetic(b.gpr(i.rt()), i.sa() + 32));
    return b.sequential();
}
template <class B> typename B::Result dsllv(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.shift_left_by(b.gpr(i.rt()), b.and_(b.gpr(i.rs()), b.constant(63))));
    return b.sequential();
}
template <class B> typename B::Result dsrlv(B& b, Instruction i) {
    b.set_gpr(i.rd(),
              b.shift_right_logical_by(b.gpr(i.rt()), b.and_(b.gpr(i.rs()), b.constant(63))));
    return b.sequential();
}
template <class B> typename B::Result dsrav(B& b, Instruction i) {
    b.set_gpr(i.rd(),
              b.shift_right_arithmetic_by(b.gpr(i.rt()), b.and_(b.gpr(i.rs()), b.constant(63))));
    return b.sequential();
}

// HI and LO moves.
template <class B> typename B::Result mfhi(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.hi());
    return b.sequential();
}
template <class B> typename B::Result mthi(B& b, Instruction i) {
    b.set_hi(b.gpr(i.rs()));
    return b.sequential();
}
template <class B> typename B::Result mflo(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.lo());
    return b.sequential();
}
template <class B> typename B::Result mtlo(B& b, Instruction i) {
    b.set_lo(b.gpr(i.rs()));
    return b.sequential();
}

// Register arithmetic and logic.
template <class B> typename B::Result add(B& b, Instruction i) { // ADD
    const auto x = b.gpr(i.rs());
    const auto y = b.gpr(i.rt());
    if (b.raise_if(b.add_overflows_32(x, y), ExceptionCode::overflow)) {
        return b.raised();
    }
    b.set_gpr(i.rd(), b.sign_extend_32(b.add(x, y)));
    return b.sequential();
}
template <class B> typename B::Result addu(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.sign_extend_32(b.add(b.gpr(i.rs()), b.gpr(i.rt()))));
    return b.sequential();
}
template <class B> typename B::Result sub(B& b, Instruction i) { // SUB
    const auto x = b.gpr(i.rs());
    const auto y = b.gpr(i.rt());
    if (b.raise_if(b.sub_overflows_32(x, y), ExceptionCode::overflow)) {
        return b.raised();
    }
    b.set_gpr(i.rd(), b.sign_extend_32(b.sub(x, y)));
    return b.sequential();
}
template <class B> typename B::Result subu(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.sign_extend_32(b.sub(b.gpr(i.rs()), b.gpr(i.rt()))));
    return b.sequential();
}
template <class B> typename B::Result and_(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.and_(b.gpr(i.rs()), b.gpr(i.rt())));
    return b.sequential();
}
template <class B> typename B::Result or_(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.or_(b.gpr(i.rs()), b.gpr(i.rt())));
    return b.sequential();
}
template <class B> typename B::Result xor_(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.xor_(b.gpr(i.rs()), b.gpr(i.rt())));
    return b.sequential();
}
template <class B> typename B::Result nor(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.nor(b.gpr(i.rs()), b.gpr(i.rt())));
    return b.sequential();
}
template <class B> typename B::Result slt(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.from_condition(b.less_signed(b.gpr(i.rs()), b.gpr(i.rt()))));
    return b.sequential();
}
template <class B> typename B::Result sltu(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.from_condition(b.less_unsigned(b.gpr(i.rs()), b.gpr(i.rt()))));
    return b.sequential();
}
template <class B> typename B::Result dadd(B& b, Instruction i) { // DADD
    const auto x = b.gpr(i.rs());
    const auto y = b.gpr(i.rt());
    if (b.raise_if(b.add_overflows_64(x, y), ExceptionCode::overflow)) {
        return b.raised();
    }
    b.set_gpr(i.rd(), b.add(x, y));
    return b.sequential();
}
template <class B> typename B::Result daddu(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.add(b.gpr(i.rs()), b.gpr(i.rt())));
    return b.sequential();
}
template <class B> typename B::Result dsub(B& b, Instruction i) { // DSUB
    const auto x = b.gpr(i.rs());
    const auto y = b.gpr(i.rt());
    if (b.raise_if(b.sub_overflows_64(x, y), ExceptionCode::overflow)) {
        return b.raised();
    }
    b.set_gpr(i.rd(), b.sub(x, y));
    return b.sequential();
}
template <class B> typename B::Result dsubu(B& b, Instruction i) {
    b.set_gpr(i.rd(), b.sub(b.gpr(i.rs()), b.gpr(i.rt())));
    return b.sequential();
}

// Immediate arithmetic and logic.
template <class B> typename B::Result addi(B& b, Instruction i) { // ADDI
    const auto x = b.gpr(i.rs());
    const auto immediate = b.constant(i.signed_immediate());
    if (b.raise_if(b.add_overflows_32(x, immediate), ExceptionCode::overflow)) {
        return b.raised();
    }
    b.set_gpr(i.rt(), b.sign_extend_32(b.add(x, immediate)));
    return b.sequential();
}
template <class B> typename B::Result addiu(B& b, Instruction i) {
    b.set_gpr(i.rt(), b.sign_extend_32(b.add(b.gpr(i.rs()), b.constant(i.signed_immediate()))));
    return b.sequential();
}
template <class B> typename B::Result daddi(B& b, Instruction i) { // DADDI
    const auto x = b.gpr(i.rs());
    const auto immediate = b.constant(i.signed_immediate());
    if (b.raise_if(b.add_overflows_64(x, immediate), ExceptionCode::overflow)) {
        return b.raised();
    }
    b.set_gpr(i.rt(), b.add(x, immediate));
    return b.sequential();
}
template <class B> typename B::Result daddiu(B& b, Instruction i) {
    b.set_gpr(i.rt(), b.add(b.gpr(i.rs()), b.constant(i.signed_immediate())));
    return b.sequential();
}
template <class B> typename B::Result slti(B& b, Instruction i) {
    b.set_gpr(i.rt(),
              b.from_condition(b.less_signed(b.gpr(i.rs()), b.constant(i.signed_immediate()))));
    return b.sequential();
}
template <class B> typename B::Result sltiu(B& b, Instruction i) {
    // The immediate is sign-extended, then compared unsigned (ISA A-110).
    b.set_gpr(i.rt(),
              b.from_condition(b.less_unsigned(b.gpr(i.rs()), b.constant(i.signed_immediate()))));
    return b.sequential();
}
template <class B> typename B::Result andi(B& b, Instruction i) {
    b.set_gpr(i.rt(), b.and_(b.gpr(i.rs()), b.constant(i.immediate())));
    return b.sequential();
}
template <class B> typename B::Result ori(B& b, Instruction i) {
    b.set_gpr(i.rt(), b.or_(b.gpr(i.rs()), b.constant(i.immediate())));
    return b.sequential();
}
template <class B> typename B::Result xori(B& b, Instruction i) {
    b.set_gpr(i.rt(), b.xor_(b.gpr(i.rs()), b.constant(i.immediate())));
    return b.sequential();
}
template <class B> typename B::Result lui(B& b, Instruction i) {
    b.set_gpr(i.rt(), b.sign_extend_32(b.constant(std::uint64_t{i.immediate()} << 16)));
    return b.sequential();
}

// Branches and jumps. The target of a PC-relative branch is the delay slot's address plus the
// offset (ISA A-25).
template <class B> std::uint64_t branch_target(const B& b, Instruction i) {
    return b.pc() + 4 + (i.signed_immediate() << 2);
}
template <class B> typename B::Result beq(B& b, Instruction i) {
    return b.branch(b.equal(b.gpr(i.rs()), b.gpr(i.rt())), branch_target(b, i), false);
}
template <class B> typename B::Result bne(B& b, Instruction i) {
    return b.branch(b.not_equal(b.gpr(i.rs()), b.gpr(i.rt())), branch_target(b, i), false);
}
template <class B> typename B::Result blez(B& b, Instruction i) {
    return b.branch(b.less_equal_signed(b.gpr(i.rs()), b.constant(0)), branch_target(b, i), false);
}
template <class B> typename B::Result bgtz(B& b, Instruction i) {
    return b.branch(b.greater_signed(b.gpr(i.rs()), b.constant(0)), branch_target(b, i), false);
}
template <class B> typename B::Result bltz(B& b, Instruction i) {
    return b.branch(b.less_signed(b.gpr(i.rs()), b.constant(0)), branch_target(b, i), false);
}
template <class B> typename B::Result bgez(B& b, Instruction i) {
    return b.branch(b.greater_equal_signed(b.gpr(i.rs()), b.constant(0)), branch_target(b, i),
                    false);
}
template <class B> typename B::Result beql(B& b, Instruction i) {
    return b.branch(b.equal(b.gpr(i.rs()), b.gpr(i.rt())), branch_target(b, i), true);
}
template <class B> typename B::Result bnel(B& b, Instruction i) {
    return b.branch(b.not_equal(b.gpr(i.rs()), b.gpr(i.rt())), branch_target(b, i), true);
}
template <class B> typename B::Result blezl(B& b, Instruction i) {
    return b.branch(b.less_equal_signed(b.gpr(i.rs()), b.constant(0)), branch_target(b, i), true);
}
template <class B> typename B::Result bgtzl(B& b, Instruction i) {
    return b.branch(b.greater_signed(b.gpr(i.rs()), b.constant(0)), branch_target(b, i), true);
}
template <class B> typename B::Result bltzl(B& b, Instruction i) {
    return b.branch(b.less_signed(b.gpr(i.rs()), b.constant(0)), branch_target(b, i), true);
}
template <class B> typename B::Result bgezl(B& b, Instruction i) {
    return b.branch(b.greater_equal_signed(b.gpr(i.rs()), b.constant(0)), branch_target(b, i),
                    true);
}
// BLTZAL, BGEZAL, BLTZALL, BGEZALL: the condition is read before the link is written, which
// is written whether or not the branch is taken.
template <class B> typename B::Result branch_and_link(B& b, Instruction i) {
    const auto condition = (i.rt() & 1) != 0
                               ? b.greater_equal_signed(b.gpr(i.rs()), b.constant(0))
                               : b.less_signed(b.gpr(i.rs()), b.constant(0));
    b.set_gpr(31, b.constant(b.pc() + 8));
    return b.branch(condition, branch_target(b, i), i.rt() >= 18);
}
template <class B> typename B::Result j(B& b, Instruction i) {
    return b.jump(b.constant(((b.pc() + 4) & ~std::uint64_t{0x0fff'ffff}) | (i.target() << 2)));
}
template <class B> typename B::Result jal(B& b, Instruction i) {
    const auto target =
        b.constant(((b.pc() + 4) & ~std::uint64_t{0x0fff'ffff}) | (i.target() << 2));
    b.set_gpr(31, b.constant(b.pc() + 8));
    return b.jump(target);
}
template <class B> typename B::Result jr(B& b, Instruction i) {
    return b.jump(b.gpr(i.rs()));
}
template <class B> typename B::Result jalr(B& b, Instruction i) {
    const auto target = b.gpr(i.rs()); // read before the link: rd may equal rs
    b.set_gpr(i.rd(), b.constant(b.pc() + 8));
    return b.jump(target);
}

// The operations above, for a builder that dispatches at translation time (tier 1); empty for
// an operation not in builder form. The interpreters call each function from their own switch
// instead (one dispatch per instruction).
template <class B>
std::optional<typename B::Result> translate(B& b, DecodedOpcode op, Instruction i) {
    using enum DecodedOpcode;
    switch (op) {
    case sll: return operations::sll(b, i);
    case srl: return operations::srl(b, i);
    case sra: return operations::sra(b, i);
    case sllv: return operations::sllv(b, i);
    case srlv: return operations::srlv(b, i);
    case srav: return operations::srav(b, i);
    case dsll: return operations::dsll(b, i);
    case dsrl: return operations::dsrl(b, i);
    case dsra: return operations::dsra(b, i);
    case dsll32: return operations::dsll32(b, i);
    case dsrl32: return operations::dsrl32(b, i);
    case dsra32: return operations::dsra32(b, i);
    case dsllv: return operations::dsllv(b, i);
    case dsrlv: return operations::dsrlv(b, i);
    case dsrav: return operations::dsrav(b, i);
    case mfhi: return operations::mfhi(b, i);
    case mthi: return operations::mthi(b, i);
    case mflo: return operations::mflo(b, i);
    case mtlo: return operations::mtlo(b, i);
    case DecodedOpcode::add: return operations::add(b, i);
    case addu: return operations::addu(b, i);
    case DecodedOpcode::sub: return operations::sub(b, i);
    case subu: return operations::subu(b, i);
    case DecodedOpcode::and_: return operations::and_(b, i);
    case DecodedOpcode::or_: return operations::or_(b, i);
    case DecodedOpcode::xor_: return operations::xor_(b, i);
    case DecodedOpcode::nor: return operations::nor(b, i);
    case slt: return operations::slt(b, i);
    case sltu: return operations::sltu(b, i);
    case dadd: return operations::dadd(b, i);
    case daddu: return operations::daddu(b, i);
    case dsub: return operations::dsub(b, i);
    case dsubu: return operations::dsubu(b, i);
    case addi: return operations::addi(b, i);
    case addiu: return operations::addiu(b, i);
    case daddi: return operations::daddi(b, i);
    case daddiu: return operations::daddiu(b, i);
    case slti: return operations::slti(b, i);
    case sltiu: return operations::sltiu(b, i);
    case andi: return operations::andi(b, i);
    case ori: return operations::ori(b, i);
    case xori: return operations::xori(b, i);
    case lui: return operations::lui(b, i);
    case beq: return operations::beq(b, i);
    case bne: return operations::bne(b, i);
    case blez: return operations::blez(b, i);
    case bgtz: return operations::bgtz(b, i);
    case bltz: return operations::bltz(b, i);
    case bgez: return operations::bgez(b, i);
    case beql: return operations::beql(b, i);
    case bnel: return operations::bnel(b, i);
    case blezl: return operations::blezl(b, i);
    case bgtzl: return operations::bgtzl(b, i);
    case bltzl: return operations::bltzl(b, i);
    case bgezl: return operations::bgezl(b, i);
    case bltzal:
    case bgezal:
    case bltzall:
    case bgezall: return operations::branch_and_link(b, i);
    case DecodedOpcode::j: return operations::j(b, i);
    case jal: return operations::jal(b, i);
    case jr: return operations::jr(b, i);
    case jalr: return operations::jalr(b, i);
    default: return std::nullopt;
    }
}

} // namespace ultraviolent::mips::operations
