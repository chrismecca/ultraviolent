#pragma once

#include <ultraviolent/arch/mips/decoded.hpp>

#include <ultraviolent/arch/mips/fpu.hpp>
#include <ultraviolent/arch/mips/instruction.hpp>

#include "operations.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>

// The MIPS semantics, the one copy every engine uses (IR.adoc "DecodedInstruction"). A
// private header so the engines (interpreter.cpp, block_interpreter.cpp) inline them into
// their loops (IR.adoc "Stage H"); semantics.cpp compiles the public, out-of-line `execute`
// from the same source.
//
// Instruction semantics follow the MIPS IV Instruction Set, Revision 3.2 (ISA), with R10000
// specifics from the R10000 User's Manual, Version 2.0 (UM). Choices for behavior the ISA
// leaves UNPREDICTABLE are listed in doc/MIPS.adoc.
namespace ultraviolent::mips {

namespace semantics {

constexpr std::uint64_t sign_extend_32(std::uint64_t value) {
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(value))));
}

constexpr std::uint64_t sign_extend_16(std::uint64_t value) {
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(static_cast<std::int16_t>(static_cast<std::uint16_t>(value))));
}

constexpr std::uint64_t sign_extend_8(std::uint64_t value) {
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(static_cast<std::int8_t>(static_cast<std::uint8_t>(value))));
}

constexpr std::int64_t as_signed(std::uint64_t value) {
    return static_cast<std::int64_t>(value);
}

struct Product {
    std::uint64_t hi;
    std::uint64_t lo;
};

