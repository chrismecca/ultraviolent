// Tier 1 on x86-64 (JIT.adoc): decoded blocks as native code.
//
// Register use in generated code: rbx holds the Cpu, r12 the run's Context, r15 the CPU's
// cycle count (Cpu's copy is written before every helper call and exit, and read back after
// a call); all three are callee-saved, so helper calls keep them. The entry code saves the callee-saved registers
// and calls the block; a block returns its exit in rax (reason in bits 7:0, operations run in
// bits 39:8). Helpers are ordinary functions reached through a table at the start of the code
// region (call [rip + slot]), so no call depends on where the region is mapped.
//
// An operation in builder form (operations.hpp) is emitted by EmitBuilder from the same
// source the interpreters execute; any other operation, and any operation the translation
// context makes reserved, runs through call_semantics.

#include <ultraviolent/arch/mips/block_interpreter.hpp>
#include <ultraviolent/arch/mips/decoded.hpp>
#include <ultraviolent/arch/mips/native_code.hpp>
#include <ultraviolent/core/invariant.hpp>
#include <ultraviolent/jit/executable_memory.hpp>
#include <ultraviolent/jit/x86_64/assembler.hpp>

#include "../operations.hpp"
#include "../semantics.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <cpuid.h>
#include <unistd.h>

namespace ultraviolent::mips {

namespace {

using jit::ExecutableMemory;
using namespace jit::x86_64;

static_assert(sizeof(bool) == 1, "generated code reads and writes bool fields as bytes");
static_assert(sizeof(DecodedInstruction) == 8, "an operation is passed as one immediate");

// What a run gives its generated code (r12).
struct Context {
    Cpu* cpu;
    const std::uint64_t* limit;
    // The run limit or the CPU's next internal event, whichever is first: a block starts only
    // when all of its operations fit before it.
    std::uint64_t deadline;
    const DispatchEntry* dispatch;
    // Cpu::code_changes() when the run started: every block entered was valid then, and stays
    // valid while it is unchanged.
    std::uint64_t code_changes;
};
constexpr std::int32_t context_cpu = offsetof(Context, cpu);
constexpr std::int32_t context_limit = offsetof(Context, limit);
constexpr std::int32_t context_deadline = offsetof(Context, deadline);
constexpr std::int32_t context_dispatch = offsetof(Context, dispatch);
constexpr std::int32_t context_code_changes = offsetof(Context, code_changes);

static_assert(sizeof(DispatchEntry) == 32, "dispatch scales the index by 32");
constexpr std::int32_t dispatch_pc = offsetof(DispatchEntry, pc);
constexpr std::int32_t dispatch_context = offsetof(DispatchEntry, context);
constexpr std::int32_t dispatch_stamp = offsetof(DispatchEntry, stamp);
constexpr std::int32_t dispatch_code = offsetof(DispatchEntry, code);

// call_semantics: one operation through the shared semantics, and its cycle (complete_cycle).
// Returns whether it retired.
// An access that reached the bus may have let a device write memory (DMA), so code changes
// are synchronized after one (Cpu::synchronize_code).
std::uint64_t operation_helper(Cpu* cpu, std::uint64_t operation) {
    cpu->clear_left_host_path();
    const bool retired =
        complete_cycle<&execute_inline>(*cpu, std::bit_cast<DecodedInstruction>(operation));
    if (cpu->left_host_path()) {
        cpu->synchronize_code();
    }
    return retired ? 1 : 0;
}

// raise_if held in native code: the exception the direct builder would have taken, then the
// cycle (complete_cycle's exception path).
void raise_helper(Cpu* cpu, std::uint64_t code) {
    cpu->take_exception(Exception{.code = static_cast<ExceptionCode>(code)});
    cpu->end_cycle(false);
}

// The cycle count reached the processor's next internal event (end_cycle's unlikely path).
void internal_event_helper(Cpu* cpu) {
    cpu->reach_internal_event();
}

enum Helper : unsigned {
    helper_operation,
    helper_raise,
    helper_internal_event,
    helper_count,
};

constexpr std::uint64_t helper_slot(std::uint64_t table, Helper helper) {
    return table + helper * sizeof(std::uint64_t);
}

constexpr bool fits_int32(std::uint64_t v) {
    return static_cast<std::int64_t>(v) == static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
}

// Guest registers and the host registers that hold them within a block (JIT.adoc "Guest
// values and the register abstraction"). Guest GPRs 1-31, HI, and LO are each in the Cpu, in a
// host register (clean or dirty), or a constant the translation knows (dirty: the Cpu has not
// been written). Dirty values are written back wherever the Cpu must be exact: before a
// helper, at side exits, and at the block's end. Host registers also hold an operation's
// temporaries; rcx stays outside the pool (shift counts, wide constants).
constexpr unsigned guest_hi = 32;
constexpr unsigned guest_lo = 33;
constexpr unsigned guest_count = 34;

class GuestRegisters {
  public:
    enum class Where : std::uint8_t { cpu, host, constant };
    struct Slot {
        Where where{Where::cpu};
        bool dirty{};
        Reg reg{};
        std::uint64_t constant{};
        std::size_t last_use{};
    };
    // Where every guest value is at one point of the translation.
    using State = std::array<Slot, guest_count>;

    GuestRegisters(Assembler& a, const Cpu::NativeLayout& layout) : a_{a}, layout_{layout} {
        for (const Reg r : pool) {
            hosts_[static_cast<unsigned>(r)].use = Use::free;
        }
    }

    [[nodiscard]] const State& state() const {
        return guests_;
    }
    [[nodiscard]] const Slot& slot(unsigned g) const {
        return guests_[g];
    }

    // An operation's values live until end_operation: host registers it read or released stay
    // untouched until then, and its temporaries are freed.
    void begin_operation(std::size_t index) {
        operation_ = index;
    }
    void end_operation() {
        for (Host& h : hosts_) {
            if (h.use == Use::temp || h.use == Use::released) {
                h.use = Use::free;
            }
            h.pinned = false;
        }
    }

    // The host register holding guest g (not a constant), loaded from the Cpu if needed.
    Reg read(unsigned g) {
        Slot& s = guests_[g];
        invariant(s.where != Where::constant, "a constant guest value is not read from a register");
        if (s.where == Where::cpu) {
            const Reg r = allocate();
            a_.mov(r, Mem{Reg::rbx, offset(g)});
            s = {.where = Where::host, .dirty = false, .reg = r};
            hosts_[static_cast<unsigned>(r)] = {.use = Use::guest, .guest = g};
        }
        s.last_use = operation_;
        hosts_[static_cast<unsigned>(s.reg)].pinned = true;
        return s.reg;
    }
    void write_constant(unsigned g, std::uint64_t value) {
        release(g);
        guests_[g] = {.where = Where::constant, .dirty = true, .constant = value,
                      .last_use = operation_};
    }
    // `temp` (from temp()) becomes guest g's register.
    void write_register(unsigned g, Reg temp) {
        invariant(is_temp(temp), "a guest register takes a temporary");
        release(g);
        guests_[g] = {.where = Where::host, .dirty = true, .reg = temp, .last_use = operation_};
        hosts_[static_cast<unsigned>(temp)] = {.use = Use::guest, .guest = g, .pinned = true};
    }
    [[nodiscard]] bool is_temp(Reg r) const {
        return hosts_[static_cast<unsigned>(r)].use == Use::temp;
    }
    // A temporary the operation no longer reads.
    void release_temp(Reg r) {
        if (is_temp(r)) {
            hosts_[static_cast<unsigned>(r)].use = Use::free;
        }
    }
    Reg temp() {
        const Reg r = allocate();
        hosts_[static_cast<unsigned>(r)].use = Use::temp;
        return r;
    }

    // Writes every dirty value back and forgets them all: the Cpu is exact and no host
    // register holds a guest value (before a helper call, which may read or write any).
    void flush() {
        write_back(a_, layout_, guests_);
        guests_ = {};
        for (const Reg r : pool) {
            hosts_[static_cast<unsigned>(r)] = {.use = Use::free};
        }
    }

