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
};
constexpr std::int32_t context_limit = offsetof(Context, limit);

// call_semantics: one operation through the shared semantics, and its cycle (complete_cycle).
// Returns whether it retired.
std::uint64_t operation_helper(Cpu* cpu, std::uint64_t operation) {
    return complete_cycle<&execute_inline>(*cpu, std::bit_cast<DecodedInstruction>(operation)) ? 1
                                                                                                  : 0;
}

// A boundary with an interrupt or delayed watch pending: takes it as tier 0 does. Returns
// whether one was taken (the block is then left).
std::uint64_t pending_helper(Cpu* cpu) {
    if (cpu->service_pending_exceptions()) {
        cpu->end_cycle(false);
        return 1;
    }
    return 0;
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
    helper_pending,
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

// The builder that emits (operations.hpp, JIT.adoc "Semantics"). A Value is a translation-time
// constant or a scratch register; an operation's values live only while it is emitted, so the
// pool starts over for every operation. In J2's register file every guest register read loads
// from the Cpu and every write stores to it (JIT.adoc "Guest values").
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
    // A raise_if that held: the exception to take, and where its code jumps.
    struct Raise {
        Label label;
        ExceptionCode code;
    };

    EmitBuilder(Assembler& a, const Cpu::NativeLayout& layout, std::uint64_t pc,
                std::vector<Raise>& raises)
        : a_{a}, layout_{layout}, pc_{pc}, raises_{raises} {}

    [[nodiscard]] std::uint64_t pc() const {
        return pc_;
    }
    [[nodiscard]] static Value constant(std::uint64_t x) {
        return {.is_constant = true, .constant = x};
    }

    Value gpr(unsigned r) {
        if (r == 0) {
            return constant(0);
        }
        return load(layout_.gpr + 8 * static_cast<std::int32_t>(r));
    }
    void set_gpr(unsigned r, Value v) {
        if (r != 0) {
            store(layout_.gpr + 8 * static_cast<std::int32_t>(r), v);
        }
    }
    Value hi() {
        return load(layout_.hi);
    }
    Value lo() {
        return load(layout_.lo);
    }
    void set_hi(Value v) {
        store(layout_.hi, v);
    }
    void set_lo(Value v) {
        store(layout_.lo, v);
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
        const Reg r = allocate();
        a_.movsxd(r, v.reg);
        return reg(r);
    }
    Value zero_extend_32(Value v) {
        if (v.is_constant) {
            return constant(v.constant & 0xffff'ffff);
        }
        const Reg r = allocate();
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
        raises_.push_back({label, code});
        if (c.is_constant) {
            a_.jmp(label);
        } else {
            a_.test(c.reg, c.reg);
            a_.jcc(Cond::ne, label);
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

    // For the translator, after the operation: a scratch register for the flow and cycle code.
    Reg scratch() {
        return allocate();
    }
    Reg materialize(Value v) {
        if (!v.is_constant) {
            return v.reg;
        }
        const Reg r = allocate();
        a_.mov(r, v.constant);
        return r;
    }

  private:
    // Every register but rbx and r12 (the Cpu and the context), rsp, and rcx (shift counts).
    // rbp and r13-r15 are callee-saved, which the entry code saved; no value lives across a
    // helper call. The largest operations (ADD, SRLV) with their flow and cycle use 8.
    static constexpr std::array<Reg, 12> pool = {Reg::rax, Reg::rdx, Reg::rsi, Reg::rdi,
                                                 Reg::r8,  Reg::r9,  Reg::r10, Reg::r11,
                                                 Reg::rbp, Reg::r13, Reg::r14, Reg::r15};

    static Value reg(Reg r) {
        return {.is_constant = false, .reg = r};
    }
    Reg allocate() {
        invariant(next_ < pool.size(), "an operation's values fit the scratch pool");
        return pool[next_++];
    }
    Value load(std::int32_t offset) {
        const Reg r = allocate();
        a_.mov(r, Mem{Reg::rbx, offset});
        return reg(r);
    }
    void store(std::int32_t offset, Value v) {
        const Mem m{Reg::rbx, offset};
        if (v.is_constant && fits_int32(v.constant)) {
            a_.mov(m, static_cast<std::int32_t>(v.constant));
        } else {
            a_.mov(m, materialize(v));
        }
    }
    Reg copy(Value v) {
        const Reg r = allocate();
        if (v.is_constant) {
            a_.mov(r, v.constant);
        } else {
            a_.mov(r, v.reg);
        }
        return r;
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
        const Reg r = allocate();
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
        const Reg flag = allocate();
        a_.setcc(Cond::o, flag);
        a_.movzx_byte(flag, flag);
        return reg(flag);
    }

    Assembler& a_;
    const Cpu::NativeLayout& layout_;
    std::uint64_t pc_;
    std::vector<Raise>& raises_;
    std::size_t next_{};
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
            reinterpret_cast<std::uint64_t>(&pending_helper),
            reinterpret_cast<std::uint64_t>(&raise_helper),
            reinterpret_cast<std::uint64_t>(&internal_event_helper),
        };
        std::memcpy(table.writable.data(), helpers, sizeof helpers);
        table_ = table.executable;
        emit_entry();
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

    NativeExit run(const void* code, const std::uint64_t& limit) override {
        Context context{.cpu = &cpu_, .limit = &limit};
        const std::uint64_t exit = entry_(&context, code);
        return {.reason = static_cast<BlockExit>(exit & 0xff),
                .operations = static_cast<std::uint32_t>(exit >> 8)};
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

    // Saves the callee-saved registers, loads rbx and r12, and calls the block. Six pushes
    // after the caller's call leave rsp 16-byte aligned at the block's helper calls.
    void emit_entry() {
        Assembler a;
        // Assembled at offset 0 and copied: the entry has no position-dependent operands.
        for (const Reg r : {Reg::rbx, Reg::rbp, Reg::r12, Reg::r13, Reg::r14, Reg::r15}) {
            a.push(r);
        }
        a.mov(Reg::r12, Reg::rdi);
        a.mov(Reg::rbx, Mem{Reg::rdi, 0});
        a.call(Reg::rsi);
        for (const Reg r : {Reg::r15, Reg::r14, Reg::r13, Reg::r12, Reg::rbp, Reg::rbx}) {
            a.pop(r);
        }
        a.ret();
        const auto bytes = a.finish();
        auto space = memory_.allocate(bytes.size());
        invariant(!space.writable.empty(), "code region holds the entry");
        std::ranges::copy(bytes, space.writable.begin());
        entry_ = reinterpret_cast<Entry>(space.executable);
    }

    // Whether `op` is emitted natively in the current translation context: in builder form, and
    // not a MIPS III operation that Status (fixed by the block's key) makes reserved, which
    // call_semantics raises.
    bool native(const DecodedInstruction& op) const {
        return !(semantics::needs_64bit_operations[std::to_underlying(op.opcode)] &&
                 !cpu_.allows_64bit_operations());
    }

    // apply_flow (decoded.hpp) for an operation emitted natively.
    void emit_flow(Assembler& a, EmitBuilder& b, const EmitBuilder::Result& result) {
        const Mem pc = field(layout_.pc);
        const Mem next_pc = field(layout_.next_pc);
        const Mem delay_slot = field(layout_.delay_slot);
        const Reg t = b.scratch();
        a.mov(t, next_pc);
        switch (result.flow) {
        case Flow::sequential:
            a.mov(pc, t);
            a.add(t, 4);
            a.mov(next_pc, t);
            a.mov_byte(delay_slot, 0);
            return;
        case Flow::branch: {
            a.mov(pc, t);
            const auto& taken = result.taken;
            if (taken.is_constant) {
                if (taken.constant != 0) {
                    a.mov(next_pc, b.materialize(result.target));
                } else {
                    a.add(t, 4);
                    a.mov(next_pc, t);
                }
            } else {
                const Reg sequential = b.scratch();
                a.lea(sequential, Mem{t, 4});
                a.test(taken.reg, taken.reg);
                a.cmov(Cond::ne, sequential, b.materialize(result.target));
                a.mov(next_pc, sequential);
            }
            a.mov_byte(delay_slot, 1);
            return;
        }
        case Flow::branch_likely: {
            const Label not_taken = a.label();
            const Label done = a.label();
            const auto& taken = result.taken;
            if (!taken.is_constant) {
                a.test(taken.reg, taken.reg);
                a.jcc(Cond::e, not_taken);
            }
            if (!taken.is_constant || taken.constant != 0) {
                a.mov(pc, t);
                a.mov(next_pc, b.materialize(result.target));
                a.mov_byte(delay_slot, 1);
                a.jmp(done);
            }
            // Not taken: the delay slot is nullified.
            a.bind(not_taken);
            a.add(t, 4);
            a.mov(pc, t);
            a.add(t, 4);
            a.mov(next_pc, t);
            a.mov_byte(delay_slot, 0);
            a.bind(done);
            return;
        }
        case Flow::exception_return:
        case Flow::exception:
            break;
        }
        invariant(false, "builder operations return sequential, branch, or jump flows");
    }

    // end_cycle(true) for an operation emitted natively; the internal event out of line.
    void emit_end_cycle(Assembler& a, EmitBuilder& b, std::vector<std::pair<Label, Label>>& events) {
        const Reg t = b.scratch();
        a.mov(t, field(layout_.cycles));
        a.add(t, 1);
        a.mov(field(layout_.cycles), t);
        a.cmp(t, field(layout_.timer_cycle));
        const Label event = a.label();
        const Label resume = a.label();
        a.jcc(Cond::e, event);
        a.bind(resume);
        events.emplace_back(event, resume);
    }

    // The block's code in the region, or null when it does not fit.
    const void* emit_block(const Block& block) {
        // Assembled at the address it will run at: the next 16-byte boundary.
        Assembler a{next_address()};
        const auto& operations = block.operations;
        const std::size_t count = operations.size();

        struct Exit {
            Label label;
            BlockExit reason;
            std::size_t ran;
        };
        std::vector<Exit> exits;
        const auto exit_to = [&](BlockExit reason, std::size_t ran) {
            exits.push_back({a.label(), reason, ran});
            return exits.back().label;
        };
        struct Pending {
            Label label;
            Label resume;
            std::size_t index;
        };
        std::vector<Pending> pendings;
        struct RaiseExit {
            EmitBuilder::Raise raise;
            std::size_t ran;
        };
        std::vector<RaiseExit> raise_exits;
        std::vector<std::pair<Label, Label>> events;
        std::uint64_t native_count = 0;

        for (std::size_t i = 0; i < count; ++i) {
            if (i > 0) {
                // The boundary before operation i, in tier 0's order (IR.adoc "Executor").
                a.mov(Reg::rax, Mem{Reg::r12, context_limit});
                a.mov(Reg::rax, Mem{Reg::rax, 0});
                a.mov(Reg::rcx, field(layout_.cycles));
                a.cmp(Reg::rcx, Reg::rax);
                a.jcc(Cond::ae, exit_to(BlockExit::run_limit, i));
                a.cmp_byte(field(layout_.interrupt_check), 0);
                const Label pending = a.label();
                const Label resume = a.label();
                a.jcc(Cond::ne, pending);
                a.bind(resume);
                pendings.push_back({pending, resume, i});
                a.mov(Reg::rax, field(layout_.pc));
                a.mov(Reg::rcx, block.start_pc + 4 * i);
                a.cmp(Reg::rax, Reg::rcx);
                a.jcc(Cond::ne, exit_to(BlockExit::left_block, i));
            }
            const DecodedInstruction& op = operations[i];
            if (native(op)) {
                std::vector<EmitBuilder::Raise> raises;
                EmitBuilder b{a, layout_, block.start_pc + 4 * i, raises};
                if (const auto result = operations::translate(b, op.opcode, op.instruction)) {
                    // No bus access: the host-path flag stays clear.
                    emit_flow(a, b, *result);
                    emit_end_cycle(a, b, events);
                    for (const auto& r : raises) {
                        raise_exits.push_back({r, i + 1});
                    }
                    ++native_count;
                    continue;
                }
            }
            a.mov_byte(field(layout_.left_host_path), 0);
            a.mov(Reg::rdi, Reg::rbx);
            a.mov(Reg::rsi, std::bit_cast<std::uint64_t>(op));
            a.call_indirect(slot(helper_operation));
            a.test(Reg::rax, Reg::rax);
            a.jcc(Cond::e, exit_to(BlockExit::exception, i + 1));
            a.cmp_byte(field(layout_.left_host_path), 0);
            a.jcc(Cond::ne, exit_to(BlockExit::host_path, i + 1));
        }
        a.jmp(exit_to(BlockExit::completed, count));

        for (const Pending& p : pendings) {
            a.bind(p.label);
            a.mov(Reg::rdi, Reg::rbx);
            a.call_indirect(slot(helper_pending));
            a.test(Reg::rax, Reg::rax);
            a.jcc(Cond::ne, exit_to(BlockExit::pending_exception, p.index));
            a.jmp(p.resume);
        }
        for (const auto& [event, resume] : events) {
            a.bind(event);
            a.mov(Reg::rdi, Reg::rbx);
            a.call_indirect(slot(helper_internal_event));
            a.jmp(resume);
        }
        for (const RaiseExit& r : raise_exits) {
            // Side exit (JIT.adoc "Precise exceptions"): J2 keeps every guest value in the Cpu,
            // so nothing is materialized before taking the exception.
            a.bind(r.raise.label);
            a.mov(Reg::rdi, Reg::rbx);
            a.mov(Reg::rsi, static_cast<std::uint64_t>(r.raise.code));
            a.call_indirect(slot(helper_raise));
            a.jmp(exit_to(BlockExit::exception, r.ran));
        }
        for (const Exit& e : exits) {
            a.bind(e.label);
            a.mov(Reg::rax, static_cast<std::uint64_t>(e.reason) | std::uint64_t{e.ran} << 8);
            a.ret();
        }

        const auto bytes = a.finish();
        auto space = memory_.allocate(bytes.size());
        if (space.writable.empty()) {
            return nullptr;
        }
        invariant(space.executable == a.here() - bytes.size(), "block assembled where it runs");
        std::ranges::copy(bytes, space.writable.begin());
        ++statistics_.blocks_translated;
        statistics_.native_bytes += bytes.size();
        statistics_.native_operations += native_count;
        statistics_.helper_operations += count - native_count;
        return reinterpret_cast<const void*>(space.executable);
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
    std::uint64_t epoch_{};
    NativeStatistics statistics_;
};

} // namespace

std::unique_ptr<NativeCode> make_native_code(Cpu& cpu, std::size_t code_bytes) {
    return std::make_unique<X86_64NativeCode>(cpu, code_bytes);
}

} // namespace ultraviolent::mips
