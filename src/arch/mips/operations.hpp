#pragma once

// MIPS operations written once against a builder (JIT.adoc "Semantics: one source, more than
// one consumer"). A builder either executes (DirectBuilder, semantics.hpp: the reference
// interpreter and tier 0) or translates (tier 1's emitting builder). An operation here is the
// only implementation of its semantics; operations not yet here stay in Executor::execute
// and reach tier 1 through call_semantics.
//
// The builder interface:
//
//   Value, Condition, Result            a guest value, a truth value, an operation's outcome
//   gpr(r), set_gpr(r, v)               guest registers; writes to register 0 are dropped
//   hi(), lo(), set_hi(v), set_lo(v)
//   constant(x)                         a value known when the operation is built
//   pc()                                the instruction's address (std::uint64_t)
//   add, sub, and_, or_, xor_, nor      64-bit
//   shift_left(v, n), shift_right_logical, shift_right_arithmetic (n: 0-63, constant)
//   sign_extend_32(v)                   bits 31:0 sign-extended
//   equal(a, b), not_equal, less_signed, less_unsigned       -> Condition
//   add_overflows_32(a, b)              signed 32-bit overflow of bits 31:0 -> Condition
//   raise_if(c, code)                   takes the exception when c holds; true if it did
//   raised()                            the Result after raise_if returned true
//   sequential(), branch(c, target, likely), jump(target_value)
//
// Semantics follow the MIPS IV Instruction Set, Revision 3.2 (ISA); the R10000 specifics they
// depend on come from the processor model, never from a test of which processor runs
// (ADR-022).

#include <ultraviolent/arch/mips/cp0.hpp>
#include <ultraviolent/arch/mips/instruction.hpp>

#include <cstdint>

namespace ultraviolent::mips::operations {

template <class B> typename B::Result addu(B& b, Instruction i) { // ADDU
    b.set_gpr(i.rd(), b.sign_extend_32(b.add(b.gpr(i.rs()), b.gpr(i.rt()))));
    return b.sequential();
}

template <class B> typename B::Result addiu(B& b, Instruction i) { // ADDIU
    b.set_gpr(i.rt(), b.sign_extend_32(b.add(b.gpr(i.rs()), b.constant(i.signed_immediate()))));
    return b.sequential();
}

template <class B> typename B::Result addi(B& b, Instruction i) { // ADDI
    const auto a = b.gpr(i.rs());
    const auto immediate = b.constant(i.signed_immediate());
    if (b.raise_if(b.add_overflows_32(a, immediate), ExceptionCode::overflow)) {
        return b.raised();
    }
    b.set_gpr(i.rt(), b.sign_extend_32(b.add(a, immediate)));
    return b.sequential();
}

template <class B> typename B::Result sll(B& b, Instruction i) { // SLL
    b.set_gpr(i.rd(), b.sign_extend_32(b.shift_left(b.gpr(i.rt()), i.sa())));
    return b.sequential();
}

// The target of a PC-relative branch: the delay slot's address plus the offset (ISA A-25).
template <class B> std::uint64_t branch_target(const B& b, Instruction i) {
    return b.pc() + 4 + (i.signed_immediate() << 2);
}

template <class B> typename B::Result beq(B& b, Instruction i) { // BEQ
    return b.branch(b.equal(b.gpr(i.rs()), b.gpr(i.rt())), branch_target(b, i), false);
}

template <class B> typename B::Result bne(B& b, Instruction i) { // BNE
    return b.branch(b.not_equal(b.gpr(i.rs()), b.gpr(i.rt())), branch_target(b, i), false);
}

} // namespace ultraviolent::mips::operations