    // The Cpu offset of guest g.
    [[nodiscard]] std::int32_t offset(unsigned g) const {
        return offset(layout_, g);
    }
    static std::int32_t offset(const Cpu::NativeLayout& layout, unsigned g) {
        return g == guest_hi   ? layout.hi
               : g == guest_lo ? layout.lo
                               : layout.gpr + 8 * static_cast<std::int32_t>(g);
    }
    // Emits stores of `state`'s dirty values into the Cpu (uses rcx).
    static void write_back(Assembler& a, const Cpu::NativeLayout& layout, const State& state) {
        for (unsigned g = 0; g < guest_count; ++g) {
            const Slot& s = state[g];
            if (!s.dirty) {
                continue;
            }
            const Mem m{Reg::rbx, offset(layout, g)};
            if (s.where == Where::host) {
                a.mov(m, s.reg);
            } else if (fits_int32(s.constant)) {
                a.mov(m, static_cast<std::int32_t>(s.constant));
            } else {
                a.mov(Reg::rcx, s.constant);
                a.mov(m, Reg::rcx);
            }
        }
    }
    // Emits loads of `state`'s host registers from the Cpu.
    static void reload(Assembler& a, const Cpu::NativeLayout& layout, const State& state) {
        for (unsigned g = 0; g < guest_count; ++g) {
            if (state[g].where == Where::host) {
                a.mov(state[g].reg, Mem{Reg::rbx, offset(layout, g)});
            }
        }
    }

  private:
    // Every register but rbx, r12, and r15 (the Cpu, the context, the cycle count), rsp, and
    // the scratch registers rax, rcx, and rdx (shift counts, wide constants, and the fixed
    // operands of multiply and divide). rbp, r13, and r14 are callee-saved, which the entry
    // code saved; no value lives across a helper call.
    static constexpr std::array<Reg, 9> pool = {Reg::rsi, Reg::rdi, Reg::r8,  Reg::r9, Reg::r10,
                                                Reg::r11, Reg::rbp, Reg::r13, Reg::r14};
    enum class Use : std::uint8_t {
        unavailable,
        free,
        temp,
        guest,
        // Held a guest value this operation replaced; may still be read until it ends.
        released,
    };
    struct Host {
        Use use{Use::unavailable};
        unsigned guest{};
        bool pinned{};
    };

    // Guest g no longer lives in its host register.
    void release(unsigned g) {
        Slot& s = guests_[g];
        if (s.where == Where::host) {
            hosts_[static_cast<unsigned>(s.reg)].use = Use::released;
        }
        s = {};
    }
    // A free register, or the least recently used guest register not in use by this operation
    // (written back first when dirty).
    Reg allocate() {
        for (const Reg r : pool) {
            if (hosts_[static_cast<unsigned>(r)].use == Use::free) {
                return r;
            }
        }
        const Host* victim = nullptr;
        for (const Reg r : pool) {
            const Host& h = hosts_[static_cast<unsigned>(r)];
            if (h.use == Use::guest && !h.pinned &&
                (victim == nullptr || guests_[h.guest].last_use < guests_[victim->guest].last_use)) {
                victim = &h;
            }
        }
        invariant(victim != nullptr, "an operation's values fit the register pool");
        Slot& s = guests_[victim->guest];
        const Reg r = s.reg;
        if (s.dirty) {
            a_.mov(Mem{Reg::rbx, offset(victim->guest)}, r);
        }
        s = {};
        hosts_[static_cast<unsigned>(r)] = {.use = Use::free};
        return r;
    }

    Assembler& a_;
    const Cpu::NativeLayout& layout_;
    State guests_{};
    std::array<Host, 16> hosts_{};
    std::size_t operation_{};
};

// The builder that emits (operations.hpp, JIT.adoc "Semantics"). A Value is a translation-time
// constant or a host register: a guest register's own (read only) or a temporary. Guest
// registers go through GuestRegisters.
class EmitBuilder {
  public:
    struct Value {
        bool is_constant{true};
        std::uint64_t constant{};
        Reg reg{Reg::rax};
    };
    using Condition = Value; // 0 or 1
    struct Result {
        Flow flow{Flow::sequential};
        Condition taken{};
        Value target{};
    };
    // A side exit inside the operation, with where every guest value was when it was taken: a
    // raise_if that held (the exception to take), or a load or store that missed the host page
    // cache (the slow path runs the operation through call_semantics).
    struct SideExit {
        Label label;
        GuestRegisters::State registers;
        ExceptionCode code{};
    };

    EmitBuilder(Assembler& a, GuestRegisters& registers, const Cpu::NativeLayout& layout,
                bool movbe, std::uint64_t pc, std::vector<SideExit>& raises,
                std::vector<SideExit>& slow)
        : a_{a}, registers_{registers}, layout_{layout}, movbe_{movbe}, pc_{pc}, raises_{raises},
          slow_{slow} {}

    [[nodiscard]] std::uint64_t pc() const {
        return pc_;
    }
    [[nodiscard]] static Value constant(std::uint64_t x) {
        return {.is_constant = true, .constant = x};
    }

    Value gpr(unsigned r) {
        return r == 0 ? constant(0) : guest(r);
    }
    void set_gpr(unsigned r, Value v) {
        if (r != 0) {
            set_guest(r, v);
        }
    }
    Value hi() {
        return guest(guest_hi);
    }
    Value lo() {
        return guest(guest_lo);
    }
    void set_hi(Value v) {
        set_guest(guest_hi, v);
    }
    void set_lo(Value v) {
        set_guest(guest_lo, v);
    }

    Value add(Value x, Value y) {
        return binary(x, y, semantics::DirectBuilder::add, &Assembler::add, &Assembler::add);
    }
    Value sub(Value x, Value y) {
        return binary(x, y, semantics::DirectBuilder::sub, &Assembler::sub, &Assembler::sub);
    }
    Value and_(Value x, Value y) {
        return binary(x, y, semantics::DirectBuilder::and_, &Assembler::and_, &Assembler::and_);
    }
    Value or_(Value x, Value y) {
        return binary(x, y, semantics::DirectBuilder::or_, &Assembler::or_, &Assembler::or_);
    }
    Value xor_(Value x, Value y) {
        return binary(x, y, semantics::DirectBuilder::xor_, &Assembler::xor_, &Assembler::xor_);
    }
    Value nor(Value x, Value y) {
        if (x.is_constant && y.is_constant) {
            return constant(~(x.constant | y.constant));
        }
        const Value r = or_(x, y);
        a_.not_(r.reg);
        return r;
    }

