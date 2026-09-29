#include <ultraviolent/arch/mips/interpreter.hpp>

#include <ultraviolent/arch/mips/fpu.hpp>
#include <ultraviolent/arch/mips/instruction.hpp>

#include <cstdint>
#include <expected>
#include <limits>

// Instruction semantics follow the MIPS IV Instruction Set, Revision 3.2 (ISA), with R10000
// specifics from the R10000 User's Manual, Version 2.0 (UM). Choices for behavior the ISA
// leaves UNPREDICTABLE are listed in doc/MIPS.adoc.
namespace ultraviolent::mips {

namespace {

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

using Result = ExecutionResult;

constexpr Control sequential() {
    return {};
}

std::unexpected<Exception> raise(ExceptionCode code) {
    return std::unexpected(Exception{.code = code});
}

std::unexpected<Exception> reserved() {
    return raise(ExceptionCode::reserved_instruction);
}

std::unexpected<Exception> unusable(std::uint8_t unit) {
    return std::unexpected(
        Exception{.code = ExceptionCode::coprocessor_unusable, .coprocessor = unit});
}

class Executor {
  public:
    Executor(Cpu& cpu, Instruction instruction)
        : cpu_{cpu}, s_{cpu.state()}, i_{instruction}, pc_{cpu.state().pc} {}

    Result execute();

  private:
    [[nodiscard]] std::uint64_t rs() const {
        return s_.gpr[i_.rs()];
    }
    [[nodiscard]] std::uint64_t rt() const {
        return s_.gpr[i_.rt()];
    }
    [[nodiscard]] std::uint64_t address() const {
        return rs() + i_.signed_immediate();
    }

    Result special();
    Result regimm();
    Result cop0();
    Result branch(bool condition, bool likely = false);
    template <AccessWidth W, bool SignExtend> Result load();
    template <AccessWidth W> Result store();
    Result load_left_right(bool left, AccessWidth width);
    Result store_left_right(bool left, AccessWidth width);
    Result load_linked(AccessWidth width);
    Result store_conditional(AccessWidth width);
    Result trap(bool condition);

