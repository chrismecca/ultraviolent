#include <ultraviolent/arch/mips/fpu.hpp>

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>

// MIPS IV floating point (ISA appendix B) with R10000 specifics (UM chapter 15).
//
// Arithmetic runs on the host's IEEE 754 binary32/binary64 hardware under the FCSR rounding
// mode, with operands and results passed through volatile objects so that no computation is
// moved across the rounding-mode change or the exception-flag read. MIPS-specific encodings
// (signaling-NaN sense, default NaNs) and trap rules are applied around the host operation.
// Choices where the sources are silent are marked "hypothesis" and listed in doc/MIPS.adoc.
namespace ultraviolent::mips {

namespace {

constexpr unsigned format_single = 16;
constexpr unsigned format_double = 17;
constexpr unsigned format_word = 20;
constexpr unsigned format_long = 21;

template <class T> struct Traits;

template <> struct Traits<float> {
    using Bits = std::uint32_t;
    static constexpr Bits sign = 0x8000'0000u;
    static constexpr Bits exponent = 0x7f80'0000u;
    static constexpr Bits fraction = 0x007f'ffffu;
    // MIPS legacy NaN sense: a set fraction MSB marks a signaling NaN (ISA Table B-2).
    static constexpr Bits signaling = 0x0040'0000u;
    // ISA Table B-3.
    static constexpr Bits default_nan = 0x7fbf'ffffu;
};

template <> struct Traits<double> {
    using Bits = std::uint64_t;
    static constexpr Bits sign = 0x8000'0000'0000'0000u;
    static constexpr Bits exponent = 0x7ff0'0000'0000'0000u;
    static constexpr Bits fraction = 0x000f'ffff'ffff'ffffu;
    static constexpr Bits signaling = 0x0008'0000'0000'0000u;
    static constexpr Bits default_nan = 0x7ff7'ffff'ffff'ffffu;
};

template <class T> constexpr bool is_nan(typename Traits<T>::Bits bits) {
    return (bits & Traits<T>::exponent) == Traits<T>::exponent && (bits & Traits<T>::fraction) != 0;
}

template <class T> constexpr bool is_signaling_nan(typename Traits<T>::Bits bits) {
    return is_nan<T>(bits) && (bits & Traits<T>::signaling) != 0;
}

template <class T> constexpr bool is_infinity(typename Traits<T>::Bits bits) {
    return (bits & ~Traits<T>::sign) == Traits<T>::exponent;
}

template <class T> constexpr bool is_denormal(typename Traits<T>::Bits bits) {
    return (bits & Traits<T>::exponent) == 0 && (bits & Traits<T>::fraction) != 0;
}

// Result bits and the FCSR Cause bits (including E) of one operation.
struct Outcome {
    std::uint64_t bits;
    std::uint32_t cause;
};

// Sets the host rounding mode for one operation and collects the IEEE exceptions it raises.
class HostOperation {
  public:
    explicit HostOperation(unsigned rounding) : saved_{std::fegetround()} {
        static constexpr int modes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
        std::fesetround(modes[rounding & 3]);
        std::feclearexcept(FE_ALL_EXCEPT);
    }
    HostOperation(const HostOperation&) = delete;
    HostOperation& operator=(const HostOperation&) = delete;
    ~HostOperation() {
        std::fesetround(saved_);
    }

    [[nodiscard]] std::uint32_t raised() const {
        const int raised = std::fetestexcept(FE_ALL_EXCEPT);
        std::uint32_t cause = 0;
        cause |= (raised & FE_INEXACT) != 0 ? fcsr::inexact : 0;
        cause |= (raised & FE_UNDERFLOW) != 0 ? fcsr::underflow : 0;
        cause |= (raised & FE_OVERFLOW) != 0 ? fcsr::overflow : 0;
        cause |= (raised & FE_DIVBYZERO) != 0 ? fcsr::divide_by_zero : 0;
        cause |= (raised & FE_INVALID) != 0 ? fcsr::invalid : 0;
        return cause;
    }