// Full 128-bit product of two unsigned 64-bit values, in portable arithmetic.
constexpr Product multiply_unsigned(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a_lo = a & 0xffff'ffff;
    const std::uint64_t a_hi = a >> 32;
    const std::uint64_t b_lo = b & 0xffff'ffff;
    const std::uint64_t b_hi = b >> 32;
    const std::uint64_t p0 = a_lo * b_lo;
    const std::uint64_t p1 = a_lo * b_hi;
    const std::uint64_t p2 = a_hi * b_lo;
    const std::uint64_t p3 = a_hi * b_hi;
    const std::uint64_t middle = (p0 >> 32) + (p1 & 0xffff'ffff) + (p2 & 0xffff'ffff);
    return {p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32), (p0 & 0xffff'ffff) | (middle << 32)};
}

constexpr Product multiply_signed(std::uint64_t a, std::uint64_t b) {
    Product p = multiply_unsigned(a, b);
    // Two's-complement correction of the high half.
    if (as_signed(a) < 0) {
        p.hi -= b;
    }
    if (as_signed(b) < 0) {
        p.hi -= a;
    }
    return p;
}

// What an operation's semantics return: the control flow, or Flow::exception once the
// exception has been taken (Executor::fail). 16 bytes, so it stays in registers; the
// std::expected<Control, Exception> it replaces went through memory (IR.adoc "Stage H").
// Taking the exception where it is raised is exact because every error is returned at once:
// nothing in an operation runs after it raises, and no operation raises twice.
using Result = Control;

constexpr Control sequential() {
    return {};
}

// The builder that executes (operations.hpp): values are the guest's, operations happen
// at once, and an exception is taken where it is raised. After inlining it is the code the
// operation had before it moved into builder form (a gate when an operation moves:
// unchanged host counters per guest cycle).
class DirectBuilder {
  public:
    using Value = std::uint64_t;
    using Condition = bool;
    using Result = Control;

    DirectBuilder(Cpu& cpu, IntegerState& s, std::uint64_t pc) : cpu_{cpu}, s_{s}, pc_{pc} {}

    [[nodiscard]] Value gpr(unsigned r) const {
        return s_.gpr[r];
    }
    void set_gpr(unsigned r, Value v) {
        cpu_.set_gpr(r, v);
    }
    [[nodiscard]] Value hi() const {
        return s_.hi;
    }
    [[nodiscard]] Value lo() const {
        return s_.lo;
    }
    void set_hi(Value v) {
        s_.hi = v;
    }
    void set_lo(Value v) {
        s_.lo = v;
    }
    [[nodiscard]] static Value constant(std::uint64_t x) {
        return x;
    }
    [[nodiscard]] std::uint64_t pc() const {
        return pc_;
    }

    static Value add(Value a, Value b) {
        return a + b;
    }
    static Value sub(Value a, Value b) {
        return a - b;
    }
    static Value and_(Value a, Value b) {
        return a & b;
    }
    static Value or_(Value a, Value b) {
        return a | b;
    }
    static Value xor_(Value a, Value b) {
        return a ^ b;
    }
    static Value nor(Value a, Value b) {
        return ~(a | b);
    }
    static Value shift_left(Value v, unsigned n) {
        return v << n;
    }
    static Value shift_right_logical(Value v, unsigned n) {
        return v >> n;
    }
    static Value shift_right_arithmetic(Value v, unsigned n) {
        return static_cast<std::uint64_t>(as_signed(v) >> n);
    }
    static Value shift_left_by(Value v, Value n) {
        return v << n;
    }
    static Value shift_right_logical_by(Value v, Value n) {
        return v >> n;
    }
    static Value shift_right_arithmetic_by(Value v, Value n) {
        return static_cast<std::uint64_t>(as_signed(v) >> n);
    }
    static Value sign_extend_32(Value v) {
        return semantics::sign_extend_32(v);
    }
    static Value zero_extend_32(Value v) {
        return v & 0xffff'ffff;
    }

    static Condition equal(Value a, Value b) {
        return a == b;
    }
    static Condition not_equal(Value a, Value b) {
        return a != b;
    }
    static Condition less_signed(Value a, Value b) {
        return as_signed(a) < as_signed(b);
    }
    static Condition less_equal_signed(Value a, Value b) {
        return as_signed(a) <= as_signed(b);
    }
    static Condition greater_signed(Value a, Value b) {
        return as_signed(a) > as_signed(b);
    }
    static Condition greater_equal_signed(Value a, Value b) {
        return as_signed(a) >= as_signed(b);
    }
    static Condition less_unsigned(Value a, Value b) {
        return a < b;
    }
    static Value from_condition(Condition c) {
        return c ? 1 : 0;
    }
    static Condition add_overflows_32(Value a, Value b) {
        const std::int64_t sum = std::int64_t{static_cast<std::int32_t>(a)} +
                                 std::int64_t{static_cast<std::int32_t>(b)};
        return sum != static_cast<std::int32_t>(sum);
    }
    static Condition sub_overflows_32(Value a, Value b) {
        const std::int64_t difference = std::int64_t{static_cast<std::int32_t>(a)} -
                                        std::int64_t{static_cast<std::int32_t>(b)};
        return difference != static_cast<std::int32_t>(difference);
    }
    static Condition add_overflows_64(Value a, Value b) {
        const std::uint64_t sum = a + b;
        return (((a ^ sum) & (b ^ sum)) >> 63) != 0;
    }
    static Condition sub_overflows_64(Value a, Value b) {
        const std::uint64_t difference = a - b;
        return (((a ^ b) & (a ^ difference)) >> 63) != 0;
    }

    bool raise_if(Condition c, ExceptionCode code) {
        if (c) [[unlikely]] {
            cpu_.take_exception(Exception{.code = code});
        }
        return c;
    }
    template <AccessWidth W, bool SignExtend> std::optional<Value> load(Value address) {
        auto value = cpu_.load_as<W>(address);
        if (!value) [[unlikely]] {
            cpu_.take_exception(value.error());
            return std::nullopt;
        }
        if constexpr (SignExtend && W == AccessWidth::bits8) {
            return sign_extend_8(*value);
        } else if constexpr (SignExtend && W == AccessWidth::bits16) {
            return sign_extend_16(*value);
        } else if constexpr (SignExtend && W == AccessWidth::bits32) {
            return semantics::sign_extend_32(*value);
        } else {
            return *value;
        }
    }
    template <AccessWidth W> bool store(Value address, Value value) {
        if (auto result = cpu_.store_as<W>(address, static_cast<UnsignedOf<W>>(value)); !result)
            [[unlikely]] {
            cpu_.take_exception(result.error());
            return true;
        }
        return false;
    }
    [[nodiscard]] static Result raised() {
        return {.flow = Flow::exception};
    }
    [[nodiscard]] static Result sequential() {
        return {};
    }
    [[nodiscard]] static Result branch(Condition taken, std::uint64_t target, bool likely) {
        return Control{likely ? Flow::branch_likely : Flow::branch, taken, target};
    }
    [[nodiscard]] static Result jump(Value target) {
        return Control{Flow::branch, true, target};
    }

  private:
    Cpu& cpu_;
    IntegerState& s_;
    std::uint64_t pc_;
};

class Executor {
  public:
    Executor(Cpu& cpu, const DecodedInstruction& instruction, std::uint64_t pc)
        : cpu_{cpu}, s_{cpu.state()}, op_{instruction.opcode}, i_{instruction.instruction},
          pc_{pc} {}

    Result execute();

  private:
    [[nodiscard]] std::uint64_t rs() const {
        return s_.gpr[i_.rs()];
    }
    [[nodiscard]] std::uint64_t rt() const {
        return s_.gpr[i_.rt()];
    }
    [[nodiscard]] std::uint32_t rs32() const {
        return static_cast<std::uint32_t>(rs());
    }
    [[nodiscard]] std::uint32_t rt32() const {
        return static_cast<std::uint32_t>(rt());
    }
    [[nodiscard]] std::int64_t rs_signed() const {
        return as_signed(rs());
    }
    [[nodiscard]] std::uint64_t address() const {
        return rs() + i_.signed_immediate();
    }

    // Takes `exception` for this instruction (Cpu::take_exception) and reports it.
    Result fail(const Exception& exception) {
        cpu_.take_exception(exception);
        return {.flow = Flow::exception};
    }
    Result raise(ExceptionCode code) {
        return fail(Exception{.code = code});
    }
    Result reserved() {
        return raise(ExceptionCode::reserved_instruction);
    }
    Result unusable(std::uint8_t unit) {
        return fail(Exception{.code = ExceptionCode::coprocessor_unusable, .coprocessor = unit});
    }
    // The FPU (fpu.cpp) still returns ExecutionResult.
    Result from_fpu(const ExecutionResult& result) {
        return result ? *result : fail(result.error());
    }

    DirectBuilder direct() {
        return DirectBuilder{cpu_, s_, pc_};
    }
    Result load_left_right(bool left, AccessWidth width);
    Result store_left_right(bool left, AccessWidth width);
    Result load_linked(AccessWidth width);
    Result store_conditional(AccessWidth width);
    Result trap(bool condition);

    Cpu& cpu_;
    IntegerState& s_;
    DecodedOpcode op_;
    Instruction i_;
    std::uint64_t pc_;
};

[[gnu::always_inline]] inline Result Executor::trap(bool condition) {
    if (condition) {
        return raise(ExceptionCode::trap);
    }
    return sequential();
}

// LWL, LWR, LDL, LDR (ISA A-97 to A-102, A-81 to A-86). `k` is the byte offset in
// big-endian numbering, which makes both byte orders share one formula.
[[gnu::always_inline]] inline Result Executor::load_left_right(bool left, AccessWidth width) {
    const std::uint64_t address_value = address();
    const std::uint64_t size = byte_count(width);
    auto translation = cpu_.translate(address_value, AccessKind::load);
    if (!translation) {
        return fail(translation.error());
    }
    Translation aligned = *translation;
    aligned.address.value &= ~(size - 1);
    auto memory = cpu_.read(aligned, width, AccessKind::load);
    if (!memory) {
        return fail(memory.error());
    }
    const unsigned last = static_cast<unsigned>(size) - 1;
    const unsigned k = static_cast<unsigned>(address_value & last) ^ (cpu_.big_endian() ? 0 : last);

    if (width == AccessWidth::bits64) {
        std::uint64_t result = 0;
        if (left) {
            const unsigned shift = 8 * k;
            const std::uint64_t keep = shift == 0 ? 0 : (std::uint64_t{1} << shift) - 1;
            result = (*memory << shift) | (rt() & keep);
        } else {
            const unsigned shift = 8 * (last - k);
            const std::uint64_t keep = shift == 0 ? 0 : ~(~std::uint64_t{0} >> shift);
            result = (*memory >> shift) | (rt() & keep);
        }
        cpu_.set_gpr(i_.rt(), result);
        return sequential();
    }

    const auto word = static_cast<std::uint32_t>(*memory);
    const auto old = static_cast<std::uint32_t>(rt());
    if (left) {
        const unsigned shift = 8 * k;
        const std::uint32_t keep = shift == 0 ? 0 : (1u << shift) - 1;
        cpu_.set_gpr(i_.rt(), sign_extend_32((word << shift) | (old & keep)));
    } else {
        const unsigned shift = 8 * (last - k);
        const std::uint32_t keep = shift == 0 ? 0 : ~(0xffff'ffffu >> shift);
        const std::uint32_t merged = (word >> shift) | (old & keep);
        if (k == last) {
            // The word's sign bit was loaded: sign-extend (ISA A-101).
            cpu_.set_gpr(i_.rt(), sign_extend_32(merged));
        } else {
            // hypothesis: bits 63:32 are unchanged when bit 31 is not loaded; the ISA allows
            // this or copying the unloaded bit 31, and the UM does not say which.
            cpu_.set_gpr(i_.rt(), (rt() & 0xffff'ffff'0000'0000) | merged);
        }
    }
    return sequential();
}

// SWL, SWR, SDL, SDR. The bytes that change are written one at a time; see MIPS.adoc for
// why partial-word stores are expressed as byte writes.
[[gnu::always_inline]] inline Result Executor::store_left_right(bool left, AccessWidth width) {
    const std::uint64_t address_value = address();
    const std::uint64_t size = byte_count(width);
    auto translation = cpu_.translate(address_value, AccessKind::store);
    if (!translation) {
        return fail(translation.error());
    }
    const unsigned last = static_cast<unsigned>(size) - 1;
    const bool big = cpu_.big_endian();
    const unsigned k = static_cast<unsigned>(address_value & last) ^ (big ? 0 : last);
    const std::uint64_t value = width == AccessWidth::bits64 ? rt() : (rt() & 0xffff'ffff);
    const unsigned bits = 8 * static_cast<unsigned>(size);
    // Merged unit in big-endian numbering; positions [first, end) are written.
    const std::uint64_t merged = left ? value >> (8 * k) : value << (8 * (last - k));
    const unsigned first = left ? k : 0;
    const unsigned end = left ? last + 1 : k + 1;
    for (unsigned j = first; j < end; ++j) {
        const std::uint64_t byte = (merged >> (bits - 8 - 8 * j)) & 0xff;
        Translation target = *translation;
        target.address.value = (target.address.value & ~(size - 1)) + (big ? j : last - j);
        if (auto result = cpu_.write(target, AccessWidth::bits8, byte); !result) {
            return fail(result.error());
        }
    }
    return sequential();
}

[[gnu::always_inline]] inline Result Executor::load_linked(AccessWidth width) {
    auto b = direct();
    auto result = width == AccessWidth::bits32 ? operations::lw(b, i_) : operations::ld(b, i_);
    if (result.flow == Flow::exception) {
        return result;
    }
    // LLAddr is a scratch register on the R10000 and is not written (UM 14.15).
    s_.ll_bit = true;
    return sequential();
}

[[gnu::always_inline]] inline Result Executor::store_conditional(AccessWidth width) {
    const std::uint64_t address_value = address();
    if (address_value % byte_count(width) != 0) {
        return fail(
            Exception{.code = ExceptionCode::address_error_store, .bad_address = address_value});
    }
    auto translation = cpu_.translate(address_value, AccessKind::store);
    if (!translation) {
        return fail(translation.error());
    }
    if (s_.ll_bit) {
        const std::uint64_t value = width == AccessWidth::bits64 ? rt() : rt() & 0xffff'ffff;
        if (auto result = cpu_.write(*translation, width, value); !result) {
            return fail(result.error());
        }
    }
    cpu_.set_gpr(i_.rt(), s_.ll_bit ? 1 : 0);
    return sequential();
}

// MIPS III operations: reserved unless 64-bit operations are enabled (UM 17 "Reserved
// Instruction Exception"). Checked before anything else the operation does. A table, not a
// switch: measured, the switch compiled to a second indirect jump per instruction.
inline constexpr auto needs_64bit_operations = [] {
    std::array<bool, std::to_underlying(DecodedOpcode::reserved) + 1> table{};
    for (const DecodedOpcode op : mips3_operations) {
        table[std::to_underlying(op)] = true;
    }
    return table;
}();

[[gnu::always_inline]] inline Result Executor::execute() {
    if (needs_64bit_operations[std::to_underlying(op_)] && !cpu_.allows_64bit_operations()) {
        return reserved();
    }
    switch (op_) {
    case DecodedOpcode::sll: {
        auto b = direct();
        return operations::sll(b, i_);
    }
    case DecodedOpcode::movci: // MOVF, MOVT (MIPS IV)
        if (!cpu_.allows_mips4()) {
            return reserved();
        }
        if (!cpu_.coprocessor_usable(1)) {
            return unusable(1);
        }
        if (fpu_condition(cpu_, (i_.rt() >> 2) & 7) == ((i_.rt() & 1) != 0)) {
            cpu_.set_gpr(i_.rd(), rs());
        }
        return sequential();
    case DecodedOpcode::srl: {
        auto b = direct();
        return operations::srl(b, i_);
    }
    case DecodedOpcode::sra: {
        auto b = direct();
        return operations::sra(b, i_);
    }
    case DecodedOpcode::sllv: {
        auto b = direct();
        return operations::sllv(b, i_);
    }
    case DecodedOpcode::srlv: {
        auto b = direct();
        return operations::srlv(b, i_);
    }
    case DecodedOpcode::srav: {
        auto b = direct();
        return operations::srav(b, i_);
    }
    case DecodedOpcode::jr: {
        auto b = direct();
        return operations::jr(b, i_);
    }
    case DecodedOpcode::jalr: {
        auto b = direct();
        return operations::jalr(b, i_);
    }
    case DecodedOpcode::movz: // MOVZ (MIPS IV)
    case DecodedOpcode::movn: // MOVN (MIPS IV)
        if (!cpu_.allows_mips4()) {
            return reserved();
        }
        if ((rt() == 0) == (op_ == DecodedOpcode::movz)) {
            cpu_.set_gpr(i_.rd(), rs());
        }
        return sequential();
    case DecodedOpcode::syscall:
        return raise(ExceptionCode::syscall);
    case DecodedOpcode::break_:
        return raise(ExceptionCode::breakpoint);
    case DecodedOpcode::sync: // SYNC: references are already performed in program order.
        return sequential();
    case DecodedOpcode::mfhi: {
        auto b = direct();
        return operations::mfhi(b, i_);
    }
    case DecodedOpcode::mthi: {
        auto b = direct();
        return operations::mthi(b, i_);
    }
    case DecodedOpcode::mflo: {
        auto b = direct();
        return operations::mflo(b, i_);
    }
    case DecodedOpcode::mtlo: {
        auto b = direct();
        return operations::mtlo(b, i_);
    }
    case DecodedOpcode::dsllv: {
        auto b = direct();
        return operations::dsllv(b, i_);
    }
    case DecodedOpcode::dsrlv: {
        auto b = direct();
        return operations::dsrlv(b, i_);
    }
    case DecodedOpcode::dsrav: {
        auto b = direct();
        return operations::dsrav(b, i_);
    }
    case DecodedOpcode::mult: { // MULT
        const std::int64_t product =
            std::int64_t{static_cast<std::int32_t>(rs32())} * static_cast<std::int32_t>(rt32());
        s_.lo = sign_extend_32(static_cast<std::uint64_t>(product));
        s_.hi = sign_extend_32(static_cast<std::uint64_t>(product) >> 32);
        return sequential();
    }
    case DecodedOpcode::multu: { // MULTU
        const std::uint64_t product = std::uint64_t{rs32()} * rt32();
        s_.lo = sign_extend_32(product);
        s_.hi = sign_extend_32(product >> 32);
        return sequential();
    }
    case DecodedOpcode::div: { // DIV
        const auto dividend = static_cast<std::int32_t>(rs32());
        const auto divisor = static_cast<std::int32_t>(rt32());
        // hypothesis: a zero divisor leaves HI and LO unchanged (the ISA result is
        // UNPREDICTABLE and raises no exception).
        if (divisor == 0) {
            return sequential();
        }
        if (dividend == std::numeric_limits<std::int32_t>::min() && divisor == -1) {
            s_.lo = sign_extend_32(static_cast<std::uint32_t>(dividend));
            s_.hi = 0;
            return sequential();
        }
        s_.lo = sign_extend_32(static_cast<std::uint32_t>(dividend / divisor));
        s_.hi = sign_extend_32(static_cast<std::uint32_t>(dividend % divisor));
        return sequential();
    }
    case DecodedOpcode::divu: // DIVU
        if (rt32() == 0) {
            return sequential();
        }
        s_.lo = sign_extend_32(rs32() / rt32());
        s_.hi = sign_extend_32(rs32() % rt32());
        return sequential();
    case DecodedOpcode::dmult: { // DMULT
        const Product p = multiply_signed(rs(), rt());
        s_.hi = p.hi;
        s_.lo = p.lo;
        return sequential();
    }
    case DecodedOpcode::dmultu: { // DMULTU
        const Product p = multiply_unsigned(rs(), rt());
        s_.hi = p.hi;
        s_.lo = p.lo;
        return sequential();
    }
    case DecodedOpcode::ddiv: { // DDIV
        const std::int64_t dividend = as_signed(rs());
        const std::int64_t divisor = as_signed(rt());
        if (divisor == 0) {
            return sequential();
        }
        if (dividend == std::numeric_limits<std::int64_t>::min() && divisor == -1) {
            s_.lo = rs();
            s_.hi = 0;
            return sequential();
        }
        s_.lo = static_cast<std::uint64_t>(dividend / divisor);
        s_.hi = static_cast<std::uint64_t>(dividend % divisor);
        return sequential();
    }
    case DecodedOpcode::ddivu: // DDIVU
        if (rt() == 0) {
            return sequential();
        }
        s_.lo = rs() / rt();
        s_.hi = rs() % rt();
        return sequential();
    case DecodedOpcode::add: {
        auto b = direct();
        return operations::add(b, i_);
    }
    case DecodedOpcode::addu: {
        auto b = direct();
        return operations::addu(b, i_);
    }
    case DecodedOpcode::sub: {
        auto b = direct();
        return operations::sub(b, i_);
    }
    case DecodedOpcode::subu: {
        auto b = direct();
        return operations::subu(b, i_);
    }
    case DecodedOpcode::and_: {
        auto b = direct();
        return operations::and_(b, i_);
    }
    case DecodedOpcode::or_: {
        auto b = direct();
        return operations::or_(b, i_);
    }
    case DecodedOpcode::xor_: {
        auto b = direct();
        return operations::xor_(b, i_);
    }
    case DecodedOpcode::nor: {
        auto b = direct();
        return operations::nor(b, i_);
    }
    case DecodedOpcode::slt: {
        auto b = direct();
        return operations::slt(b, i_);
    }
    case DecodedOpcode::sltu: {
        auto b = direct();
        return operations::sltu(b, i_);
    }
    case DecodedOpcode::dadd: {
        auto b = direct();
        return operations::dadd(b, i_);
    }
    case DecodedOpcode::daddu: {
        auto b = direct();
        return operations::daddu(b, i_);
    }
    case DecodedOpcode::dsub: {
        auto b = direct();
        return operations::dsub(b, i_);
    }
    case DecodedOpcode::dsubu: {
        auto b = direct();
        return operations::dsubu(b, i_);
    }
    case DecodedOpcode::tge:
        return trap(as_signed(rs()) >= as_signed(rt())); // TGE
    case DecodedOpcode::tgeu:
        return trap(rs() >= rt()); // TGEU
    case DecodedOpcode::tlt:
        return trap(as_signed(rs()) < as_signed(rt())); // TLT
    case DecodedOpcode::tltu:
        return trap(rs() < rt()); // TLTU
    case DecodedOpcode::teq:
        return trap(rs() == rt()); // TEQ
    case DecodedOpcode::tne:
        return trap(rs() != rt()); // TNE
    case DecodedOpcode::dsll: {
        auto b = direct();
        return operations::dsll(b, i_);
    }
    case DecodedOpcode::dsrl: {
        auto b = direct();
        return operations::dsrl(b, i_);
    }
    case DecodedOpcode::dsra: {
        auto b = direct();
        return operations::dsra(b, i_);
    }
    case DecodedOpcode::dsll32: {
        auto b = direct();
        return operations::dsll32(b, i_);
    }
    case DecodedOpcode::dsrl32: {
        auto b = direct();
        return operations::dsrl32(b, i_);
    }
    case DecodedOpcode::dsra32: {
        auto b = direct();
        return operations::dsra32(b, i_);
    }
    case DecodedOpcode::bltz: {
        auto b = direct();
        return operations::bltz(b, i_);
    }
    case DecodedOpcode::bgez: {
        auto b = direct();
        return operations::bgez(b, i_);
    }
    case DecodedOpcode::bltzl: {
        auto b = direct();
        return operations::bltzl(b, i_);
    }
    case DecodedOpcode::bgezl: {
        auto b = direct();
        return operations::bgezl(b, i_);
    }
    case DecodedOpcode::tgei:
        return trap(rs_signed() >= as_signed(i_.signed_immediate())); // TGEI
    case DecodedOpcode::tgeiu:
        return trap(rs() >= i_.signed_immediate()); // TGEIU
    case DecodedOpcode::tlti:
        return trap(rs_signed() < as_signed(i_.signed_immediate())); // TLTI
    case DecodedOpcode::tltiu:
        return trap(rs() < i_.signed_immediate()); // TLTIU
    case DecodedOpcode::teqi:
        return trap(rs() == i_.signed_immediate()); // TEQI
    case DecodedOpcode::tnei:
        return trap(rs() != i_.signed_immediate()); // TNEI
    case DecodedOpcode::bltzal:
    case DecodedOpcode::bgezal:
    case DecodedOpcode::bltzall:
    case DecodedOpcode::bgezall: {
        auto b = direct();
        return operations::branch_and_link(b, i_);
    }
    case DecodedOpcode::mfc0:
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        cpu_.set_gpr(i_.rt(), cpu_.mfc0(i_.rd()));
        return sequential();
    case DecodedOpcode::dmfc0:
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        if (!cpu_.allows_64bit_operations()) {
            return reserved();
        }
        cpu_.set_gpr(i_.rt(), cpu_.dmfc0(i_.rd()));
        return sequential();
    case DecodedOpcode::mtc0:
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        cpu_.mtc0(i_.rd(), rt());
        return sequential();
    case DecodedOpcode::dmtc0:
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        if (!cpu_.allows_64bit_operations()) {
            return reserved();
        }
        cpu_.dmtc0(i_.rd(), rt());
        return sequential();
    case DecodedOpcode::tlbr:
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        cpu_.tlb_read();
        return sequential();
    case DecodedOpcode::tlbwi:
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        cpu_.tlb_write_indexed();
        return sequential();
    case DecodedOpcode::tlbwr:
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        cpu_.tlb_write_random();
        return sequential();
    case DecodedOpcode::tlbp:
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        cpu_.tlb_probe();
        return sequential();
    case DecodedOpcode::eret:
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        return Control{Flow::exception_return, true, cpu_.exception_return()};
    case DecodedOpcode::cop0_reserved:
        // Includes BC0x, which the R10000 treats as reserved (UM 14.25), RFE, and undefined
        // functions (UM 17.5; hypothesis: undefined functions are reserved).
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        return reserved();
    case DecodedOpcode::j: {
        auto b = direct();
        return operations::j(b, i_);
    }
    case DecodedOpcode::jal: {
        auto b = direct();
        return operations::jal(b, i_);
    }
    case DecodedOpcode::beq: {
        auto b = direct();
        return operations::beq(b, i_);
    }
    case DecodedOpcode::bne: {
        auto b = direct();
        return operations::bne(b, i_);
    }
    case DecodedOpcode::blez: {
        auto b = direct();
        return operations::blez(b, i_);
    }
    case DecodedOpcode::bgtz: {
        auto b = direct();
        return operations::bgtz(b, i_);
    }
    case DecodedOpcode::addi: {
        auto b = direct();
        return operations::addi(b, i_);
    }
    case DecodedOpcode::addiu: {
        auto b = direct();
        return operations::addiu(b, i_);
    }
    case DecodedOpcode::slti: {
        auto b = direct();
        return operations::slti(b, i_);
    }
    case DecodedOpcode::sltiu: {
        auto b = direct();
        return operations::sltiu(b, i_);
    }
    case DecodedOpcode::andi: {
        auto b = direct();
        return operations::andi(b, i_);
    }
    case DecodedOpcode::ori: {
        auto b = direct();
        return operations::ori(b, i_);
    }
    case DecodedOpcode::xori: {
        auto b = direct();
        return operations::xori(b, i_);
    }
    case DecodedOpcode::lui: {
        auto b = direct();
        return operations::lui(b, i_);
    }
    case DecodedOpcode::cop1: // COP1, LWC1, LDC1, SWC1, SDC1
        if (!cpu_.coprocessor_usable(1)) {
            return unusable(1);
        }
        return from_fpu(execute_fpu(cpu_, i_));
    case DecodedOpcode::cop1x: // COP1X (MIPS IV)
        if (!cpu_.allows_mips4()) {
            return reserved();
        }
        if (!cpu_.coprocessor_usable(1)) {
            return unusable(1);
        }
        return from_fpu(execute_fpu(cpu_, i_));
    case DecodedOpcode::cop2: // COP2, LWC2, LDC2, SWC2, SDC2
        // The R10000 has no coprocessor 2 (UM 17.7).
        if (!cpu_.coprocessor_usable(2)) {
            return unusable(2);
        }
        return reserved();
    case DecodedOpcode::beql: {
        auto b = direct();
        return operations::beql(b, i_);
    }
    case DecodedOpcode::bnel: {
        auto b = direct();
        return operations::bnel(b, i_);
    }
    case DecodedOpcode::blezl: {
        auto b = direct();
        return operations::blezl(b, i_);
    }
    case DecodedOpcode::bgtzl: {
        auto b = direct();
        return operations::bgtzl(b, i_);
    }
    case DecodedOpcode::daddi: {
        auto b = direct();
        return operations::daddi(b, i_);
    }
    case DecodedOpcode::daddiu: {
        auto b = direct();
        return operations::daddiu(b, i_);
    }
    case DecodedOpcode::ldl:
        return load_left_right(true, AccessWidth::bits64); // LDL
    case DecodedOpcode::ldr:
        return load_left_right(false, AccessWidth::bits64); // LDR
    case DecodedOpcode::lb: {
        auto b = direct();
        return operations::lb(b, i_);
    }
    case DecodedOpcode::lh: {
        auto b = direct();
        return operations::lh(b, i_);
    }
    case DecodedOpcode::lwl:
        return load_left_right(true, AccessWidth::bits32); // LWL
    case DecodedOpcode::lw: {
        auto b = direct();
        return operations::lw(b, i_);
    }
    case DecodedOpcode::lbu: {
        auto b = direct();
        return operations::lbu(b, i_);
    }
    case DecodedOpcode::lhu: {
        auto b = direct();
        return operations::lhu(b, i_);
    }
    case DecodedOpcode::lwr:
        return load_left_right(false, AccessWidth::bits32); // LWR
    case DecodedOpcode::lwu: {
        auto b = direct();
        return operations::lwu(b, i_);
    }
    case DecodedOpcode::sb: {
        auto b = direct();
        return operations::sb(b, i_);
    }
    case DecodedOpcode::sh: {
        auto b = direct();
        return operations::sh(b, i_);
    }
    case DecodedOpcode::swl:
        return store_left_right(true, AccessWidth::bits32); // SWL
    case DecodedOpcode::sw: {
        auto b = direct();
        return operations::sw(b, i_);
    }
    case DecodedOpcode::sdl:
        return store_left_right(true, AccessWidth::bits64); // SDL
    case DecodedOpcode::sdr:
        return store_left_right(false, AccessWidth::bits64); // SDR
    case DecodedOpcode::swr:
        return store_left_right(false, AccessWidth::bits32); // SWR
    case DecodedOpcode::cache:                               // CACHE
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        if (auto result = cpu_.cache(i_.rt(), address()); !result) {
            return fail(result.error());
        }
        return sequential();
    case DecodedOpcode::ll:
        return load_linked(AccessWidth::bits32); // LL
    case DecodedOpcode::pref: // PREF (MIPS IV): a hint with no architectural effect and no
                              // exceptions.
        if (!cpu_.allows_mips4()) {
            return reserved();
        }
        return sequential();
    case DecodedOpcode::lld:
        return load_linked(AccessWidth::bits64); // LLD
    case DecodedOpcode::ld: {
        auto b = direct();
        return operations::ld(b, i_);
    }
    case DecodedOpcode::sc:
        return store_conditional(AccessWidth::bits32); // SC
    case DecodedOpcode::scd:
        return store_conditional(AccessWidth::bits64); // SCD
    case DecodedOpcode::sd: {
        auto b = direct();
        return operations::sd(b, i_);
    }
    case DecodedOpcode::reserved:
        return reserved();
    }
    // decode produces only DecodedOpcode values, and every one has a case (-Wswitch): telling
    // the compiler so drops the jump table's range check (IR.adoc "Stage H", mechanism 3).
    std::unreachable();
}

} // namespace semantics

// execute (decoded.hpp), for inlining into an engine's loop.
[[gnu::always_inline]] inline Control execute_inline(Cpu& cpu,
                                                     const DecodedInstruction& instruction,
                                                     std::uint64_t pc) {
    return semantics::Executor{cpu, instruction, pc}.execute();
}

} // namespace ultraviolent::mips