    Value shift_left(Value v, unsigned n) {
        return shift(v, n, semantics::DirectBuilder::shift_left, &Assembler::shl);
    }
    Value shift_right_logical(Value v, unsigned n) {
        return shift(v, n, semantics::DirectBuilder::shift_right_logical, &Assembler::shr);
    }
    Value shift_right_arithmetic(Value v, unsigned n) {
        return shift(v, n, semantics::DirectBuilder::shift_right_arithmetic, &Assembler::sar);
    }
    Value shift_left_by(Value v, Value n) {
        return n.is_constant ? shift_left(v, static_cast<unsigned>(n.constant))
                             : shift_by(v, n, &Assembler::shl_cl);
    }
    Value shift_right_logical_by(Value v, Value n) {
        return n.is_constant ? shift_right_logical(v, static_cast<unsigned>(n.constant))
                             : shift_by(v, n, &Assembler::shr_cl);
    }
    Value shift_right_arithmetic_by(Value v, Value n) {
        return n.is_constant ? shift_right_arithmetic(v, static_cast<unsigned>(n.constant))
                             : shift_by(v, n, &Assembler::sar_cl);
    }
    Value sign_extend_32(Value v) {
        if (v.is_constant) {
            return constant(semantics::DirectBuilder::sign_extend_32(v.constant));
        }
        const Reg r = registers_.temp();
        a_.movsxd(r, v.reg);
        return reg(r);
    }
    Value zero_extend_32(Value v) {
        if (v.is_constant) {
            return constant(v.constant & 0xffff'ffff);
        }
        const Reg r = registers_.temp();
        a_.mov32(r, v.reg);
        return reg(r);
    }

    Condition equal(Value x, Value y) {
        return compare(x, y, Cond::e, semantics::DirectBuilder::equal);
    }
    Condition not_equal(Value x, Value y) {
        return compare(x, y, Cond::ne, semantics::DirectBuilder::not_equal);
    }
    Condition less_signed(Value x, Value y) {
        return compare(x, y, Cond::l, semantics::DirectBuilder::less_signed);
    }
    Condition less_equal_signed(Value x, Value y) {
        return compare(x, y, Cond::le, semantics::DirectBuilder::less_equal_signed);
    }
    Condition greater_signed(Value x, Value y) {
        return compare(x, y, Cond::g, semantics::DirectBuilder::greater_signed);
    }
    Condition greater_equal_signed(Value x, Value y) {
        return compare(x, y, Cond::ge, semantics::DirectBuilder::greater_equal_signed);
    }
    Condition less_unsigned(Value x, Value y) {
        return compare(x, y, Cond::b, semantics::DirectBuilder::less_unsigned);
    }
    static Value from_condition(Condition c) {
        return c;
    }
    Condition add_overflows_32(Value x, Value y) {
        return overflow(x, y, semantics::DirectBuilder::add_overflows_32, &Assembler::add32,
                        &Assembler::add32);
    }
    Condition sub_overflows_32(Value x, Value y) {
        return overflow(x, y, semantics::DirectBuilder::sub_overflows_32, &Assembler::sub32,
                        &Assembler::sub32);
    }
    Condition add_overflows_64(Value x, Value y) {
        return overflow(x, y, semantics::DirectBuilder::add_overflows_64, &Assembler::add,
                        &Assembler::add);
    }
    Condition sub_overflows_64(Value x, Value y) {
        return overflow(x, y, semantics::DirectBuilder::sub_overflows_64, &Assembler::sub,
                        &Assembler::sub);
    }

    Value select(Condition c, Value x, Value y) {
        if (c.is_constant) {
            return c.constant != 0 ? x : y;
        }
        const Reg r = copy(y);
        a_.test(c.reg, c.reg);
        a_.cmov(Cond::ne, r, materialize(x));
        return reg(r);
    }
    Value multiply(Value x, Value y) {
        if (x.is_constant && y.is_constant) {
            return constant(semantics::DirectBuilder::multiply(x.constant, y.constant));
        }
        const Reg r = copy(x);
        a_.imul(r, materialize(y));
        return reg(r);
    }
    struct Wide {
        Value hi;
        Value lo;
    };
    Wide multiply_wide_signed(Value x, Value y) {
        return multiply_wide(x, y, true);
    }
    Wide multiply_wide_unsigned(Value x, Value y) {
        return multiply_wide(x, y, false);
    }
    struct Division {
        Value quotient;
        Value remainder;
    };
    Division divide_signed(Value x, Value y) {
        return divide(x, y, true);
    }
    Division divide_unsigned(Value x, Value y) {
        return divide(x, y, false);
    }

    // Branches to the operation's exception exit when c holds; translation continues either
    // way, so it never reports having raised.
    bool raise_if(Condition c, ExceptionCode code) {
        if (c.is_constant && c.constant == 0) {
            return false;
        }
        const Label label = a_.label();
        raises_.push_back({label, registers_.state(), code});
        if (c.is_constant) {
            a_.jmp(label);
        } else {
            a_.test(c.reg, c.reg);
            a_.jcc(Cond::ne, label);
        }
        return false;
    }

    // Loads and stores (JIT.adoc "Native memory"): Cpu::load_as and store_as's host page fast
    // path inline, with the same tests (the page and translation context of the address's
    // entry, natural alignment, and for a store a writable entry); anything else leaves through
    // the slow path, which runs the whole operation through call_semantics.
    template <AccessWidth W, bool SignExtend> std::optional<Value> load(Value address) {
        const Mem m = host_address(address, byte_count(W), false);
        const Reg r = registers_.temp();
        const bool swap = layout_.byte_order == ByteOrder::big;
        if constexpr (W == AccessWidth::bits8) {
            if (SignExtend) {
                a_.movsx_byte(r, m);
            } else {
                a_.movzx_byte(r, m);
            }
        } else if constexpr (W == AccessWidth::bits16) {
            a_.movzx_word(r, m);
            if (swap) {
                a_.rol16(r, 8);
            }
            if (SignExtend) {
                a_.movsx_word(r, r);
            }
        } else if constexpr (W == AccessWidth::bits32) {
            if (swap && movbe_) {
                a_.movbe(r, m, 4);
            } else {
                a_.mov32(r, m);
                if (swap) {
                    a_.bswap32(r);
                }
            }
            if (SignExtend) {
                a_.movsxd(r, r);
            }
        } else if (swap && movbe_) {
            a_.movbe(r, m, 8);
        } else {
            a_.mov(r, m);
            if (swap) {
                a_.bswap64(r);
            }
        }
        return reg(r);
    }
    template <AccessWidth W> bool store(Value address, Value value) {
        const Mem m = host_address(address, byte_count(W), true);
        const bool swap = layout_.byte_order == ByteOrder::big;
        if constexpr (W == AccessWidth::bits8) {
            if (value.is_constant) {
                a_.mov_byte(m, static_cast<std::uint8_t>(value.constant));
            } else {
                a_.mov_byte(m, value.reg);
            }
            return false;
        }
        if (swap && movbe_ && !value.is_constant) {
            a_.movbe(m, value.reg, byte_count(W));
            return false;
        }
        // The value in memory order, in a temporary.
        const Reg r = registers_.temp();
        if (value.is_constant) {
            using Word = UnsignedOf<W>;
            const auto word = static_cast<Word>(value.constant);
            a_.mov(r, std::uint64_t{swap ? std::byteswap(word) : word});
        } else {
            a_.mov(r, value.reg);
            if (swap) {
                if constexpr (W == AccessWidth::bits16) {
                    a_.rol16(r, 8);
                } else if constexpr (W == AccessWidth::bits32) {
                    a_.bswap32(r);
                } else {
                    a_.bswap64(r);
                }
            }
        }
        if constexpr (W == AccessWidth::bits16) {
            a_.mov16(m, r);
        } else if constexpr (W == AccessWidth::bits32) {
            a_.mov32(m, r);
        } else {
            a_.mov(m, r);
        }
        return false;
    }

    // The FPU's moves, loads, and stores (JIT.adoc "FPU"), as FpuExecutor performs them with
    // Status.FR = `fr` and coprocessor 1 usable: both are checked first, and any other Status
    // leaves through the slow path (the whole operation through call_semantics, which raises
    // what it must). `wide` is whether MIPS III operations are allowed (fixed by the block's
    // translation context). Empty, with nothing emitted, for every other COP1 operation.
    std::optional<Result> cop1(Instruction i, bool fr, bool wide) {
        enum class Form : std::uint8_t { ldc1, sdc1, lwc1, swc1, mfc1, dmfc1, mtc1, dmtc1, mov };
        std::optional<Form> form;
        switch (i.opcode()) {
        case 53:
            form = Form::ldc1;
            break;
        case 61:
            form = Form::sdc1;
            break;
        case 49:
            form = Form::lwc1;
            break;
        case 57:
            form = Form::swc1;
            break;
        case 17:
            if (i.rs() == 0) {
                form = Form::mfc1;
            } else if (i.rs() == 1 && wide) {
                form = Form::dmfc1;
            } else if (i.rs() == 4) {
                form = Form::mtc1;
            } else if (i.rs() == 5 && wide) {
                form = Form::dmtc1;
            } else if ((i.rs() == 16 || i.rs() == 17) && i.funct() == 6) {
                form = Form::mov;
            }
            break;
        default:
            break;
        }
        if (!form) {
            return std::nullopt;
        }
        {
            const Reg t = registers_.temp();
            a_.mov32(t, Mem{Reg::rbx, layout_.status});
            a_.and_(t, static_cast<std::int32_t>(status::cu1 | status::fr));
            a_.cmp(t, static_cast<std::int32_t>(status::cu1 | (fr ? status::fr : 0)));
            const Label slow = a_.label();
            slow_.push_back({slow, registers_.state()});
            a_.jcc(Cond::ne, slow);
        }
        // With FR=0 arithmetic register numbers are even, and word accesses to an odd register
        // reach the upper half of the even one below it (FpuExecutor).
        const auto doubleword = [&](unsigned n) {
            return Mem{Reg::rbx, layout_.fpr + 8 * static_cast<std::int32_t>(fr ? n : n & ~1u)};
        };
        const auto word = [&](unsigned n) {
            return !fr && (n & 1) != 0
                       ? Mem{Reg::rbx, layout_.fpr + 8 * static_cast<std::int32_t>(n - 1) + 4}
                       : Mem{Reg::rbx, layout_.fpr + 8 * static_cast<std::int32_t>(n)};
        };
        const auto read_word = [&](unsigned n) {
            const Reg r = registers_.temp();
            a_.mov32(r, word(n));
            return reg(r);
        };
        const auto read_doubleword = [&](unsigned n) {
            const Reg r = registers_.temp();
            a_.mov(r, doubleword(n));
            return reg(r);
        };
        const auto write_doubleword = [&](Mem m, Value v) {
            if (v.is_constant && fits_int32(v.constant)) {
                a_.mov(m, static_cast<std::int32_t>(v.constant));
            } else {
                a_.mov(m, materialize(v));
            }
        };
        // FR=1: the whole register, its upper half cleared (UM 15.5); FR=0: the half alone.
        const auto write_word = [&](unsigned n, Value v) {
            if (fr) {
                write_doubleword(word(n), zero_extend_32(v));
            } else {
                a_.mov32(word(n), materialize(v));
            }
        };
        const auto address = [&] {
            return add(gpr(i.rs()), constant(i.signed_immediate()));
        };
        switch (*form) {
        case Form::ldc1: {
            const auto value = load<AccessWidth::bits64, false>(address());
            write_doubleword(doubleword(i.rt()), *value);
            break;
        }
        case Form::sdc1: {
            const Value v = read_doubleword(i.rt());
            store<AccessWidth::bits64>(address(), v);
            break;
        }
        case Form::lwc1: {
            const auto value = load<AccessWidth::bits32, false>(address());
            write_word(i.rt(), *value);
            break;
        }
        case Form::swc1: {
            const Value v = read_word(i.rt());
            store<AccessWidth::bits32>(address(), v);
            break;
        }
        case Form::mfc1:
            set_gpr(i.rt(), sign_extend_32(read_word(i.rd())));
            break;
        case Form::dmfc1:
            set_gpr(i.rt(), read_doubleword(i.rd()));
            break;
        case Form::mtc1:
            write_word(i.rd(), gpr(i.rt()));
            break;
        case Form::dmtc1:
            write_doubleword(doubleword(i.rd()), gpr(i.rt()));
            break;
        case Form::mov: {
            // MOV.S and MOV.D (not arithmetic: Cause unchanged). A single result clears the
            // upper half with FR=1 and keeps it with FR=0 (FpuExecutor::write_result).
            const Value v = read_doubleword(i.rd());
            if (i.rs() == 17) {
                write_doubleword(doubleword(i.sa()), v);
            } else if (fr) {
                write_doubleword(doubleword(i.sa()), zero_extend_32(v));
            } else {
                a_.mov32(doubleword(i.sa()), v.reg);
            }
            break;
        }
        }
        return sequential();
    }

    [[nodiscard]] static Result raised() {
        return {.flow = Flow::exception};
    }
    [[nodiscard]] static Result sequential() {
        return {};
    }
    [[nodiscard]] static Result branch(Condition taken, std::uint64_t target, bool likely) {
        return {.flow = likely ? Flow::branch_likely : Flow::branch,
                .taken = taken,
                .target = constant(target)};
    }
    [[nodiscard]] static Result jump(Value target) {
        return {.flow = Flow::branch, .taken = constant(1), .target = target};
    }

    // For the translator, after the operation: a register holding v.
    Reg materialize(Value v) {
        if (!v.is_constant) {
            return v.reg;
        }
        const Reg r = registers_.temp();
        a_.mov(r, v.constant);
        return r;
    }

  private:
    static Value reg(Reg r) {
        return {.is_constant = false, .reg = r};
    }
    Value guest(unsigned g) {
        const GuestRegisters::Slot& s = registers_.slot(g);
        if (s.where == GuestRegisters::Where::constant) {
            return constant(s.constant);
        }
        return reg(registers_.read(g));
    }
    void set_guest(unsigned g, Value v) {
        if (v.is_constant) {
            registers_.write_constant(g, v.constant);
        } else if (registers_.is_temp(v.reg)) {
            registers_.write_register(g, v.reg);
        } else {
            // Another guest's register: a copy.
            registers_.write_register(g, copy(v));
        }
    }
    Reg copy(Value v) {
        const Reg r = registers_.temp();
        if (v.is_constant) {
            a_.mov(r, v.constant);
        } else {
            a_.mov(r, v.reg);
        }
        return r;
    }

    // The host bytes of `size` bytes at guest `address`, after the host page cache tests
    // (Cpu::data_page): the entry for the address's page (bits 17:12 select it) holds the
    // page's address tagged with its translation context (Cpu::HostPage), which the address
    // masked to its page and misaligned bits, tagged with the current context, must equal;
    // a store compares the entry's store tag. A miss branches to the slow path. The bytes are
    // at the address plus the entry's addend.
    Mem host_address(Value address, std::size_t size, bool store) {
        const Reg va = materialize(address);
        const Reg entry = registers_.temp();
        const Reg t = registers_.temp();
        const auto entry_bits = static_cast<unsigned>(std::countr_zero(layout_.host_page_size));
        a_.mov(entry, va);
        a_.shr(entry, static_cast<std::uint8_t>(12 - entry_bits));
        a_.and_(entry, static_cast<std::int32_t>((layout_.host_page_count - 1) << entry_bits));
        const std::int32_t base = layout_.data_pages;
        a_.mov(t, va);
        a_.and_(t, static_cast<std::int32_t>(~std::uint64_t{0xfff} | (size - 1)));
        a_.or_(t, Mem{Reg::rbx, layout_.context_tag});
        const Label slow = a_.label();
        slow_.push_back({slow, registers_.state()});
        a_.cmp(t, Mem{Reg::rbx,
                      base + (store ? layout_.host_page_store_page : layout_.host_page_page),
                      entry});
        a_.jcc(Cond::ne, slow);
        a_.mov(t, Mem{Reg::rbx, base + layout_.host_page_addend, entry});
        return Mem{t, 0, va};
    }

    // The x86 forms take rax and leave the result in rdx:rax; both are scratch, outside the
    // pool, so no guest value or temporary is there. The other operand goes in rcx. The
    // operands are consumed (operations.hpp), so temporaries holding them are freed, and the
    // results stay in rax and rdx: valid until the next multiply or divide, which no operation
    // makes before using them.
    void load_operands(Value x, Value y) {
        if (y.is_constant) {
            a_.mov(Reg::rcx, y.constant);
        } else {
            a_.mov(Reg::rcx, y.reg);
        }
        if (x.is_constant) {
            a_.mov(Reg::rax, x.constant);
        } else {
            a_.mov(Reg::rax, x.reg);
        }
        for (const Value& v : {x, y}) {
            if (!v.is_constant) {
                registers_.release_temp(v.reg);
            }
        }
    }
    Wide multiply_wide(Value x, Value y, bool is_signed) {
        if (x.is_constant && y.is_constant) {
            const auto p = is_signed
                               ? semantics::DirectBuilder::multiply_wide_signed(x.constant, y.constant)
                               : semantics::DirectBuilder::multiply_wide_unsigned(x.constant,
                                                                                 y.constant);
            return {constant(p.hi), constant(p.lo)};
        }
        load_operands(x, y);
        if (is_signed) {
            a_.imul(Reg::rcx);
        } else {
            a_.mul(Reg::rcx);
        }
        return {reg(Reg::rdx), reg(Reg::rax)};
    }
    // The builder's division, defined for every input: a zero divisor gives 0 and 0 (x86
    // would fault), and a signed division by -1 gives the negation and 0 (x86 would fault on
    // the most negative dividend).
    Division divide(Value x, Value y, bool is_signed) {
        if (x.is_constant && y.is_constant) {
            const auto d = is_signed
                               ? semantics::DirectBuilder::divide_signed(x.constant, y.constant)
                               : semantics::DirectBuilder::divide_unsigned(x.constant, y.constant);
            return {constant(d.quotient), constant(d.remainder)};
        }
        load_operands(x, y);
        const Label zero = a_.label();
        const Label done = a_.label();
        a_.test(Reg::rcx, Reg::rcx);
        a_.jcc(Cond::e, zero);
        if (is_signed) {
            const Label minus_one = a_.label();
            a_.cmp(Reg::rcx, -1);
            a_.jcc(Cond::e, minus_one);
            a_.cqo();
            a_.idiv(Reg::rcx);
            a_.jmp(done);
            a_.bind(minus_one);
            a_.neg(Reg::rax);
            a_.xor_(Reg::rdx, Reg::rdx);
            a_.jmp(done);
        } else {
            a_.xor_(Reg::rdx, Reg::rdx);
            a_.div(Reg::rcx);
            a_.jmp(done);
        }
        a_.bind(zero);
        a_.xor_(Reg::rax, Reg::rax);
        a_.xor_(Reg::rdx, Reg::rdx);
        a_.bind(done);
        return {reg(Reg::rax), reg(Reg::rdx)};
    }

    using Fold = std::uint64_t (*)(std::uint64_t, std::uint64_t);
    using RegOp = void (Assembler::*)(Reg, Reg);
    using ImmOp = void (Assembler::*)(Reg, std::int32_t);
    Value binary(Value x, Value y, Fold fold, RegOp reg_op, ImmOp imm_op) {
        if (x.is_constant && y.is_constant) {
            return constant(fold(x.constant, y.constant));
        }
        const Reg r = copy(x);
        if (y.is_constant && fits_int32(y.constant)) {
            (a_.*imm_op)(r, static_cast<std::int32_t>(y.constant));
        } else {
            (a_.*reg_op)(r, materialize(y));
        }
        return reg(r);
    }

    using ShiftFold = std::uint64_t (*)(std::uint64_t, unsigned);
    using ShiftOp = void (Assembler::*)(Reg, std::uint8_t);
    Value shift(Value v, unsigned n, ShiftFold fold, ShiftOp op) {
        if (v.is_constant) {
            return constant(fold(v.constant, n));
        }
        const Reg r = copy(v);
        if (n != 0) {
            (a_.*op)(r, static_cast<std::uint8_t>(n));
        }
        return reg(r);
    }
    using ShiftClOp = void (Assembler::*)(Reg);
    Value shift_by(Value v, Value n, ShiftClOp op) {
        const Reg r = copy(v);
        a_.mov(Reg::rcx, n.reg);
        (a_.*op)(r);
        return reg(r);
    }

    using CompareFold = bool (*)(std::uint64_t, std::uint64_t);
    Condition compare(Value x, Value y, Cond c, CompareFold fold) {
        if (x.is_constant && y.is_constant) {
            return constant(fold(x.constant, y.constant) ? 1 : 0);
        }
        const Reg left = materialize(x);
        if (y.is_constant && fits_int32(y.constant)) {
            a_.cmp(left, static_cast<std::int32_t>(y.constant));
        } else {
            a_.cmp(left, materialize(y));
        }
        const Reg r = registers_.temp();
        a_.setcc(c, r);
        a_.movzx_byte(r, r);
        return reg(r);
    }

    Condition overflow(Value x, Value y, CompareFold fold, RegOp reg_op, ImmOp imm_op) {
        if (x.is_constant && y.is_constant) {
            return constant(fold(x.constant, y.constant) ? 1 : 0);
        }
        const Reg r = copy(x);
        if (y.is_constant && fits_int32(y.constant)) {
            (a_.*imm_op)(r, static_cast<std::int32_t>(y.constant));
        } else {
            (a_.*reg_op)(r, materialize(y));
        }
        const Reg flag = registers_.temp();
        a_.setcc(Cond::o, flag);
        a_.movzx_byte(flag, flag);
        return reg(flag);
    }

    Assembler& a_;
    GuestRegisters& registers_;
    const Cpu::NativeLayout& layout_;
    // Whether the host has MOVBE (byte-swapping loads and stores).
    bool movbe_;
    std::uint64_t pc_;
    std::vector<SideExit>& raises_;
    std::vector<SideExit>& slow_;
};

class X86_64NativeCode final : public NativeCode {
  public:
    X86_64NativeCode(Cpu& cpu, std::size_t code_bytes)
        : cpu_{cpu}, layout_{cpu.native_layout()}, memory_{code_bytes} {
        // Profiling only: ULTRAVIOLENT_PERF_MAP names generated code for perf
        // (/tmp/perf-<pid>.map, perf's convention for code it cannot find in a file).
        if (std::getenv("ULTRAVIOLENT_PERF_MAP") != nullptr) {
            char path[64];
            std::snprintf(path, sizeof path, "/tmp/perf-%d.map", static_cast<int>(::getpid()));
            perf_map_ = std::fopen(path, "a");
        }
        // MOVBE: CPUID leaf 1, ECX bit 22. ULTRAVIOLENT_NO_MOVBE tests the other path.
        unsigned eax = 0;
        unsigned ebx = 0;
        unsigned ecx = 0;
        unsigned edx = 0;
        movbe_ = __get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0 && (ecx & (1u << 22)) != 0 &&
                 std::getenv("ULTRAVIOLENT_NO_MOVBE") == nullptr;
        // ULTRAVIOLENT_JIT_DUMP=<file>: each block's guest words and host code, in hex.
        if (const char* dump = std::getenv("ULTRAVIOLENT_JIT_DUMP")) {
            jit_dump_ = std::fopen(dump, "a");
        }
        // The helper table.
        auto table = memory_.allocate(helper_count * sizeof(std::uint64_t), 64);
        invariant(!table.writable.empty(), "code region holds the helper table");
        const std::uint64_t helpers[helper_count] = {
            reinterpret_cast<std::uint64_t>(&operation_helper),
            reinterpret_cast<std::uint64_t>(&raise_helper),
            reinterpret_cast<std::uint64_t>(&internal_event_helper),
        };
        std::memcpy(table.writable.data(), helpers, sizeof helpers);
        table_ = table.executable;
        emit_shared();
        memory_.make_permanent();
    }