    Cpu& cpu_;
    IntegerState& s_;
    Instruction i_;
    std::uint64_t pc_;
};

Result Executor::branch(bool condition, bool likely) {
    return Control{likely ? Flow::branch_likely : Flow::branch, condition,
                   pc_ + 4 + (i_.signed_immediate() << 2)};
}

Result Executor::trap(bool condition) {
    if (condition) {
        return raise(ExceptionCode::trap);
    }
    return sequential();
}

template <AccessWidth W, bool SignExtend> Result Executor::load() {
    auto value = cpu_.load_as<W>(address());
    if (!value) {
        return std::unexpected(value.error());
    }
    std::uint64_t result = *value;
    if constexpr (SignExtend) {
        if constexpr (W == AccessWidth::bits8) {
            result = sign_extend_8(result);
        } else if constexpr (W == AccessWidth::bits16) {
            result = sign_extend_16(result);
        } else if constexpr (W == AccessWidth::bits32) {
            result = sign_extend_32(result);
        }
    }
    cpu_.set_gpr(i_.rt(), result);
    return sequential();
}

template <AccessWidth W> Result Executor::store() {
    if (auto result = cpu_.store_as<W>(address(), static_cast<UnsignedOf<W>>(rt())); !result) {
        return std::unexpected(result.error());
    }
    return sequential();
}

// LWL, LWR, LDL, LDR (ISA A-97 to A-102, A-81 to A-86). `k` is the byte offset in
// big-endian numbering, which makes both byte orders share one formula.
Result Executor::load_left_right(bool left, AccessWidth width) {
    const std::uint64_t address_value = address();
    const std::uint64_t size = byte_count(width);
    auto translation = cpu_.translate(address_value, AccessKind::load);
    if (!translation) {
        return std::unexpected(translation.error());
    }
    Translation aligned = *translation;
    aligned.address.value &= ~(size - 1);
    auto memory = cpu_.read(aligned, width, AccessKind::load);
    if (!memory) {
        return std::unexpected(memory.error());
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
Result Executor::store_left_right(bool left, AccessWidth width) {
    const std::uint64_t address_value = address();
    const std::uint64_t size = byte_count(width);
    auto translation = cpu_.translate(address_value, AccessKind::store);
    if (!translation) {
        return std::unexpected(translation.error());
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
            return std::unexpected(result.error());
        }
    }
    return sequential();
}

Result Executor::load_linked(AccessWidth width) {
    auto result = width == AccessWidth::bits32 ? load<AccessWidth::bits32, true>()
                                               : load<AccessWidth::bits64, false>();
    if (!result) {
        return result;
    }
    // LLAddr is a scratch register on the R10000 and is not written (UM 14.15).
    s_.ll_bit = true;
    return sequential();
}

Result Executor::store_conditional(AccessWidth width) {
    const std::uint64_t address_value = address();
    if (address_value % byte_count(width) != 0) {
        return std::unexpected(
            Exception{.code = ExceptionCode::address_error_store, .bad_address = address_value});
    }
    auto translation = cpu_.translate(address_value, AccessKind::store);
    if (!translation) {
        return std::unexpected(translation.error());
    }
    if (s_.ll_bit) {
        const std::uint64_t value = width == AccessWidth::bits64 ? rt() : rt() & 0xffff'ffff;
        if (auto result = cpu_.write(*translation, width, value); !result) {
            return std::unexpected(result.error());
        }
    }
    cpu_.set_gpr(i_.rt(), s_.ll_bit ? 1 : 0);
    return sequential();
}

Result Executor::special() {
    const unsigned sa = i_.sa();
    const unsigned rd = i_.rd();
    const auto rs32 = static_cast<std::uint32_t>(rs());
    const auto rt32 = static_cast<std::uint32_t>(rt());

    // MIPS III doubleword operations (UM 17 "Reserved Instruction Exception").
    switch (i_.funct()) {
    case 20:
    case 22:
    case 23:
    case 28:
    case 29:
    case 30:
    case 31:
    case 44:
    case 45:
    case 46:
    case 47:
    case 56:
    case 58:
    case 59:
    case 60:
    case 62:
    case 63:
        if (!cpu_.allows_64bit_operations()) {
            return reserved();
        }
        break;
    default:
        break;
    }

    switch (i_.funct()) {
    case 0: // SLL
        cpu_.set_gpr(rd, sign_extend_32(rt32 << sa));
        return sequential();
    case 1: // MOVF, MOVT (MIPS IV)
        if (!cpu_.allows_mips4()) {
            return reserved();
        }
        if (!cpu_.coprocessor_usable(1)) {
            return unusable(1);
        }
        if (fpu_condition(cpu_, (i_.rt() >> 2) & 7) == ((i_.rt() & 1) != 0)) {
            cpu_.set_gpr(rd, rs());
        }
        return sequential();
    case 2: // SRL
        cpu_.set_gpr(rd, sign_extend_32(rt32 >> sa));
        return sequential();
    case 3: // SRA
        cpu_.set_gpr(
            rd, sign_extend_32(static_cast<std::uint32_t>(static_cast<std::int32_t>(rt32) >> sa)));
        return sequential();
    case 4: // SLLV
        cpu_.set_gpr(rd, sign_extend_32(rt32 << (rs32 & 31)));
        return sequential();
    case 6: // SRLV
        cpu_.set_gpr(rd, sign_extend_32(rt32 >> (rs32 & 31)));
        return sequential();
    case 7: // SRAV
        cpu_.set_gpr(rd, sign_extend_32(static_cast<std::uint32_t>(
                             static_cast<std::int32_t>(rt32) >> (rs32 & 31))));
        return sequential();
    case 8: // JR
        return Control{Flow::branch, true, rs()};
    case 9: { // JALR
        const std::uint64_t target = rs();
        cpu_.set_gpr(rd, pc_ + 8);
        return Control{Flow::branch, true, target};
    }
    case 10: // MOVZ (MIPS IV)
    case 11: // MOVN (MIPS IV)
        if (!cpu_.allows_mips4()) {
            return reserved();
        }
        if ((rt() == 0) == (i_.funct() == 10)) {
            cpu_.set_gpr(rd, rs());
        }
        return sequential();
    case 12:
        return raise(ExceptionCode::syscall);
    case 13:
        return raise(ExceptionCode::breakpoint);
    case 15: // SYNC: references are already performed in program order.
        return sequential();
    case 16: // MFHI
        cpu_.set_gpr(rd, s_.hi);
        return sequential();
    case 17: // MTHI
        s_.hi = rs();
        return sequential();
    case 18: // MFLO
        cpu_.set_gpr(rd, s_.lo);
        return sequential();
    case 19: // MTLO
        s_.lo = rs();
        return sequential();
    case 20: // DSLLV
        cpu_.set_gpr(rd, rt() << (rs() & 63));
        return sequential();
    case 22: // DSRLV
        cpu_.set_gpr(rd, rt() >> (rs() & 63));
        return sequential();
    case 23: // DSRAV
        cpu_.set_gpr(rd, static_cast<std::uint64_t>(as_signed(rt()) >> (rs() & 63)));
        return sequential();
    case 24: { // MULT
        const std::int64_t product =
            std::int64_t{static_cast<std::int32_t>(rs32)} * static_cast<std::int32_t>(rt32);
        s_.lo = sign_extend_32(static_cast<std::uint64_t>(product));
        s_.hi = sign_extend_32(static_cast<std::uint64_t>(product) >> 32);
        return sequential();
    }
    case 25: { // MULTU
        const std::uint64_t product = std::uint64_t{rs32} * rt32;
        s_.lo = sign_extend_32(product);
        s_.hi = sign_extend_32(product >> 32);
        return sequential();
    }
    case 26: { // DIV
        const auto dividend = static_cast<std::int32_t>(rs32);
        const auto divisor = static_cast<std::int32_t>(rt32);
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
    case 27: // DIVU
        if (rt32 == 0) {
            return sequential();
        }
        s_.lo = sign_extend_32(rs32 / rt32);
        s_.hi = sign_extend_32(rs32 % rt32);
        return sequential();
    case 28: { // DMULT
        const Product p = multiply_signed(rs(), rt());
        s_.hi = p.hi;
        s_.lo = p.lo;
        return sequential();
    }
    case 29: { // DMULTU
        const Product p = multiply_unsigned(rs(), rt());
        s_.hi = p.hi;
        s_.lo = p.lo;
        return sequential();
    }
    case 30: { // DDIV
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
    case 31: // DDIVU
        if (rt() == 0) {
            return sequential();
        }
        s_.lo = rs() / rt();
        s_.hi = rs() % rt();
        return sequential();
    case 32: { // ADD
        const std::int64_t sum =
            std::int64_t{static_cast<std::int32_t>(rs32)} + static_cast<std::int32_t>(rt32);
        if (sum != static_cast<std::int32_t>(sum)) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(rd, static_cast<std::uint64_t>(sum));
        return sequential();
    }
    case 33: // ADDU
        cpu_.set_gpr(rd, sign_extend_32(rs32 + rt32));
        return sequential();
    case 34: { // SUB
        const std::int64_t difference =
            std::int64_t{static_cast<std::int32_t>(rs32)} - static_cast<std::int32_t>(rt32);
        if (difference != static_cast<std::int32_t>(difference)) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(rd, static_cast<std::uint64_t>(difference));
        return sequential();
    }
    case 35: // SUBU
        cpu_.set_gpr(rd, sign_extend_32(rs32 - rt32));
        return sequential();
    case 36:
        cpu_.set_gpr(rd, rs() & rt());
        return sequential();
    case 37:
        cpu_.set_gpr(rd, rs() | rt());
        return sequential();
    case 38:
        cpu_.set_gpr(rd, rs() ^ rt());
        return sequential();
    case 39:
        cpu_.set_gpr(rd, ~(rs() | rt()));
        return sequential();
    case 42: // SLT
        cpu_.set_gpr(rd, as_signed(rs()) < as_signed(rt()) ? 1 : 0);
        return sequential();
    case 43: // SLTU
        cpu_.set_gpr(rd, rs() < rt() ? 1 : 0);
        return sequential();
    case 44: { // DADD
        const std::uint64_t sum = rs() + rt();
        if ((((rs() ^ sum) & (rt() ^ sum)) >> 63) != 0) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(rd, sum);
        return sequential();
    }
    case 45: // DADDU
        cpu_.set_gpr(rd, rs() + rt());
        return sequential();
    case 46: { // DSUB
        const std::uint64_t difference = rs() - rt();
        if ((((rs() ^ rt()) & (rs() ^ difference)) >> 63) != 0) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(rd, difference);
        return sequential();
    }
    case 47: // DSUBU
        cpu_.set_gpr(rd, rs() - rt());
        return sequential();
    case 48:
        return trap(as_signed(rs()) >= as_signed(rt())); // TGE
    case 49:
        return trap(rs() >= rt()); // TGEU
    case 50:
        return trap(as_signed(rs()) < as_signed(rt())); // TLT
    case 51:
        return trap(rs() < rt()); // TLTU
    case 52:
        return trap(rs() == rt()); // TEQ
    case 54:
        return trap(rs() != rt()); // TNE
    case 56:                       // DSLL
        cpu_.set_gpr(rd, rt() << sa);
        return sequential();
    case 58: // DSRL
        cpu_.set_gpr(rd, rt() >> sa);
        return sequential();
    case 59: // DSRA
        cpu_.set_gpr(rd, static_cast<std::uint64_t>(as_signed(rt()) >> sa));
        return sequential();
    case 60: // DSLL32
        cpu_.set_gpr(rd, rt() << (sa + 32));
        return sequential();
    case 62: // DSRL32
        cpu_.set_gpr(rd, rt() >> (sa + 32));
        return sequential();
    case 63: // DSRA32
        cpu_.set_gpr(rd, static_cast<std::uint64_t>(as_signed(rt()) >> (sa + 32)));
        return sequential();
    default:
        return reserved();
    }
}

Result Executor::regimm() {
    const std::int64_t value = as_signed(rs());
    const std::uint64_t immediate = i_.signed_immediate();
    switch (i_.rt()) {
    case 0:
        return branch(value < 0); // BLTZ
    case 1:
        return branch(value >= 0); // BGEZ
    case 2:
        return branch(value < 0, true); // BLTZL
    case 3:
        return branch(value >= 0, true); // BGEZL
    case 8:
        return trap(value >= as_signed(immediate)); // TGEI
    case 9:
        return trap(rs() >= immediate); // TGEIU
    case 10:
        return trap(value < as_signed(immediate)); // TLTI
    case 11:
        return trap(rs() < immediate); // TLTIU
    case 12:
        return trap(rs() == immediate); // TEQI
    case 14:
        return trap(rs() != immediate); // TNEI
    case 16:
    case 17:
    case 18:
    case 19: { // BLTZAL, BGEZAL, BLTZALL, BGEZALL: the link is written whether or not taken.
        const bool condition = (i_.rt() & 1) != 0 ? value >= 0 : value < 0;
        cpu_.set_gpr(31, pc_ + 8);
        return branch(condition, i_.rt() >= 18);
    }
    default:
        return reserved();
    }
}

Result Executor::cop0() {
    if (!cpu_.coprocessor_usable(0)) {
        return unusable(0);
    }
    const unsigned rd = i_.rd();
    switch (i_.rs()) {
    case 0: // MFC0
        cpu_.set_gpr(i_.rt(), cpu_.mfc0(rd));
        return sequential();
    case 1: // DMFC0
        if (!cpu_.allows_64bit_operations()) {
            return reserved();
        }
        cpu_.set_gpr(i_.rt(), cpu_.dmfc0(rd));
        return sequential();
    case 4: // MTC0
        cpu_.mtc0(rd, rt());
        return sequential();
    case 5: // DMTC0
        if (!cpu_.allows_64bit_operations()) {
            return reserved();
        }
        cpu_.dmtc0(rd, rt());
        return sequential();
    default:
        break;
    }
    if (i_.rs() < 16) {
        // Includes BC0x, which the R10000 treats as reserved (UM 14.25).
        return reserved();
    }
    switch (i_.funct()) {
    case 1:
        cpu_.tlb_read();
        return sequential();
    case 2:
        cpu_.tlb_write_indexed();
        return sequential();
    case 6:
        cpu_.tlb_write_random();
        return sequential();
    case 8:
        cpu_.tlb_probe();
        return sequential();
    case 24: // ERET
        return Control{Flow::exception_return, true, cpu_.exception_return()};
    default:
        // RFE and undefined functions (UM 17.5). hypothesis: undefined functions are reserved.
        return reserved();
    }
}

Result Executor::execute() {
    const unsigned op = i_.opcode();

    // MIPS III doubleword loads, stores, and immediates.
    switch (op) {
    case 24:
    case 25:
    case 26:
    case 27:
    case 39:
    case 44:
    case 45:
    case 52:
    case 55:
    case 60:
    case 63:
        if (!cpu_.allows_64bit_operations()) {
            return reserved();
        }
        break;
    default:
        break;
    }

    switch (op) {
    case 0:
        return special();
    case 1:
        return regimm();
    case 2:   // J
    case 3: { // JAL
        const std::uint64_t target = ((pc_ + 4) & ~std::uint64_t{0x0fff'ffff}) | (i_.target() << 2);
        if (op == 3) {
            cpu_.set_gpr(31, pc_ + 8);
        }
        return Control{Flow::branch, true, target};
    }
    case 4:
        return branch(rs() == rt()); // BEQ
    case 5:
        return branch(rs() != rt()); // BNE
    case 6:
        return branch(as_signed(rs()) <= 0); // BLEZ
    case 7:
        return branch(as_signed(rs()) > 0); // BGTZ
    case 8: {                               // ADDI
        const std::int64_t sum = std::int64_t{static_cast<std::int32_t>(rs())} +
                                 static_cast<std::int16_t>(i_.immediate());
        if (sum != static_cast<std::int32_t>(sum)) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(i_.rt(), static_cast<std::uint64_t>(sum));
        return sequential();
    }
    case 9: // ADDIU
        cpu_.set_gpr(i_.rt(), sign_extend_32(rs() + i_.signed_immediate()));
        return sequential();
    case 10: // SLTI
        cpu_.set_gpr(i_.rt(), as_signed(rs()) < as_signed(i_.signed_immediate()) ? 1 : 0);
        return sequential();
    case 11: // SLTIU
        cpu_.set_gpr(i_.rt(), rs() < i_.signed_immediate() ? 1 : 0);
        return sequential();
    case 12: // ANDI
        cpu_.set_gpr(i_.rt(), rs() & i_.immediate());
        return sequential();
    case 13: // ORI
        cpu_.set_gpr(i_.rt(), rs() | i_.immediate());
        return sequential();
    case 14: // XORI
        cpu_.set_gpr(i_.rt(), rs() ^ i_.immediate());
        return sequential();
    case 15: // LUI
        cpu_.set_gpr(i_.rt(), sign_extend_32(i_.immediate() << 16));
        return sequential();
    case 16:
        return cop0();
    case 17: // COP1
    case 49: // LWC1
    case 53: // LDC1
    case 57: // SWC1
    case 61: // SDC1
        if (!cpu_.coprocessor_usable(1)) {
            return unusable(1);
        }
        return execute_fpu(cpu_, i_);
    case 19: // COP1X (MIPS IV)
        if (!cpu_.allows_mips4()) {
            return reserved();
        }
        if (!cpu_.coprocessor_usable(1)) {
            return unusable(1);
        }
        return execute_fpu(cpu_, i_);
    case 18: // COP2
    case 50: // LWC2
    case 54: // LDC2
    case 58: // SWC2
    case 62: // SDC2
        // The R10000 has no coprocessor 2 (UM 17.7).
        if (!cpu_.coprocessor_usable(2)) {
            return unusable(2);
        }
        return reserved();
    case 20:
        return branch(rs() == rt(), true); // BEQL
    case 21:
        return branch(rs() != rt(), true); // BNEL
    case 22:
        return branch(as_signed(rs()) <= 0, true); // BLEZL
    case 23:
        return branch(as_signed(rs()) > 0, true); // BGTZL
    case 24: {                                    // DADDI
        const std::uint64_t immediate = i_.signed_immediate();
        const std::uint64_t sum = rs() + immediate;
        if ((((rs() ^ sum) & (immediate ^ sum)) >> 63) != 0) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(i_.rt(), sum);
        return sequential();
    }
    case 25: // DADDIU
        cpu_.set_gpr(i_.rt(), rs() + i_.signed_immediate());
        return sequential();
    case 26:
        return load_left_right(true, AccessWidth::bits64); // LDL
    case 27:
        return load_left_right(false, AccessWidth::bits64); // LDR
    case 32:
        return load<AccessWidth::bits8, true>(); // LB
    case 33:
        return load<AccessWidth::bits16, true>(); // LH
    case 34:
        return load_left_right(true, AccessWidth::bits32); // LWL
    case 35:
        return load<AccessWidth::bits32, true>(); // LW
    case 36:
        return load<AccessWidth::bits8, false>(); // LBU
    case 37:
        return load<AccessWidth::bits16, false>(); // LHU
    case 38:
        return load_left_right(false, AccessWidth::bits32); // LWR
    case 39:
        return load<AccessWidth::bits32, false>(); // LWU
    case 40:
        return store<AccessWidth::bits8>(); // SB
    case 41:
        return store<AccessWidth::bits16>(); // SH
    case 42:
        return store_left_right(true, AccessWidth::bits32); // SWL
    case 43:
        return store<AccessWidth::bits32>(); // SW
    case 44:
        return store_left_right(true, AccessWidth::bits64); // SDL
    case 45:
        return store_left_right(false, AccessWidth::bits64); // SDR
    case 46:
        return store_left_right(false, AccessWidth::bits32); // SWR
    case 47:                                                 // CACHE
        if (!cpu_.coprocessor_usable(0)) {
            return unusable(0);
        }
        if (auto result = cpu_.cache(i_.rt(), address()); !result) {
            return std::unexpected(result.error());
        }
        return sequential();
    case 48:
        return load_linked(AccessWidth::bits32); // LL
    case 51: // PREF (MIPS IV): a hint with no architectural effect and no exceptions.
        if (!cpu_.allows_mips4()) {
            return reserved();
        }
        return sequential();
    case 52:
        return load_linked(AccessWidth::bits64); // LLD
    case 55:
        return load<AccessWidth::bits64, false>(); // LD
    case 56:
        return store_conditional(AccessWidth::bits32); // SC
    case 60:
        return store_conditional(AccessWidth::bits64); // SCD
    case 63:
        return store<AccessWidth::bits64>(); // SD
    default:
        return reserved();
    }
}

// One processor cycle; Interpreter::step and Interpreter::run_until both inline it.
[[gnu::always_inline]] inline void execute_cycle(Cpu& cpu) {
    if (cpu.service_pending_exceptions()) {
        cpu.end_cycle(false);
        return;
    }
    IntegerState& s = cpu.state();
    auto word = cpu.fetch(s.pc);
    if (!word) {
        cpu.take_exception(word.error());
        cpu.end_cycle(false);
        return;
    }
    auto result = Executor{cpu, Instruction{*word}}.execute();
    if (!result) {
        cpu.take_exception(result.error());
        cpu.end_cycle(false);
        return;
    }
    switch (result->flow) {
    case Flow::sequential:
        s.pc = s.next_pc;
        s.next_pc = s.pc + 4;
        s.delay_slot = false;
        break;
    case Flow::branch:
        // The delay slot executes whether or not the branch is taken.
        s.pc = s.next_pc;
        s.next_pc = result->taken ? result->target : s.pc + 4;
        s.delay_slot = true;
        break;
    case Flow::branch_likely:
        if (result->taken) {
            s.pc = s.next_pc;
            s.next_pc = result->target;
            s.delay_slot = true;
        } else {
            // A branch-likely that is not taken nullifies its delay slot.
            s.pc = s.next_pc + 4;
            s.next_pc = s.pc + 4;
            s.delay_slot = false;
        }
        break;
    case Flow::exception_return:
        // ERET has no delay slot (UM 14.30).
        s.pc = result->target;
        s.next_pc = s.pc + 4;
        s.delay_slot = false;
        break;
    }
    cpu.end_cycle(true);
}

} // namespace

void Interpreter::step() {
    execute_cycle(cpu_);
}

void Interpreter::run_until(const std::uint64_t& limit) {
    while (cpu_.cycles() < limit) {
        execute_cycle(cpu_);
    }
}

} // namespace ultraviolent::mips
