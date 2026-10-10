// Tier 1 on x86-64 (JIT.adoc): decoded blocks as native code.
//
// Register use in generated code: rbx holds the Cpu, r12 the run's Context; both are
// callee-saved, so helper calls keep them. The entry code saves the callee-saved registers
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
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

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
    // Blocks entered.
    std::uint64_t blocks;
    const DispatchEntry* dispatch;
    // Cpu::code_changes() when the run started: every block entered was valid then, and stays
    // valid while it is unchanged.
    std::uint64_t code_changes;
};
constexpr std::int32_t context_cpu = offsetof(Context, cpu);
constexpr std::int32_t context_limit = offsetof(Context, limit);
constexpr std::int32_t context_deadline = offsetof(Context, deadline);
constexpr std::int32_t context_blocks = offsetof(Context, blocks);
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
    // Every register but rbx and r12 (the Cpu and the context), rsp, and rcx. rbp and
    // r13-r15 are callee-saved, which the entry code saved; no value lives across a helper call.
    static constexpr std::array<Reg, 12> pool = {Reg::rax, Reg::rdx, Reg::rsi, Reg::rdi,
                                                 Reg::r8,  Reg::r9,  Reg::r10, Reg::r11,
                                                 Reg::rbp, Reg::r13, Reg::r14, Reg::r15};
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
                std::uint64_t pc, std::vector<SideExit>& raises, std::vector<SideExit>& slow)
        : a_{a}, registers_{registers}, layout_{layout}, pc_{pc}, raises_{raises}, slow_{slow} {}

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
        const Reg host = host_address(address, byte_count(W), false);
        const Reg r = registers_.temp();
        const Mem m{host, 0};
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
            a_.mov32(r, m);
            if (swap) {
                a_.bswap32(r);
            }
            if (SignExtend) {
                a_.movsxd(r, r);
            }
        } else {
            a_.mov(r, m);
            if (swap) {
                a_.bswap64(r);
            }
        }
        return reg(r);
    }
    template <AccessWidth W> bool store(Value address, Value value) {
        const Reg host = host_address(address, byte_count(W), true);
        const Mem m{host, 0};
        const bool swap = layout_.byte_order == ByteOrder::big;
        if constexpr (W == AccessWidth::bits8) {
            if (value.is_constant) {
                a_.mov_byte(m, static_cast<std::uint8_t>(value.constant));
            } else {
                a_.mov_byte(m, value.reg);
            }
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

    // The host address of `size` bytes at guest `address`, after the host page cache tests
    // (Cpu::data_page): the entry for the address's page (bits 17:12 select it), its page
    // (with the address's misaligned bits, which no page has), its translation context, and
    // for a store its store page. A miss branches to the slow path.
    Reg host_address(Value address, std::size_t size, bool store) {
        const Reg va = materialize(address);
        const Reg entry = registers_.temp();
        const Reg t = registers_.temp();
        const auto entry_bits = static_cast<unsigned>(std::countr_zero(layout_.host_page_size));
        a_.mov(entry, va);
        a_.shr(entry, static_cast<std::uint8_t>(12 - entry_bits));
        a_.and_(entry, static_cast<std::int32_t>((layout_.host_page_count - 1) << entry_bits));
        a_.add(entry, Reg::rbx);
        const std::int32_t base = layout_.data_pages;
        a_.mov(t, va);
        a_.and_(t, static_cast<std::int32_t>(~std::uint64_t{0xfff} | (size - 1)));
        const Label slow = a_.label();
        slow_.push_back({slow, registers_.state()});
        a_.cmp(t, Mem{entry, base + (store ? layout_.host_page_store_page : layout_.host_page_page)});
        a_.jcc(Cond::ne, slow);
        a_.mov(t, Mem{Reg::rbx, layout_.translation_context});
        a_.cmp(t, Mem{entry, base + layout_.host_page_context});
        a_.jcc(Cond::ne, slow);
        a_.mov(t, va);
        a_.and_(t, 0xfff);
        a_.add(t, Mem{entry, base + (store ? layout_.host_page_writable_bytes
                                           : layout_.host_page_bytes)});
        return t;
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
    std::uint64_t pc_;
    std::vector<SideExit>& raises_;
    std::vector<SideExit>& slow_;
};

class X86_64NativeCode final : public NativeCode {
  public:
    X86_64NativeCode(Cpu& cpu, std::size_t code_bytes)
        : cpu_{cpu}, layout_{cpu.native_layout()}, memory_{code_bytes} {
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

    const void* translate(const Block& block) override {
        const auto start = std::chrono::steady_clock::now();
        const void* code = emit_block(block);
        if (code == nullptr) {
            // The region is full: discard every block and start again (between blocks, so
            // nothing discarded is running).
            memory_.discard();
            ++epoch_;
            ++statistics_.discards;
            code = emit_block(block);
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
                        .blocks = 0,
                        .dispatch = dispatch,
                        .code_changes = cpu_.code_changes()};
        const std::uint64_t exit = entry_(&context, code);
        return {.reason = static_cast<BlockExit>(exit & 0xff), .blocks = context.blocks};
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
    std::uint64_t place_permanent(Assembler& a) {
        const std::uint64_t at = place(a);
        invariant(at != 0, "code region holds the shared code");
        return at;
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
            a.call(Reg::rsi);
            for (const Reg r : {Reg::r15, Reg::r14, Reg::r13, Reg::r12, Reg::rbp, Reg::rbx}) {
                a.pop(r);
            }
            a.ret();
            entry_ = reinterpret_cast<Entry>(place_permanent(a));
        }
        for (std::size_t reason = 0; reason < block_exit_count; ++reason) {
            Assembler a{next_address()};
            a.mov(Reg::rax, std::uint64_t{reason});
            a.ret();
            exits_[reason] = place_permanent(a);
        }
        {
            // The cycle count reached the deadline at a block's end: the internal event, if
            // that is what arrived, then back to the engine.
            Assembler a{next_address()};
            const Label done = a.label();
            a.mov(Reg::rax, field(layout_.cycles));
            a.cmp(Reg::rax, field(layout_.timer_cycle));
            a.jcc(Cond::ne, done);
            a.mov(Reg::rdi, Reg::rbx);
            a.call_indirect(slot(helper_internal_event));
            a.bind(done);
            a.jmp_to(exit(BlockExit::run_limit));
            deadline_ = place_permanent(a);
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
            dispatch_ = place_permanent(a);
        }
    }

    [[nodiscard]] std::uint64_t exit(BlockExit reason) const {
        return exits_[static_cast<std::size_t>(reason)];
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
            a.add(field(layout_.cycles), static_cast<std::int32_t>(index - b.cycles));
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
    // A block's normal end: the guest values in `registers` written back, and the CPU's state
    // made exact for the next block (pc, next pc, delay slot clear, the cycles of `ran`
    // operations); then the deadline check and dispatch.
    void end_block(Assembler& a, const GuestRegisters::State& registers, Final final,
                   std::uint64_t pc, std::size_t ran, const Boundary& b, bool delay_written) {
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
        if (final != Final::cpu) {
            a.mov(field(layout_.pc), Reg::rax);
            a.lea(Reg::rcx, Mem{Reg::rax, 4});
            a.mov(field(layout_.next_pc), Reg::rcx);
            if (delay_written) {
                a.mov_byte(field(layout_.delay_slot), 0);
            }
        }
        if (ran > b.cycles) {
            a.add(field(layout_.cycles), static_cast<std::int32_t>(ran - b.cycles));
        }
        a.mov(Reg::rcx, field(layout_.cycles));
        a.cmp(Reg::rcx, Mem{Reg::r12, context_deadline});
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
        a.call_indirect(slot(helper_operation));
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
            a.mov(Reg::rcx, field(layout_.cycles));
            a.add(Reg::rcx, static_cast<std::int32_t>(count - index - 1));
            a.cmp(Reg::rcx, Reg::rax);
            a.jcc_to(Cond::a, exit(BlockExit::run_limit));
        }
    }

    // The block's code in the region, or null when it does not fit.
    const void* emit_block(const Block& block) {
        Assembler a{next_address()};
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

        // At entry: no pending exception, and room for every operation before the deadline,
        // so no boundary inside the block can meet the run limit or the next internal event
        // (JIT.adoc "Boundaries"). Only a helper can change either; it is checked after one.
        a.cmp_byte(field(layout_.interrupt_check), 0);
        a.jcc_to(Cond::ne, exit(BlockExit::pending_exception));
        a.mov(Reg::rax, field(layout_.cycles));
        a.add(Reg::rax, static_cast<std::int32_t>(count));
        a.cmp(Reg::rax, Mem{Reg::r12, context_deadline});
        a.jcc_to(Cond::a, exit(BlockExit::run_limit));
        a.add(Mem{Reg::r12, context_blocks}, 1);

        Boundary boundary;
        Final final = Final::constant;
        for (std::size_t i = 0; i < count; ++i) {
            const DecodedInstruction& op = operations[i];
            const std::uint64_t pc = start + 4 * i;
            const bool last = i + 1 == count;
            if (native(op)) {
                std::vector<EmitBuilder::SideExit> raises;
                std::vector<EmitBuilder::SideExit> slow;
                registers.begin_operation(i);
                EmitBuilder b{a, registers, layout_, pc, raises, slow};
                if (const auto result = operations::translate(b, op.opcode, op.instruction)) {
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
        end_block(a, registers.state(), final, start + 4 * count, count, boundary, delay_written);

        for (const Nullified& n : nullified) {
            // pc moves past the slot; the slot takes no cycle.
            a.bind(n.label);
            end_block(a, n.registers, Final::constant, start + 4 * (n.index + 2), n.index + 1,
                      n.boundary, delay_written);
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
            a.call_indirect(slot(helper_raise));
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
            a.add(field(layout_.cycles),
                  -static_cast<std::int32_t>(p.index + 1 - p.boundary.cycles));
            if (p.boundary.delay_slot) {
                a.mov(Reg::rax, field(layout_.pc));
                a.mov(field(layout_.next_pc), Reg::rax);
            }
            GuestRegisters::reload(a, layout_, p.after);
            a.jmp(p.merge);
        }

        const std::uint64_t at = place(a);
        if (at == 0) {
            return nullptr;
        }
        ++statistics_.blocks_translated;
        statistics_.native_bytes += a.size();
        statistics_.native_operations += native_count;
        statistics_.helper_operations += count - native_count;
        return reinterpret_cast<const void*>(at);
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
};

} // namespace

std::unique_ptr<NativeCode> make_native_code(Cpu& cpu, std::size_t code_bytes) {
    return std::make_unique<X86_64NativeCode>(cpu, code_bytes);
}

} // namespace ultraviolent::mips