    X86_64NativeCode(const X86_64NativeCode&) = delete;
    X86_64NativeCode& operator=(const X86_64NativeCode&) = delete;
    ~X86_64NativeCode() override {
        for (std::FILE* f : {perf_map_, jit_dump_}) {
            if (f != nullptr) {
                std::fclose(f);
            }
        }
    }

    const void* translate(const Block& block, BlockSource& more,
                          std::vector<RegionEntry>& entries) override {
        const auto start = std::chrono::steady_clock::now();
        const void* code = emit_region(block, more, entries, max_region_blocks);
        if (code == nullptr) {
            // The code region is full: discard every block and start again (between blocks,
            // so nothing discarded is running), with the block alone if the region still does
            // not fit.
            memory_.discard();
            ++epoch_;
            ++statistics_.discards;
            code = emit_region(block, more, entries, max_region_blocks);
            if (code == nullptr) {
                code = emit_region(block, more, entries, 1);
            }
        }
        statistics_.translation_nanoseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 start)
                .count());
        return code;
    }

    NativeExit run(const void* code, const std::uint64_t& limit,
                   const DispatchEntry* dispatch) override {
        Context context{.cpu = &cpu_,
                        .limit = &limit,
                        .deadline = std::min(limit, cpu_.next_internal_event_cycle()),
                        .dispatch = dispatch,
                        .code_changes = cpu_.code_changes()};
        const std::uint64_t exit = entry_(&context, code);
        return {.reason = static_cast<BlockExit>(exit & 0xff)};
    }