  private:
    int saved_;
};

template <class T, class Op> T guarded(Op op, T a, T b) {
    volatile T x = a;
    volatile T y = b;
    volatile T result = op(T{x}, T{y});
    return result;
}

// Applies MIPS result rules to a host result: a host NaN becomes the MIPS default NaN, and a
// tiny result either traps as unimplemented (FS=0) or is flushed to zero without flagging an
// exception (FS=1) (UM 15.4, "Flush (FS) bit").
template <class T> Outcome finish(typename Traits<T>::Bits bits, std::uint32_t cause, bool flush) {
    if (is_nan<T>(bits)) {
        bits = Traits<T>::default_nan;
    }
    if ((cause & fcsr::underflow) != 0 || is_denormal<T>(bits)) {
        if (!flush) {
            return {bits, cause | fcsr::unimplemented};
        }
        bits &= Traits<T>::sign;
        cause &= ~(fcsr::underflow | fcsr::inexact);
    }
    return {bits, cause};
}

// NaN operand rules for arithmetic (ISA B 5.3.1, B 2.1): a signaling NaN is an invalid
// operation producing the default NaN; otherwise a quiet NaN operand is the result.
template <class T>
bool nan_operand(Outcome& outcome, typename Traits<T>::Bits a, typename Traits<T>::Bits b) {
    if (is_signaling_nan<T>(a) || is_signaling_nan<T>(b)) {
        outcome = {Traits<T>::default_nan, fcsr::invalid};
        return true;
    }
    if (is_nan<T>(a)) {
        outcome = {a, 0};
        return true;
    }
    if (is_nan<T>(b)) {
        outcome = {b, 0};
        return true;
    }
    return false;
}

template <class T, class Op>
Outcome binary(std::uint64_t a_bits, std::uint64_t b_bits, unsigned rounding, bool flush, Op op) {
    using Bits = typename Traits<T>::Bits;
    const auto a = static_cast<Bits>(a_bits);
    const auto b = static_cast<Bits>(b_bits);
    Outcome outcome{};
    if (nan_operand<T>(outcome, a, b)) {
        return outcome;
    }
    HostOperation host{rounding};
    const T result = guarded(op, std::bit_cast<T>(a), std::bit_cast<T>(b));
    return finish<T>(std::bit_cast<Bits>(result), host.raised(), flush);
}

template <class T, class Op>
Outcome unary(std::uint64_t a_bits, unsigned rounding, bool flush, Op op) {
    return binary<T>(a_bits, 0, rounding, flush, [op](T a, T) { return op(a); });
}

// ABS and NEG change only the sign, but are arithmetic: NaN rules and the denormal-result
// rule apply (ISA ABS.fmt, NEG.fmt).
template <class T> Outcome sign_operation(std::uint64_t a_bits, bool negate, bool flush) {
    using Bits = typename Traits<T>::Bits;
    const auto a = static_cast<Bits>(a_bits);
    Outcome outcome{};
    if (nan_operand<T>(outcome, a, 0)) {
        return outcome;
    }
    const Bits result = negate ? a ^ Traits<T>::sign : a & ~Traits<T>::sign;
    return finish<T>(result, 0, flush);
}

// Conversion to a fixed-point format with the given rounding. Word results that are NaN,
// infinite, or out of range are invalid operations with result 2^31-1 (ISA CVT.W.fmt,
// Table B-3). The R10000 converts to longword only within 51 bits and otherwise takes an
// Unimplemented Operation exception (UM 15.5); hypothesis: NaN and infinity also do.
template <class T> Outcome to_fixed(std::uint64_t a_bits, unsigned rounding, bool longword) {
    using Bits = typename Traits<T>::Bits;
    const auto a = static_cast<Bits>(a_bits);
    if (is_nan<T>(a) || is_infinity<T>(a)) {
        return longword ? Outcome{0, fcsr::unimplemented} : Outcome{0x7fff'ffff, fcsr::invalid};
    }
    HostOperation host{rounding};
    const T value = std::bit_cast<T>(a);
    const T rounded = guarded([](T x, T) { return std::nearbyint(x); }, value, T{});
    const std::uint32_t inexact = rounded != value ? fcsr::inexact : 0;
    if (longword) {
        constexpr T limit = static_cast<T>(std::int64_t{1} << 51);
        if (!(rounded > -limit && rounded < limit)) {
            return {0, fcsr::unimplemented};
        }
        return {static_cast<std::uint64_t>(static_cast<std::int64_t>(rounded)), inexact};
    }
    if (!(rounded >= static_cast<T>(-2147483648.0) && rounded <= static_cast<T>(2147483647.0))) {
        return {0x7fff'ffff, fcsr::invalid};
    }
    return {static_cast<std::uint32_t>(static_cast<std::int32_t>(rounded)), inexact};
}

// Conversion from a fixed-point format. hypothesis: longwords needing more than 51 bits take
// an Unimplemented Operation exception, mirroring the CVT.L limit (UM 15.5).
template <class T>
Outcome from_fixed(std::uint64_t source, bool longword, unsigned rounding, bool flush) {
    using Bits = typename Traits<T>::Bits;
    const std::int64_t value =
        longword ? static_cast<std::int64_t>(source)
                 : std::int64_t{static_cast<std::int32_t>(static_cast<std::uint32_t>(source))};
    if (longword && (value >= (std::int64_t{1} << 51) || value <= -(std::int64_t{1} << 51))) {
        return {0, fcsr::unimplemented};
    }
    HostOperation host{rounding};
    volatile std::int64_t input = value;
    volatile T result = static_cast<T>(std::int64_t{input});
    return finish<T>(std::bit_cast<Bits>(T{result}), host.raised(), flush);
}

// CVT.S.D and CVT.D.S. hypothesis: a quiet NaN converts to the destination default NaN.
template <class To, class From>
Outcome convert_float(std::uint64_t a_bits, unsigned rounding, bool flush) {
    using FromBits = typename Traits<From>::Bits;
    using ToBits = typename Traits<To>::Bits;
    const auto a = static_cast<FromBits>(a_bits);
    if (is_nan<From>(a)) {
        return {Traits<To>::default_nan, is_signaling_nan<From>(a) ? fcsr::invalid : 0};
    }
    HostOperation host{rounding};
    volatile From input = std::bit_cast<From>(a);
    volatile To result = static_cast<To>(From{input});
    return finish<To>(std::bit_cast<ToBits>(To{result}), host.raised(), flush);
}

// C.cond.fmt (ISA C.cond.fmt, Table B-21): bit 3 of cond makes unordered operands invalid,
// bits 2:0 select less, equal, and unordered.
template <class T> Outcome compare(std::uint64_t a_bits, std::uint64_t b_bits, unsigned condition) {
    using Bits = typename Traits<T>::Bits;
    const auto a = static_cast<Bits>(a_bits);
    const auto b = static_cast<Bits>(b_bits);
    const bool unordered = is_nan<T>(a) || is_nan<T>(b);
    const bool invalid =
        is_signaling_nan<T>(a) || is_signaling_nan<T>(b) || (unordered && (condition & 8) != 0);
    bool result = false;
    if (unordered) {
        result = (condition & 1) != 0;
    } else {
        const T x = std::bit_cast<T>(a);
        const T y = std::bit_cast<T>(b);
        result = ((condition & 4) != 0 && x < y) || ((condition & 2) != 0 && x == y);
    }
    return {result ? 1u : 0u, invalid ? fcsr::invalid : 0};
}

std::unexpected<Exception> raise(ExceptionCode code) {
    return std::unexpected(Exception{.code = code});
}

class FpuExecutor {
  public:
    FpuExecutor(Cpu& cpu, Instruction instruction)
        : cpu_{cpu}, f_{cpu.fpu()}, i_{instruction}, wide_{cpu.fpu_64bit_registers()} {}

    ExecutionResult execute();

  private:
    [[nodiscard]] unsigned rounding() const {
        return f_.fcsr & fcsr::rounding_mask;
    }
    [[nodiscard]] bool flush() const {
        return (f_.fcsr & fcsr::flush) != 0;
    }
    [[nodiscard]] std::uint64_t gpr(unsigned n) const {
        return cpu_.state().gpr[n];
    }

    // Register access by format (UM 15.3). With FR=0, arithmetic register numbers have
    // their low bit forced to zero, as the R10000 rename unit does.
    [[nodiscard]] unsigned arithmetic_register(unsigned n) const {
        return wide_ ? n : n & ~1u;
    }
    [[nodiscard]] std::uint64_t read_operand(unsigned n, bool doubleword) const {
        const std::uint64_t value = f_.fpr[arithmetic_register(n)];
        return doubleword ? value : value & 0xffff'ffff;
    }
    void write_result(unsigned n, bool doubleword, std::uint64_t value) {
        std::uint64_t& reg = f_.fpr[arithmetic_register(n)];
        if (doubleword) {
            reg = value;
        } else if (wide_) {
            // The R10000 clears the upper half of single and word results (UM 15.5).
            reg = value & 0xffff'ffff;
        } else {
            // hypothesis: with FR=0 the upper half is the odd logical register and is kept.
            reg = (reg & 0xffff'ffff'0000'0000) | (value & 0xffff'ffff);
        }
    }
    // Word moves, loads, and stores reach odd logical registers when FR=0.
    [[nodiscard]] std::uint32_t read_word(unsigned n) const {
        if (!wide_ && (n & 1) != 0) {
            return static_cast<std::uint32_t>(f_.fpr[n - 1] >> 32);
        }
        return static_cast<std::uint32_t>(f_.fpr[n]);
    }
    void write_word(unsigned n, std::uint32_t value) {
        if (wide_) {
            // Upper half cleared by the R10000 for MTC1 and LWC1 (UM 15.5).
            f_.fpr[n] = value;
        } else if ((n & 1) != 0) {
            f_.fpr[n - 1] = (f_.fpr[n - 1] & 0xffff'ffff) | (std::uint64_t{value} << 32);
        } else {
            f_.fpr[n] = (f_.fpr[n] & 0xffff'ffff'0000'0000) | value;
        }
    }
    [[nodiscard]] std::uint64_t read_doubleword(unsigned n) const {
        return f_.fpr[arithmetic_register(n)];
    }
    void write_doubleword(unsigned n, std::uint64_t value) {
        f_.fpr[arithmetic_register(n)] = value;
    }

    // Records the Cause field and either traps or updates Flags and delivers the result
    // (UM 15.4 "Bit Descriptions of the FSR").
    template <class Deliver> ExecutionResult commit(const Outcome& outcome, Deliver deliver) {
        const std::uint32_t enables = (f_.fcsr >> fcsr::enable_shift) & 0x1f;
        f_.fcsr = (f_.fcsr & ~(0x3fu << fcsr::cause_shift)) | (outcome.cause << fcsr::cause_shift);
        if ((outcome.cause & fcsr::unimplemented) != 0 || (outcome.cause & enables) != 0) {
            return raise(ExceptionCode::floating_point);
        }
        f_.fcsr |= (outcome.cause & 0x1f) << fcsr::flag_shift;
        deliver(outcome.bits);
        return Control{};
    }

    ExecutionResult unimplemented() {
        return commit(Outcome{0, fcsr::unimplemented}, [](std::uint64_t) {});
    }

    ExecutionResult deliver(const Outcome& outcome, unsigned destination, bool doubleword) {
        return commit(outcome,
                      [&](std::uint64_t bits) { write_result(destination, doubleword, bits); });
    }

    ExecutionResult cop1();
    ExecutionResult branch();
    ExecutionResult arithmetic(unsigned format);
    ExecutionResult conditional_move(bool doubleword, bool move);
    ExecutionResult cop1x();
    ExecutionResult load_store();

    Cpu& cpu_;
    FpuState& f_;
    Instruction i_;
    bool wide_;
};

ExecutionResult FpuExecutor::execute() {
    switch (i_.opcode()) {
    case 17:
        return cop1();
    case 19:
        return cop1x();
    default:
        return load_store();
    }
}

ExecutionResult FpuExecutor::load_store() {
    const std::uint64_t address = gpr(i_.rs()) + i_.signed_immediate();
    const unsigned ft = i_.rt();
    switch (i_.opcode()) {
    case 49: { // LWC1
        auto value = cpu_.load(address, AccessWidth::bits32);
        if (!value) {
            return std::unexpected(value.error());
        }
        write_word(ft, static_cast<std::uint32_t>(*value));
        return Control{};
    }
    case 53: { // LDC1
        auto value = cpu_.load(address, AccessWidth::bits64);
        if (!value) {
            return std::unexpected(value.error());
        }
        write_doubleword(ft, *value);
        return Control{};
    }
    case 57: // SWC1
        if (auto result = cpu_.store(address, AccessWidth::bits32, read_word(ft)); !result) {
            return std::unexpected(result.error());
        }
        return Control{};
    default: // SDC1
        if (auto result = cpu_.store(address, AccessWidth::bits64, read_doubleword(ft)); !result) {
            return std::unexpected(result.error());
        }
        return Control{};
    }
}

ExecutionResult FpuExecutor::branch() {
    const unsigned cc = (i_.rt() >> 2) & 7;
    if (cc != 0 && !cpu_.allows_mips4()) {
        return raise(ExceptionCode::reserved_instruction);
    }
    const bool likely = (i_.rt() & 2) != 0;
    const bool on_true = (i_.rt() & 1) != 0;
    const std::uint64_t target = cpu_.state().pc + 4 + (i_.signed_immediate() << 2);
    return Control{likely ? Flow::branch_likely : Flow::branch, fpu_condition(cpu_, cc) == on_true,
                   target};
}

ExecutionResult FpuExecutor::cop1() {
    const unsigned rt = i_.rt();
    const unsigned fs = i_.rd();
    switch (i_.rs()) {
    case 0: // MFC1
        cpu_.set_gpr(
            rt, static_cast<std::uint64_t>(std::int64_t{static_cast<std::int32_t>(read_word(fs))}));
        return Control{};
    case 1: // DMFC1
        if (!cpu_.allows_64bit_operations()) {
            return raise(ExceptionCode::reserved_instruction); // UM 17.6
        }
        cpu_.set_gpr(rt, read_doubleword(fs));
        return Control{};
    case 2: { // CFC1: only control registers 0 and 31 exist (UM 15.5).
        std::uint32_t value = 0;
        if (fs == 0) {
            value = cpu_.fpu_id();
        } else if (fs == 31) {
            value = f_.fcsr;
        }
        cpu_.set_gpr(rt,
                     static_cast<std::uint64_t>(std::int64_t{static_cast<std::int32_t>(value)}));
        return Control{};
    }
    case 4: // MTC1
        write_word(fs, static_cast<std::uint32_t>(gpr(rt)));
        return Control{};
    case 5: // DMTC1
        if (!cpu_.allows_64bit_operations()) {
            return raise(ExceptionCode::reserved_instruction);
        }
        write_doubleword(fs, gpr(rt));
        return Control{};
    case 6: // CTC1
        if (fs == 31) {
            f_.fcsr = static_cast<std::uint32_t>(gpr(rt)) & fcsr::writable;
            // A Cause bit with its Enable bit set (or E) traps on the CTC1 itself, after the
            // write (UM 15.4 "Loading the FSR").
            const std::uint32_t cause = (f_.fcsr >> fcsr::cause_shift) & 0x3f;
            const std::uint32_t enables =
                ((f_.fcsr >> fcsr::enable_shift) & 0x1f) | fcsr::unimplemented;
            if ((cause & enables) != 0) {
                return raise(ExceptionCode::floating_point);
            }
        }
        return Control{};
    case 8:
        return branch();
    case format_single:
    case format_double:
    case format_word:
    case format_long:
        return arithmetic(i_.rs());
    default:
        // Undefined opcodes are reserved; undefined formats are unimplemented (UM 17.6).
        if (i_.rs() < 16) {
            return raise(ExceptionCode::reserved_instruction);
        }
        return unimplemented();
    }
}

ExecutionResult FpuExecutor::conditional_move(bool doubleword, bool move) {
    const unsigned fd = i_.sa();
    const unsigned fs = i_.rd();
    if (move) {
        write_result(fd, doubleword, read_operand(fs, doubleword));
    } else if (!doubleword && wide_) {
        // The R10000 clears the upper half even when no move occurs (UM 15.5).
        f_.fpr[fd] &= 0xffff'ffff;
    }
    return Control{};
}

ExecutionResult FpuExecutor::arithmetic(unsigned format) {
    const unsigned funct = i_.funct();
    const unsigned ft = i_.rt();
    const unsigned fs = i_.rd();
    const unsigned fd = i_.sa();
    const bool is_double = format == format_double;
    const bool floating = format == format_single || format == format_double;
    const bool mips3 = cpu_.allows_64bit_operations();

    // The L format is unimplemented unless MIPS III is enabled (UM 17.6).
    if (format == format_long && !mips3) {
        return unimplemented();
    }

    if (funct >= 48) { // C.cond.fmt
        if (!floating) {
            return unimplemented();
        }
        const unsigned cc = fd >> 2;
        if (cc != 0 && !cpu_.allows_mips4()) {
            return unimplemented(); // UM Table 17-4
        }
        const Outcome outcome =
            is_double
                ? compare<double>(read_operand(fs, true), read_operand(ft, true), funct & 15)
                : compare<float>(read_operand(fs, false), read_operand(ft, false), funct & 15);
        return commit(outcome, [&](std::uint64_t result) {
            const std::uint32_t bit = cc == 0 ? fcsr::condition0 : 1u << (24 + cc);
            f_.fcsr = result != 0 ? f_.fcsr | bit : f_.fcsr & ~bit;
        });
    }

    switch (funct) {
    case 32: // CVT.S
    case 33: // CVT.D
    {
        const bool to_double = funct == 33;
        if (format == (to_double ? format_double : format_single)) {
            return unimplemented();
        }
        Outcome outcome{};
        if (format == format_word || format == format_long) {
            const std::uint64_t source = read_operand(fs, format == format_long);
            outcome = to_double
                          ? from_fixed<double>(source, format == format_long, rounding(), flush())
                          : from_fixed<float>(source, format == format_long, rounding(), flush());
        } else if (to_double) {
            outcome = convert_float<double, float>(read_operand(fs, false), rounding(), flush());
        } else {
            outcome = convert_float<float, double>(read_operand(fs, true), rounding(), flush());
        }
        return deliver(outcome, fd, to_double);
    }
    case 36: // CVT.W
    case 37: // CVT.L
    case 8:  // ROUND.L
    case 9:  // TRUNC.L
    case 10: // CEIL.L
    case 11: // FLOOR.L
    case 12: // ROUND.W
    case 13: // TRUNC.W
    case 14: // CEIL.W
    case 15: // FLOOR.W
    {
        if (!floating) {
            return unimplemented();
        }
        const bool longword = funct == 37 || (funct >= 8 && funct <= 11);
        if (longword && !mips3) {
            return unimplemented();
        }
        // ROUND, TRUNC, CEIL, and FLOOR fix the rounding mode (RN, RZ, RP, RM).
        const unsigned mode = funct >= 36 ? rounding() : funct & 3;
        const Outcome outcome = is_double
                                    ? to_fixed<double>(read_operand(fs, true), mode, longword)
                                    : to_fixed<float>(read_operand(fs, false), mode, longword);
        return deliver(outcome, fd, longword);
    }
    default:
        break;
    }

    // The remaining functions operate on S and D only.
    if (!floating) {
        return unimplemented();
    }
    const std::uint64_t a = read_operand(fs, is_double);
    const std::uint64_t b = read_operand(ft, is_double);
    const unsigned rm = rounding();
    const bool fz = flush();

    const auto binary_op = [&](auto op) {
        return is_double ? binary<double>(a, b, rm, fz, op) : binary<float>(a, b, rm, fz, op);
    };

    switch (funct) {
    case 0:
        return deliver(binary_op([](auto x, auto y) { return x + y; }), fd, is_double);
    case 1:
        return deliver(binary_op([](auto x, auto y) { return x - y; }), fd, is_double);
    case 2:
        return deliver(binary_op([](auto x, auto y) { return x * y; }), fd, is_double);
    case 3:
        return deliver(binary_op([](auto x, auto y) { return x / y; }), fd, is_double);
    case 4: // SQRT
        return deliver(is_double ? unary<double>(a, rm, fz, [](double x) { return std::sqrt(x); })
                                 : unary<float>(a, rm, fz, [](float x) { return std::sqrt(x); }),
                       fd, is_double);
    case 5: // ABS
    case 7: // NEG
        return deliver(is_double ? sign_operation<double>(a, funct == 7, fz)
                                 : sign_operation<float>(a, funct == 7, fz),
                       fd, is_double);
    case 6: // MOV: not arithmetic; Cause is unchanged.
        write_result(fd, is_double, a);
        return Control{};
    case 17: // MOVF.fmt, MOVT.fmt
    case 18: // MOVZ.fmt
    case 19: // MOVN.fmt
    {
        if (!cpu_.allows_mips4()) {
            return unimplemented(); // UM Table 17-4
        }
        bool move = false;
        if (funct == 17) {
            move = fpu_condition(cpu_, ft >> 2) == ((ft & 1) != 0);
        } else {
            move = (gpr(ft) == 0) == (funct == 18);
        }
        return conditional_move(is_double, move);
    }
    case 21: // RECIP
    case 22: // RSQRT
    {
        if (!cpu_.allows_mips4()) {
            return unimplemented();
        }
        // hypothesis: the R10000 delivers the correctly rounded 1/x, and 1/sqrt(x) rounded
        // after each step.
        if (funct == 21) {
            return deliver(is_double ? unary<double>(a, rm, fz, [](double x) { return 1.0 / x; })
                                     : unary<float>(a, rm, fz, [](float x) { return 1.0f / x; }),
                           fd, is_double);
        }
        return deliver(is_double
                           ? unary<double>(a, rm, fz, [](double x) { return 1.0 / std::sqrt(x); })
                           : unary<float>(a, rm, fz, [](float x) { return 1.0f / std::sqrt(x); }),
                       fd, is_double);
    }
    default:
        return unimplemented();
    }
}

ExecutionResult FpuExecutor::cop1x() {
    const std::uint64_t address = gpr(i_.rs()) + gpr(i_.rt());
    const unsigned fs = i_.rd();
    const unsigned fd = i_.sa();
    switch (i_.funct()) {
    case 0: { // LWXC1
        auto value = cpu_.load(address, AccessWidth::bits32);
        if (!value) {
            return std::unexpected(value.error());
        }
        write_word(fd, static_cast<std::uint32_t>(*value));
        return Control{};
    }
    case 1: { // LDXC1
        auto value = cpu_.load(address, AccessWidth::bits64);
        if (!value) {
            return std::unexpected(value.error());
        }
        write_doubleword(fd, *value);
        return Control{};
    }
    case 8: // SWXC1
        if (auto result = cpu_.store(address, AccessWidth::bits32, read_word(fs)); !result) {
            return std::unexpected(result.error());
        }
        return Control{};
    case 9: // SDXC1
        if (auto result = cpu_.store(address, AccessWidth::bits64, read_doubleword(fs)); !result) {
            return std::unexpected(result.error());
        }
        return Control{};
    case 15: // PREFX: a hint with no architectural effect.
        return Control{};
    default:
        break;
    }

    const unsigned operation = i_.funct() >> 3;
    const unsigned format = i_.funct() & 7;
    if (operation < 4 || operation > 7 || format > 1) {
        return raise(ExceptionCode::reserved_instruction);
    }
    // MADD, MSUB, NMADD, NMSUB. The R10000 makes one pass through the multiplier and then one
    // through the adder (UM 2, floating-point queue), so the product is rounded before the add.
    const bool is_double = format == 1;
    const std::uint64_t fr = read_operand(i_.rs(), is_double);
    const std::uint64_t a = read_operand(fs, is_double);
    const std::uint64_t b = read_operand(i_.rt(), is_double);
    const unsigned rm = rounding();
    const bool fz = flush();
    const bool subtract = operation == 5 || operation == 7;
    const bool negate = operation >= 6;

    const auto multiply_add = [&]<class T>() {
        using Bits = typename Traits<T>::Bits;
        Outcome product = binary<T>(a, b, rm, fz, [](T x, T y) { return x * y; });
        if ((product.cause & fcsr::unimplemented) != 0) {
            return product;
        }
        Outcome sum = subtract
                          ? binary<T>(product.bits, fr, rm, fz, [](T x, T y) { return x - y; })
                          : binary<T>(product.bits, fr, rm, fz, [](T x, T y) { return x + y; });
        sum.cause |= product.cause;
        if (negate && !is_nan<T>(static_cast<Bits>(sum.bits))) {
            sum.bits ^= Traits<T>::sign;
        }
        return sum;
    };
    const Outcome outcome = is_double ? multiply_add.template operator()<double>()
                                      : multiply_add.template operator()<float>();
    return deliver(outcome, fd, is_double);
}

} // namespace

ExecutionResult execute_fpu(Cpu& cpu, Instruction instruction) {
    return FpuExecutor{cpu, instruction}.execute();
}

bool fpu_condition(const Cpu& cpu, unsigned cc) {
    const std::uint32_t bit = cc == 0 ? fcsr::condition0 : 1u << (24 + cc);
    return (cpu.fpu().fcsr & bit) != 0;
}

} // namespace ultraviolent::mips
