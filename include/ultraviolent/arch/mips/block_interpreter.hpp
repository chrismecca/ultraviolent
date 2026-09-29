#pragma once

#include <ultraviolent/arch/mips/block.hpp>
#include <ultraviolent/arch/mips/cpu.hpp>
#include <ultraviolent/arch/mips/interpreter.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ultraviolent::mips {

// Which execution engine runs instructions (IR.adoc "Stages" E, G). The reference
// interpreter is the default and the oracle; tier 0 is experimental until stage G's gates.
enum class ExecutionEngine : std::uint8_t {
    reference,
    tier0,
};

// Why the tier-0 executor left a block (IR.adoc "Executor").
enum class BlockExit : std::uint8_t {
    // Every operation ran.
    completed,
    // The run limit arrived.
    run_limit,
    // An interrupt or delayed watch exception was taken at a boundary.
    pending_exception,
    // An operation raised an exception.
    exception,
    // An access left the host fast path (a barrier).
    host_path,
    // pc is not the next operation's (a nullified delay slot, or a block entered in a delay
    // slot).
    left_block,
    // The next operation's word changed in memory.
    code_changed,
};
inline constexpr std::size_t block_exit_count = 7;

// Counted per block entry, not per instruction, so they can stay on for benchmarks.
struct BlockStatistics {
    // Blocks built and entered.
    std::uint64_t entries{};
    // Decoded operations executed.
    std::uint64_t operations{};
    // Reference steps taken because no block could be built, by reason.
    std::array<std::uint64_t, no_block_count> fallbacks{};
    // Interrupts and delayed watch exceptions taken before a block was built.
    std::uint64_t pending_at_entry{};
    // Static: built blocks by length and by why they end.
    std::array<std::uint64_t, max_block_length + 1> built_lengths{};
    std::array<std::uint64_t, block_end_count> built_ends{};
    // Dynamic: operations executed per entry, and why execution left the block.
    std::array<std::uint64_t, max_block_length + 1> run_lengths{};
    std::array<std::uint64_t, block_exit_count> exits{};
};

// Tier 0 (IR.adoc "Executor"): builds the block at pc on every entry (no caching yet, stage
// F) and runs its decoded operations through complete_cycle with the reference's
// per-instruction boundary accounting. Where no block can be built, the reference
// interpreter steps. Keeps no architectural state.
class BlockInterpreter {
  public:
    explicit BlockInterpreter(Cpu& cpu) : cpu_{cpu}, reference_{cpu} {}

    // Runs while the CPU's cycle count is below `limit`, which the caller may lower while
    // this runs (Interpreter::run_until).
    void run_until(const std::uint64_t& limit);

    [[nodiscard]] const BlockStatistics& statistics() const {
        return statistics_;
    }

  private:
    void run_block(const std::uint64_t& limit);

    Cpu& cpu_;
    Interpreter reference_;
    Block block_;
    BlockStatistics statistics_;
};

} // namespace ultraviolent::mips