    [[nodiscard]] std::uint64_t epoch() const override {
        return epoch_;
    }
    [[nodiscard]] const NativeStatistics& statistics() const override {
        return statistics_;
    }

  private:
    using Entry = std::uint64_t (*)(Context*, const void*);

    Mem field(std::int32_t offset) const {
        return Mem{Reg::rbx, offset};
    }
    std::uint64_t slot(Helper helper) const {
        return helper_slot(table_, helper);
    }

    // Copies finished code into the region; its address, or 0 when it does not fit.
    std::uint64_t place(Assembler& a) {
        const auto bytes = a.finish();
        auto space = memory_.allocate(bytes.size());
        if (space.writable.empty()) {
            return 0;
        }
        invariant(space.executable == a.here() - bytes.size(), "code assembled where it runs");
        std::ranges::copy(bytes, space.writable.begin());
        return space.executable;
    }
    std::uint64_t place_permanent(Assembler& a, const char* name) {
        const std::uint64_t at = place(a);
        invariant(at != 0, "code region holds the shared code");
        name_code(at, a.size(), name);
        return at;
    }
    void name_code(std::uint64_t at, std::size_t size, const char* name) {
        if (perf_map_ != nullptr) {
            std::fprintf(perf_map_, "%llx %zx uv:%s\n", static_cast<unsigned long long>(at), size,
                         name);
            std::fflush(perf_map_);
        }
    }

