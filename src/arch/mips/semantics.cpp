#include <ultraviolent/arch/mips/decoded.hpp>

#include <ultraviolent/arch/mips/fpu.hpp>
#include <ultraviolent/arch/mips/instruction.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>

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
    DecodedOpcode op_;
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

// MIPS III operations: reserved unless 64-bit operations are enabled (UM 17 "Reserved
// Instruction Exception"). Checked before anything else the operation does. A table, not a
// switch: measured, the switch compiled to a second indirect jump per instruction.
constexpr auto needs_64bit_operations = [] {
    using enum DecodedOpcode;
    std::array<bool, std::to_underlying(reserved) + 1> table{};
    for (const DecodedOpcode op :
         {dsllv, dsrlv, dsrav, dmult, dmultu, ddiv,   ddivu,  dadd,  daddu,  dsub,
          dsubu, dsll,  dsrl,  dsra,  dsll32, dsrl32, dsra32, daddi, daddiu, ldl,
          ldr,   lwu,   sdl,   sdr,   lld,    ld,     scd,    sd}) {
        table[std::to_underlying(op)] = true;
    }
    return table;
}();

Result Executor::execute() {
    if (needs_64bit_operations[std::to_underlying(op_)] && !cpu_.allows_64bit_operations()) {
        return reserved();
    }
    switch (op_) {
    case DecodedOpcode::sll: // SLL
        cpu_.set_gpr(i_.rd(), sign_extend_32(rt32() << i_.sa()));
        return sequential();
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
    case DecodedOpcode::srl: // SRL
        cpu_.set_gpr(i_.rd(), sign_extend_32(rt32() >> i_.sa()));
        return sequential();
    case DecodedOpcode::sra: // SRA
        cpu_.set_gpr(i_.rd(), sign_extend_32(static_cast<std::uint32_t>(
                                  static_cast<std::int32_t>(rt32()) >> i_.sa())));
        return sequential();
    case DecodedOpcode::sllv: // SLLV
        cpu_.set_gpr(i_.rd(), sign_extend_32(rt32() << (rs32() & 31)));
        return sequential();
    case DecodedOpcode::srlv: // SRLV
        cpu_.set_gpr(i_.rd(), sign_extend_32(rt32() >> (rs32() & 31)));
        return sequential();
    case DecodedOpcode::srav: // SRAV
        cpu_.set_gpr(i_.rd(), sign_extend_32(static_cast<std::uint32_t>(
                                  static_cast<std::int32_t>(rt32()) >> (rs32() & 31))));
        return sequential();
    case DecodedOpcode::jr: // JR
        return Control{Flow::branch, true, rs()};
    case DecodedOpcode::jalr: { // JALR
        const std::uint64_t target = rs();
        cpu_.set_gpr(i_.rd(), pc_ + 8);
        return Control{Flow::branch, true, target};
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
    case DecodedOpcode::mfhi: // MFHI
        cpu_.set_gpr(i_.rd(), s_.hi);
        return sequential();
    case DecodedOpcode::mthi: // MTHI
        s_.hi = rs();
        return sequential();
    case DecodedOpcode::mflo: // MFLO
        cpu_.set_gpr(i_.rd(), s_.lo);
        return sequential();
    case DecodedOpcode::mtlo: // MTLO
        s_.lo = rs();
        return sequential();
    case DecodedOpcode::dsllv: // DSLLV
        cpu_.set_gpr(i_.rd(), rt() << (rs() & 63));
        return sequential();
    case DecodedOpcode::dsrlv: // DSRLV
        cpu_.set_gpr(i_.rd(), rt() >> (rs() & 63));
        return sequential();
    case DecodedOpcode::dsrav: // DSRAV
        cpu_.set_gpr(i_.rd(), static_cast<std::uint64_t>(as_signed(rt()) >> (rs() & 63)));
        return sequential();
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
    case DecodedOpcode::add: { // ADD
        const std::int64_t sum =
            std::int64_t{static_cast<std::int32_t>(rs32())} + static_cast<std::int32_t>(rt32());
        if (sum != static_cast<std::int32_t>(sum)) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(i_.rd(), static_cast<std::uint64_t>(sum));
        return sequential();
    }
    case DecodedOpcode::addu: // ADDU
        cpu_.set_gpr(i_.rd(), sign_extend_32(rs32() + rt32()));
        return sequential();
    case DecodedOpcode::sub: { // SUB
        const std::int64_t difference =
            std::int64_t{static_cast<std::int32_t>(rs32())} - static_cast<std::int32_t>(rt32());
        if (difference != static_cast<std::int32_t>(difference)) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(i_.rd(), static_cast<std::uint64_t>(difference));
        return sequential();
    }
    case DecodedOpcode::subu: // SUBU
        cpu_.set_gpr(i_.rd(), sign_extend_32(rs32() - rt32()));
        return sequential();
    case DecodedOpcode::and_:
        cpu_.set_gpr(i_.rd(), rs() & rt());
        return sequential();
    case DecodedOpcode::or_:
        cpu_.set_gpr(i_.rd(), rs() | rt());
        return sequential();
    case DecodedOpcode::xor_:
        cpu_.set_gpr(i_.rd(), rs() ^ rt());
        return sequential();
    case DecodedOpcode::nor:
        cpu_.set_gpr(i_.rd(), ~(rs() | rt()));
        return sequential();
    case DecodedOpcode::slt: // SLT
        cpu_.set_gpr(i_.rd(), as_signed(rs()) < as_signed(rt()) ? 1 : 0);
        return sequential();
    case DecodedOpcode::sltu: // SLTU
        cpu_.set_gpr(i_.rd(), rs() < rt() ? 1 : 0);
        return sequential();
    case DecodedOpcode::dadd: { // DADD
        const std::uint64_t sum = rs() + rt();
        if ((((rs() ^ sum) & (rt() ^ sum)) >> 63) != 0) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(i_.rd(), sum);
        return sequential();
    }
    case DecodedOpcode::daddu: // DADDU
        cpu_.set_gpr(i_.rd(), rs() + rt());
        return sequential();
    case DecodedOpcode::dsub: { // DSUB
        const std::uint64_t difference = rs() - rt();
        if ((((rs() ^ rt()) & (rs() ^ difference)) >> 63) != 0) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(i_.rd(), difference);
        return sequential();
    }
    case DecodedOpcode::dsubu: // DSUBU
        cpu_.set_gpr(i_.rd(), rs() - rt());
        return sequential();
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
    case DecodedOpcode::dsll:      // DSLL
        cpu_.set_gpr(i_.rd(), rt() << i_.sa());
        return sequential();
    case DecodedOpcode::dsrl: // DSRL
        cpu_.set_gpr(i_.rd(), rt() >> i_.sa());
        return sequential();
    case DecodedOpcode::dsra: // DSRA
        cpu_.set_gpr(i_.rd(), static_cast<std::uint64_t>(as_signed(rt()) >> i_.sa()));
        return sequential();
    case DecodedOpcode::dsll32: // DSLL32
        cpu_.set_gpr(i_.rd(), rt() << (i_.sa() + 32));
        return sequential();
    case DecodedOpcode::dsrl32: // DSRL32
        cpu_.set_gpr(i_.rd(), rt() >> (i_.sa() + 32));
        return sequential();
    case DecodedOpcode::dsra32: // DSRA32
        cpu_.set_gpr(i_.rd(), static_cast<std::uint64_t>(as_signed(rt()) >> (i_.sa() + 32)));
        return sequential();
    case DecodedOpcode::bltz:
        return branch(rs_signed() < 0); // BLTZ
    case DecodedOpcode::bgez:
        return branch(rs_signed() >= 0); // BGEZ
    case DecodedOpcode::bltzl:
        return branch(rs_signed() < 0, true); // BLTZL
    case DecodedOpcode::bgezl:
        return branch(rs_signed() >= 0, true); // BGEZL
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
    case DecodedOpcode::bgezall: { // BLTZAL, BGEZAL, BLTZALL, BGEZALL: the link is written whether
                                   // or not taken.
        const bool condition = (i_.rt() & 1) != 0 ? rs_signed() >= 0 : rs_signed() < 0;
        cpu_.set_gpr(31, pc_ + 8);
        return branch(condition, i_.rt() >= 18);
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
    case DecodedOpcode::j:     // J
    case DecodedOpcode::jal: { // JAL
        const std::uint64_t target = ((pc_ + 4) & ~std::uint64_t{0x0fff'ffff}) | (i_.target() << 2);
        if (op_ == DecodedOpcode::jal) {
            cpu_.set_gpr(31, pc_ + 8);
        }
        return Control{Flow::branch, true, target};
    }
    case DecodedOpcode::beq:
        return branch(rs() == rt()); // BEQ
    case DecodedOpcode::bne:
        return branch(rs() != rt()); // BNE
    case DecodedOpcode::blez:
        return branch(as_signed(rs()) <= 0); // BLEZ
    case DecodedOpcode::bgtz:
        return branch(as_signed(rs()) > 0); // BGTZ
    case DecodedOpcode::addi: {             // ADDI
        const std::int64_t sum = std::int64_t{static_cast<std::int32_t>(rs())} +
                                 static_cast<std::int16_t>(i_.immediate());
        if (sum != static_cast<std::int32_t>(sum)) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(i_.rt(), static_cast<std::uint64_t>(sum));
        return sequential();
    }
    case DecodedOpcode::addiu: // ADDIU
        cpu_.set_gpr(i_.rt(), sign_extend_32(rs() + i_.signed_immediate()));
        return sequential();
    case DecodedOpcode::slti: // SLTI
        cpu_.set_gpr(i_.rt(), as_signed(rs()) < as_signed(i_.signed_immediate()) ? 1 : 0);
        return sequential();
    case DecodedOpcode::sltiu: // SLTIU
        cpu_.set_gpr(i_.rt(), rs() < i_.signed_immediate() ? 1 : 0);
        return sequential();
    case DecodedOpcode::andi: // ANDI
        cpu_.set_gpr(i_.rt(), rs() & i_.immediate());
        return sequential();
    case DecodedOpcode::ori: // ORI
        cpu_.set_gpr(i_.rt(), rs() | i_.immediate());
        return sequential();
    case DecodedOpcode::xori: // XORI
        cpu_.set_gpr(i_.rt(), rs() ^ i_.immediate());
        return sequential();
    case DecodedOpcode::lui: // LUI
        cpu_.set_gpr(i_.rt(), sign_extend_32(i_.immediate() << 16));
        return sequential();
    case DecodedOpcode::cop1: // COP1, LWC1, LDC1, SWC1, SDC1
        if (!cpu_.coprocessor_usable(1)) {
            return unusable(1);
        }
        return execute_fpu(cpu_, i_);
    case DecodedOpcode::cop1x: // COP1X (MIPS IV)
        if (!cpu_.allows_mips4()) {
            return reserved();
        }
        if (!cpu_.coprocessor_usable(1)) {
            return unusable(1);
        }
        return execute_fpu(cpu_, i_);
    case DecodedOpcode::cop2: // COP2, LWC2, LDC2, SWC2, SDC2
        // The R10000 has no coprocessor 2 (UM 17.7).
        if (!cpu_.coprocessor_usable(2)) {
            return unusable(2);
        }
        return reserved();
    case DecodedOpcode::beql:
        return branch(rs() == rt(), true); // BEQL
    case DecodedOpcode::bnel:
        return branch(rs() != rt(), true); // BNEL
    case DecodedOpcode::blezl:
        return branch(as_signed(rs()) <= 0, true); // BLEZL
    case DecodedOpcode::bgtzl:
        return branch(as_signed(rs()) > 0, true); // BGTZL
    case DecodedOpcode::daddi: {                  // DADDI
        const std::uint64_t immediate = i_.signed_immediate();
        const std::uint64_t sum = rs() + immediate;
        if ((((rs() ^ sum) & (immediate ^ sum)) >> 63) != 0) {
            return raise(ExceptionCode::overflow);
        }
        cpu_.set_gpr(i_.rt(), sum);
        return sequential();
    }
    case DecodedOpcode::daddiu: // DADDIU
        cpu_.set_gpr(i_.rt(), rs() + i_.signed_immediate());
        return sequential();
    case DecodedOpcode::ldl:
        return load_left_right(true, AccessWidth::bits64); // LDL
    case DecodedOpcode::ldr:
        return load_left_right(false, AccessWidth::bits64); // LDR
    case DecodedOpcode::lb:
        return load<AccessWidth::bits8, true>(); // LB
    case DecodedOpcode::lh:
        return load<AccessWidth::bits16, true>(); // LH
    case DecodedOpcode::lwl:
        return load_left_right(true, AccessWidth::bits32); // LWL
    case DecodedOpcode::lw:
        return load<AccessWidth::bits32, true>(); // LW
    case DecodedOpcode::lbu:
        return load<AccessWidth::bits8, false>(); // LBU
    case DecodedOpcode::lhu:
        return load<AccessWidth::bits16, false>(); // LHU
    case DecodedOpcode::lwr:
        return load_left_right(false, AccessWidth::bits32); // LWR
    case DecodedOpcode::lwu:
        return load<AccessWidth::bits32, false>(); // LWU
    case DecodedOpcode::sb:
        return store<AccessWidth::bits8>(); // SB
    case DecodedOpcode::sh:
        return store<AccessWidth::bits16>(); // SH
    case DecodedOpcode::swl:
        return store_left_right(true, AccessWidth::bits32); // SWL
    case DecodedOpcode::sw:
        return store<AccessWidth::bits32>(); // SW
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
            return std::unexpected(result.error());
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
    case DecodedOpcode::ld:
        return load<AccessWidth::bits64, false>(); // LD
    case DecodedOpcode::sc:
        return store_conditional(AccessWidth::bits32); // SC
    case DecodedOpcode::scd:
        return store_conditional(AccessWidth::bits64); // SCD
    case DecodedOpcode::sd:
        return store<AccessWidth::bits64>(); // SD
    case DecodedOpcode::reserved:
        return reserved();
    }
    return reserved();
}

} // namespace

ExecutionResult execute(Cpu& cpu, const DecodedInstruction& instruction, std::uint64_t pc) {
    return Executor{cpu, instruction, pc}.execute();
}

} // namespace ultraviolent::mips
