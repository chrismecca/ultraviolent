// Tier 1 on x86-64 (JIT.adoc): decoded blocks as native code.
//
// Register use in generated code: rbx holds the Cpu, r12 the run's Context; both are
// callee-saved, so helper calls keep them. The entry code saves the callee-saved registers
// and calls the block; a block returns its exit in rax (reason in bits 7:0, operations run in
// bits 39:8). Helpers are ordinary functions reached through a table at the start of the code
// region (call [rip + slot]), so no call depends on where the region is mapped.

#include <ultraviolent/arch/mips/block_interpreter.hpp>
#include <ultraviolent/arch/mips/decoded.hpp>
#include <ultraviolent/arch/mips/native_code.hpp>
#include <ultraviolent/core/invariant.hpp>
#include <ultraviolent/jit/executable_memory.hpp>
#include <ultraviolent/jit/x86_64/assembler.hpp>

#include "../semantics.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

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

enum Helper : unsigned {
    helper_operation,
    helper_pending,
    helper_count,
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
        return table_ + helper * sizeof(std::uint64_t);
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
            a.mov_byte(field(layout_.left_host_path), 0);
            a.mov(Reg::rdi, Reg::rbx);
            a.mov(Reg::rsi, std::bit_cast<std::uint64_t>(operations[i]));
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
        statistics_.helper_operations += count;
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