    // The entry, which saves the callee-saved registers, loads rbx and r12, and calls the
    // first block (six pushes after the caller's call leave rsp 16-byte aligned at the blocks'
    // helper calls); the exits, which return through it; and dispatch.
    void emit_shared() {
        {
            Assembler a{next_address()};
            for (const Reg r : {Reg::rbx, Reg::rbp, Reg::r12, Reg::r13, Reg::r14, Reg::r15}) {
                a.push(r);
            }
            a.mov(Reg::r12, Reg::rdi);
            a.mov(Reg::rbx, Mem{Reg::rdi, context_cpu});
            a.mov(Reg::r15, field(layout_.cycles));
            a.call(Reg::rsi);
            for (const Reg r : {Reg::r15, Reg::r14, Reg::r13, Reg::r12, Reg::rbp, Reg::rbx}) {
                a.pop(r);
            }
            a.ret();
            entry_ = reinterpret_cast<Entry>(place_permanent(a, "entry"));
        }
        for (std::size_t reason = 0; reason < block_exit_count; ++reason) {
            Assembler a{next_address()};
            a.mov(field(layout_.cycles), Reg::r15);
            a.mov(Reg::rax, std::uint64_t{reason});
            a.ret();
            exits_[reason] = place_permanent(a, "exit");
        }
        {
            // The cycle count reached the deadline at a block's end: the internal event, if
            // that is what arrived, then back to the engine.
            Assembler a{next_address()};
            const Label done = a.label();
            a.cmp(Reg::r15, field(layout_.timer_cycle));
            a.jcc(Cond::ne, done);
            a.mov(Reg::rdi, Reg::rbx);
            call_helper(a, helper_internal_event);
            a.bind(done);
            a.jmp_to(exit(BlockExit::run_limit));
            deadline_ = place_permanent(a, "deadline");
        }
        {
            // rax: the pc of the next block; the CPU's state is exact at a boundary.
            Assembler a{next_address()};
            const std::uint64_t miss = exit(BlockExit::completed);
            a.mov32(Reg::rcx, Reg::rax);
            a.shr32(Reg::rcx, 2);
            a.and_(Reg::rcx, static_cast<std::int32_t>(dispatch_entries - 1));
            a.shl(Reg::rcx, 5);
            a.add(Reg::rcx, Mem{Reg::r12, context_dispatch});
            a.cmp(Reg::rax, Mem{Reg::rcx, dispatch_pc});
            a.jcc_to(Cond::ne, miss);
            a.mov(Reg::rdx, field(layout_.translation_context));
            a.cmp(Reg::rdx, Mem{Reg::rcx, dispatch_context});
            a.jcc_to(Cond::ne, miss);
            a.mov(Reg::rdx, field(layout_.code_changes));
            a.cmp(Reg::rdx, Mem{Reg::rcx, dispatch_stamp});
            a.jcc_to(Cond::ne, miss);
            a.jmp(Mem{Reg::rcx, dispatch_code});
            dispatch_ = place_permanent(a, "dispatch");
        }
    }

    [[nodiscard]] std::uint64_t exit(BlockExit reason) const {
        return exits_[static_cast<std::size_t>(reason)];
    }
    // A helper call: the cycle count into the Cpu before, and back after.
    void call_helper(Assembler& a, Helper helper) const {
        a.mov(field(layout_.cycles), Reg::r15);
        a.call_indirect(slot(helper));
        a.mov(Reg::r15, field(layout_.cycles));
    }

    // Whether `op` is emitted natively in the current translation context: in builder form, and
    // not a MIPS III operation that Status (fixed by the block's key) makes reserved, which
    // call_semantics raises.
    bool native(const DecodedInstruction& op) const {
        return !(semantics::needs_64bit_operations[std::to_underlying(op.opcode)] &&
                 !cpu_.allows_64bit_operations());
    }

    // What the translation knows at the boundary before an operation (JIT.adoc "Boundaries").
    // pc is always the operation's address. The CPU's pc, next pc, and delay-slot flag hold
    // this boundary's values only when `exact`; a branch has stored next pc into the CPU
    // (`next_in_memory`). The cycle count in the CPU is the block's starting count plus
    // `cycles`.
    struct Boundary {
        bool next_in_memory{};
        bool delay_slot{};
        bool exact{true};
        std::uint64_t cycles{};
    };

    // Writes the boundary before operation `index` into the CPU (pc, next pc, delay slot,
    // cycles), for a helper call or a side exit.
    void materialize(Assembler& a, const Block& block, std::size_t index, Boundary& b) {
        const std::uint64_t pc = block.start_pc + 4 * index;
        if (!b.exact) {
            store_constant(a, layout_.pc, pc);
            if (!b.next_in_memory) {
                store_constant(a, layout_.next_pc, pc + 4);
            }
            a.mov_byte(field(layout_.delay_slot), b.delay_slot ? 1 : 0);
            b.exact = true;
        }
        if (index > b.cycles) {
            a.add(Reg::r15, static_cast<std::int32_t>(index - b.cycles));
            b.cycles = index;
        }
    }
    void store_constant(Assembler& a, std::int32_t offset, std::uint64_t value) {
        if (fits_int32(value)) {
            a.mov(field(offset), static_cast<std::int32_t>(value));
        } else {
            a.mov(Reg::rcx, value);
            a.mov(field(offset), Reg::rcx);
        }
    }

    // Where the pc after a block's last operation is.
    enum class Final : std::uint8_t {
        constant,  // known when translating
        next_pc,   // in the CPU's next pc (the delay slot ran natively after a branch)
        cpu,       // in the CPU's pc, exact (a helper ran last)
    };
    // Regions (JIT.adoc "Regions"): a translation joins blocks of its first block's code page
    // that a block's end reaches at a known pc, and jumps to them without dispatch.
    static constexpr std::size_t max_region_blocks = 16;
    struct RegionBlock {
        const Block* block;
        Label label;
    };
    struct Region {
        Assembler& a;
        BlockSource& more;
        std::size_t max_blocks;
        std::vector<RegionBlock> blocks;
        // pcs `more` had no block for.
        std::vector<std::uint64_t> refused;

        // The label of the region's block at pc, joining it when it can be.
        std::optional<Label> block_at(std::uint64_t pc) {
            for (const RegionBlock& b : blocks) {
                if (b.block->start_pc == pc) {
                    return b.label;
                }
            }
            if (blocks.size() >= max_blocks || std::ranges::contains(refused, pc)) {
                return std::nullopt;
            }
            const Block* block = more.block_at(pc);
            if (block == nullptr) {
                refused.push_back(pc);
                return std::nullopt;
            }
            blocks.push_back({block, a.label()});
            return blocks.back().label;
        }
    };

    // A block's normal end: the guest values in `registers` written back, and the CPU's state
    // made exact for the next block (pc, next pc, delay slot clear, the cycles of `ran`
    // operations); then the deadline check and dispatch. When the next pc is one of `targets`
    // and the region has (or can join) the block there, the end jumps to it after the deadline
    // check instead, leaving pc and next pc for that block to write where they are observed.
    // After a helper (`helpers`) the translation context is checked first: only a state change,
    // which ends its block, changes it, but the jump needs no such argument.
    void end_block(Assembler& a, Region& region, const GuestRegisters::State& registers,
                   Final final, std::uint64_t pc, std::span<const std::uint64_t> targets,
                   std::size_t ran, const Boundary& b, bool delay_written, bool helpers) {
        GuestRegisters::write_back(a, layout_, registers);
        switch (final) {
        case Final::constant:
            a.mov(Reg::rax, pc);
            break;
        case Final::next_pc:
            a.mov(Reg::rax, field(layout_.next_pc));
            break;
        case Final::cpu:
            // A helper left the CPU in a delay slot (a coprocessor branch last): blocks start
            // only outside one.
            a.cmp_byte(field(layout_.delay_slot), 0);
            a.jcc_to(Cond::ne, exit(BlockExit::left_block));
            a.mov(Reg::rax, field(layout_.pc));
            break;
        }
        if (final != Final::cpu && delay_written) {
            a.mov_byte(field(layout_.delay_slot), 0);
        }
        if (ran > b.cycles) {
            a.add(Reg::r15, static_cast<std::int32_t>(ran - b.cycles));
        }
        if (final != Final::cpu) {
            std::vector<std::pair<std::uint64_t, Label>> joined;
            for (const std::uint64_t target : targets) {
                if (const auto label = region.block_at(target)) {
                    joined.emplace_back(target, *label);
                }
            }
            if (!joined.empty()) {
                const Label generic = a.label();
                a.cmp(Reg::r15, Mem{Reg::r12, context_deadline});
                a.jcc(Cond::ae, generic);
                if (helpers) {
                    a.mov(Reg::rcx, cpu_.code_context());
                    a.cmp(Reg::rcx, field(layout_.translation_context));
                    a.jcc(Cond::ne, generic);
                }
                if (final == Final::constant) {
                    a.jmp(joined.front().second);
                } else {
                    for (const auto& [target, label] : joined) {
                        if (fits_int32(target)) {
                            a.cmp(Reg::rax, static_cast<std::int32_t>(target));
                        } else {
                            a.mov(Reg::rcx, target);
                            a.cmp(Reg::rax, Reg::rcx);
                        }
                        a.jcc(Cond::e, label);
                    }
                }
                a.bind(generic);
            }
            a.mov(field(layout_.pc), Reg::rax);
            a.lea(Reg::rcx, Mem{Reg::rax, 4});
            a.mov(field(layout_.next_pc), Reg::rcx);
        }
        a.cmp(Reg::r15, Mem{Reg::r12, context_deadline});
        a.jcc_to(Cond::ae, deadline_);
        a.jmp_to(dispatch_);
    }

    // The next pc after a branch at `pc` whose outcome is `result`, stored into the CPU.
    void store_branch(Assembler& a, EmitBuilder& b, std::uint64_t pc,
                      const EmitBuilder::Result& result) {
        const auto& taken = result.taken;
        const EmitBuilder::Value not_taken = EmitBuilder::constant(pc + 8);
        if (taken.is_constant) {
            const EmitBuilder::Value& next = taken.constant != 0 ? result.target : not_taken;
            if (next.is_constant) {
                store_constant(a, layout_.next_pc, next.constant);
            } else {
                a.mov(field(layout_.next_pc), next.reg);
            }
            return;
        }
        const Reg sequential = b.materialize(not_taken);
        a.test(taken.reg, taken.reg);
        a.cmov(Cond::ne, sequential, b.materialize(result.target));
        a.mov(field(layout_.next_pc), sequential);
    }

    // call_semantics for operation `index` (`op`), with every guest value already in the CPU:
    // the boundary before it made exact, the call (the operation's cycle is its own), and the
    // checks after it. On return the CPU is exact at the boundary after the operation and the
    // cycle count is the block's start plus index + 1; exits leave from here.
    void call_operation(Assembler& a, const Block& block, std::size_t index, Boundary b) {
        const std::size_t count = block.operations.size();
        materialize(a, block, index, b);
        a.mov(Reg::rdi, Reg::rbx);
        a.mov(Reg::rsi, std::bit_cast<std::uint64_t>(block.operations[index]));
        call_helper(a, helper_operation);
        a.test(Reg::rax, Reg::rax);
        a.jcc_to(Cond::e, exit(BlockExit::exception));
        // A bus access is a barrier (IR.adoc "Slow bus paths are barriers"): what it can change
        // is checked here instead of leaving. Code: this block and every dispatch entry may be
        // invalid (back to the engine). The run limit and the next internal event (a helper
        // may also have written Count or Compare): the deadline again. Interrupts: the
        // boundary check below.
        a.mov(Reg::rax, field(layout_.code_changes));
        a.cmp(Reg::rax, Mem{Reg::r12, context_code_changes});
        a.jcc_to(Cond::ne, exit(BlockExit::host_path));
        a.mov(Reg::rax, Mem{Reg::r12, context_limit});
        a.mov(Reg::rax, Mem{Reg::rax, 0});
        a.mov(Reg::rcx, field(layout_.timer_cycle));
        a.cmp(Reg::rcx, Reg::rax);
        a.cmov(Cond::b, Reg::rax, Reg::rcx);
        a.mov(Mem{Reg::r12, context_deadline}, Reg::rax);
        if (index + 1 < count) {
            // The boundary after it, checked as tier 0 checks it.
            a.cmp_byte(field(layout_.interrupt_check), 0);
            a.jcc_to(Cond::ne, exit(BlockExit::pending_exception));
            a.lea(Reg::rcx, Mem{Reg::r15, static_cast<std::int32_t>(count - index - 1)});
            a.cmp(Reg::rcx, Reg::rax);
            a.jcc_to(Cond::a, exit(BlockExit::run_limit));
        }
    }

    // The region starting with `first` in the code region, or null when it does not fit.
    const void* emit_region(const Block& first, BlockSource& more,
                            std::vector<RegionEntry>& entries, std::size_t max_blocks) {
        entries.clear();
        Assembler a{next_address()};
        Region region{.a = a,
                      .more = more,
                      .max_blocks = max_blocks,
                      .blocks = {{&first, a.label()}},
                      .refused = {}};
        std::vector<std::uint64_t> starts;
        std::uint64_t native_count = 0;
        std::uint64_t operations = 0;
        marks_.clear();
        // Blocks join while earlier ones are emitted.
        for (std::size_t k = 0; k < region.blocks.size(); ++k) {
            const RegionBlock b = region.blocks[k];
            starts.push_back(a.here());
            a.bind(b.label);
            mark(a, mark_entry);
            native_count += emit_block(a, region, *b.block);
            operations += b.block->operations.size();
        }
        const std::uint64_t at = place(a);
        if (at == 0) {
            return nullptr;
        }
        invariant(starts.front() == at, "a region starts with its first block");
        for (std::size_t k = 1; k < region.blocks.size(); ++k) {
            entries.push_back({region.blocks[k].block->start_pc,
                               reinterpret_cast<const void*>(starts[k])});
        }
        if (perf_map_ != nullptr) {
            char name[40];
            std::snprintf(name, sizeof name, "block_%llx",
                          static_cast<unsigned long long>(first.start_pc));
            name_code(at, a.size(), name);
        }
        if (jit_dump_ != nullptr) {
            for (std::size_t k = 0; k < region.blocks.size(); ++k) {
                const Block& block = *region.blocks[k].block;
                const std::size_t size = (k + 1 < starts.size() ? starts[k + 1] : at + a.size()) -
                                         starts[k];
                std::fprintf(jit_dump_, "block %llx %llx %zx\nguest",
                             static_cast<unsigned long long>(block.start_pc),
                             static_cast<unsigned long long>(starts[k]), size);
                for (const DecodedInstruction& op : block.operations) {
                    std::fprintf(jit_dump_, " %08x", op.instruction.word);
                }
                std::fprintf(jit_dump_, "\nmarks");
                for (const auto& [offset, kind] : marks_) {
                    const std::uint64_t address = at + offset;
                    if (address >= starts[k] && address < starts[k] + size) {
                        std::fprintf(jit_dump_, " %llx:%d",
                                     static_cast<unsigned long long>(address - starts[k]), kind);
                    }
                }
                std::fprintf(jit_dump_, "\nhost ");
                const auto* bytes = reinterpret_cast<const unsigned char*>(starts[k]);
                for (std::size_t i = 0; i < size; ++i) {
                    std::fprintf(jit_dump_, "%02x", bytes[i]);
                }
                std::fprintf(jit_dump_, "\n");
            }
        }
        statistics_.blocks_translated += region.blocks.size();
        ++statistics_.regions;
        statistics_.region_blocks += region.blocks.size() - 1;
        statistics_.native_bytes += a.size();
        statistics_.native_operations += native_count;
        statistics_.helper_operations += operations - native_count;
        return reinterpret_cast<const void*>(at);
    }

    // Emits `block` at the current position (its label bound there), with its out-of-line
    // exits after it. Returns how many of its operations are native.
    std::uint64_t emit_block(Assembler& a, Region& region, const Block& block) {
        GuestRegisters registers{a, layout_};
        const auto& operations = block.operations;
        const std::size_t count = operations.size();
        const std::uint64_t start = block.start_pc;

        // An operation's side exits, with the boundary before it.
        struct RaiseExit {
            EmitBuilder::SideExit exit;
            std::size_t index;
            Boundary boundary;
        };
        std::vector<RaiseExit> raise_exits;
        // A load or store's slow path: call_semantics, then back to `merge` with the guest
        // values where the fast path leaves them (`after`).
        struct SlowPath {
            EmitBuilder::SideExit exit;
            std::size_t index;
            Boundary boundary;
            Label merge;
            GuestRegisters::State after;
        };
        std::vector<SlowPath> slow_paths;
        // A branch-likely that was not taken: the block ends after it, its slot nullified.
        struct Nullified {
            Label label;
            std::size_t index;
            Boundary boundary;
            GuestRegisters::State registers;
        };
        std::vector<Nullified> nullified;
        std::uint64_t native_count = 0;
        // Whether the CPU's delay-slot flag may be set when the block ends.
        bool delay_written = false;
        // Whether any operation runs through call_semantics.
        bool helpers = false;
        // The pcs a branch's outcome can continue at, when known.
        std::vector<std::uint64_t> targets;

        // At entry: no pending exception, and room for every operation before the deadline,
        // so no boundary inside the block can meet the run limit or the next internal event
        // (JIT.adoc "Boundaries"). Only a helper can change either; it is checked after one.
        // A block entered from another of its region finds pc and next pc unwritten, so the
        // exits write them (and the boundary starts inexact).
        const Label pending = a.label();
        const Label limit = a.label();
        a.cmp_byte(field(layout_.interrupt_check), 0);
        a.jcc(Cond::ne, pending);
        a.lea(Reg::rax, Mem{Reg::r15, static_cast<std::int32_t>(count)});
        a.cmp(Reg::rax, Mem{Reg::r12, context_deadline});
        a.jcc(Cond::a, limit);

        Boundary boundary{.exact = false};
        Final final = Final::constant;
        for (std::size_t i = 0; i < count; ++i) {
            const DecodedInstruction& op = operations[i];
            const std::uint64_t pc = start + 4 * i;
            const bool last = i + 1 == count;
            mark(a, static_cast<int>(i));
            if (native(op)) {
                std::vector<EmitBuilder::SideExit> raises;
                std::vector<EmitBuilder::SideExit> slow;
                registers.begin_operation(i);
                EmitBuilder b{a, registers, layout_, movbe_, pc, raises, slow};
                auto result = operations::translate(b, op.opcode, op.instruction);
                if (!result && op.opcode == DecodedOpcode::cop1 && cpu_.coprocessor_usable(1)) {
                    result = b.cop1(op.instruction, cpu_.fpu_64bit_registers(),
                                    cpu_.allows_64bit_operations());
                }
                if (result) {
                    for (auto& r : raises) {
                        raise_exits.push_back({std::move(r), i, boundary});
                    }
                    if (!slow.empty()) {
                        invariant(result->flow == Flow::sequential, "loads and stores are sequential");
                        const Label merge = a.label();
                        a.bind(merge);
                        for (auto& p : slow) {
                            slow_paths.push_back({std::move(p), i, boundary, merge, registers.state()});
                        }
                        delay_written = delay_written || boundary.delay_slot;
                    }
                    ++native_count;
                    switch (result->flow) {
                    case Flow::sequential:
                        if (boundary.delay_slot) {
                            invariant(last, "a delay slot ends its block");
                            final = Final::next_pc;
                        }
                        boundary = {.next_in_memory = false,
                                    .delay_slot = false,
                                    .exact = false,
                                    .cycles = boundary.cycles};
                        break;
                    case Flow::branch_likely:
                        targets.clear();
                        if (result->target.is_constant &&
                            !(result->taken.is_constant && result->taken.constant == 0)) {
                            targets.push_back(result->target.constant);
                        }
                        if (!result->taken.is_constant || result->taken.constant == 0) {
                            const Label label = a.label();
                            nullified.push_back({label, i, boundary, registers.state()});
                            if (result->taken.is_constant) {
                                // Never taken: the slot is always nullified.
                                a.jmp(label);
                                break;
                            }
                            a.test(result->taken.reg, result->taken.reg);
                            a.jcc(Cond::e, label);
                        }
                        [[fallthrough]];
                    case Flow::branch:
                        if (result->flow == Flow::branch) {
                            targets.clear();
                            const bool always = result->taken.is_constant;
                            if (result->target.is_constant &&
                                !(always && result->taken.constant == 0)) {
                                targets.push_back(result->target.constant);
                            }
                            if (!(always && result->taken.constant != 0)) {
                                targets.push_back(pc + 8);
                            }
                        }
                        invariant(i + 2 == count && !boundary.delay_slot,
                                  "a branch and its delay slot end a block");
                        store_branch(a, b, pc, *result);
                        boundary = {.next_in_memory = true,
                                    .delay_slot = true,
                                    .exact = false,
                                    .cycles = boundary.cycles};
                        break;
                    case Flow::exception_return:
                    case Flow::exception:
                        invariant(false, "builder operations return sequential or branch flows");
                    }
                    registers.end_operation();
                    continue;
                }
                registers.end_operation();
            }
            // call_semantics: every guest value into the CPU first.
            helpers = true;
            registers.flush();
            delay_written = delay_written || boundary.delay_slot;
            call_operation(a, block, i, boundary);
            boundary = {.next_in_memory = false, .delay_slot = false, .exact = true, .cycles = i + 1};
            final = Final::cpu;
            if (!last) {
                a.mov(Reg::rcx, pc + 4);
                a.cmp(Reg::rcx, field(layout_.pc));
                a.jcc_to(Cond::ne, exit(BlockExit::left_block));
                a.cmp_byte(field(layout_.delay_slot), 0);
                a.jcc_to(Cond::ne, exit(BlockExit::left_block));
                final = Final::constant;
            }
        }
        mark(a, mark_end);
        const std::uint64_t sequential_pc = start + 4 * count;
        end_block(a, region, registers.state(), final, sequential_pc,
                  final == Final::next_pc ? std::span<const std::uint64_t>{targets}
                                          : std::span<const std::uint64_t>{&sequential_pc, 1},
                  count, boundary, delay_written, helpers);

        mark(a, mark_out_of_line);
        for (const Nullified& n : nullified) {
            // pc moves past the slot; the slot takes no cycle.
            a.bind(n.label);
            const std::uint64_t next = start + 4 * (n.index + 2);
            end_block(a, region, n.registers, Final::constant, next,
                      std::span<const std::uint64_t>{&next, 1}, n.index + 1, n.boundary,
                      delay_written, helpers);
        }
        for (const auto& [label, reason] :
             {std::pair{pending, BlockExit::pending_exception}, std::pair{limit, BlockExit::run_limit}}) {
            a.bind(label);
            store_constant(a, layout_.pc, start);
            store_constant(a, layout_.next_pc, start + 4);
            a.mov_byte(field(layout_.delay_slot), 0);
            a.jmp_to(exit(reason));
        }
        for (RaiseExit& r : raise_exits) {
            // Side exit (JIT.adoc "Precise exceptions"): the guest values and the boundary before
            // the operation made exact, then the exception and its cycle as the direct builder
            // takes them.
            a.bind(r.exit.label);
            GuestRegisters::write_back(a, layout_, r.exit.registers);
            materialize(a, block, r.index, r.boundary);
            a.mov(Reg::rdi, Reg::rbx);
            a.mov(Reg::rsi, static_cast<std::uint64_t>(r.exit.code));
            call_helper(a, helper_raise);
            a.jmp_to(exit(BlockExit::exception));
        }
        for (const SlowPath& p : slow_paths) {
            // The whole operation through call_semantics, then the fast path's state again: its
            // cycle taken back out of the CPU's count (the fast path counts it at the block's
            // end), the branch outcome back in next pc in a delay slot, and the guest values
            // reloaded.
            a.bind(p.exit.label);
            GuestRegisters::write_back(a, layout_, p.exit.registers);
            call_operation(a, block, p.index, p.boundary);
            a.sub(Reg::r15, static_cast<std::int32_t>(p.index + 1 - p.boundary.cycles));
            if (p.boundary.delay_slot) {
                a.mov(Reg::rax, field(layout_.pc));
                a.mov(field(layout_.next_pc), Reg::rax);
            }
            GuestRegisters::reload(a, layout_, p.after);
            a.jmp(p.merge);
        }

        return native_count;
    }
    // The code dump's marks: where each block's entry check, operations (by index), end, and
    // out-of-line code start, by offset in the region.
    static constexpr int mark_entry = -1;
    static constexpr int mark_end = -2;
    static constexpr int mark_out_of_line = -3;
    void mark(const Assembler& a, int kind) {
        if (jit_dump_ != nullptr) {
            marks_.emplace_back(a.size(), kind);
        }
    }

    // Where the next allocation (16-byte aligned) will execute.
    std::uint64_t next_address() const {
        return memory_.executable_start() + (memory_.used() + 15) / 16 * 16;
    }

    Cpu& cpu_;
    Cpu::NativeLayout layout_;
    ExecutableMemory memory_;
    std::uint64_t table_{};
    Entry entry_{};
    std::array<std::uint64_t, block_exit_count> exits_{};
    std::uint64_t deadline_{};
    std::uint64_t dispatch_{};
    std::uint64_t epoch_{};
    NativeStatistics statistics_;
    bool movbe_{};
    std::FILE* perf_map_{};
    std::FILE* jit_dump_{};
    std::vector<std::pair<std::size_t, int>> marks_;
};

} // namespace

std::unique_ptr<NativeCode> make_native_code(Cpu& cpu, std::size_t code_bytes) {
    return std::make_unique<X86_64NativeCode>(cpu, code_bytes);
}

} // namespace ultraviolent::mips
