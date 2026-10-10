#pragma once

#include <ultraviolent/arch/mips/block.hpp>
#include <ultraviolent/arch/mips/cpu.hpp>
#include <ultraviolent/arch/mips/interpreter.hpp>
#include <ultraviolent/arch/mips/native_code.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ultraviolent::mips {

// Which execution engine runs instructions (IR.adoc "Stages" E, G). Tier 0 is the default
// since stage G; the reference interpreter is permanent, the oracle every engine is tested
// against, and what tracing and PC probes use.
enum class ExecutionEngine : std::uint8_t {
    reference,
    tier0,
    // Native code for tier 0's blocks (JIT.adoc), where the host has a backend.
    tier1,
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
    // An access reached the bus (a barrier; BlockStatistics::barriers says what kind).
    host_path,
    // pc is not the next operation's (a nullified delay slot, or a block entered in a delay
    // slot).
    left_block,
};
inline constexpr std::size_t block_exit_count = 6;

// Why a lookup did not find a valid block (IR.adoc "Keeping blocks valid").
enum class CacheMiss : std::uint8_t {
    // The entry was empty.
    cold,
    // The entry held another block (another pc or translation context): replaced.
    tag,
    // The code epoch changed (Config, reset, snapshot load, bus remapping).
    epoch,
    // The code frame was written.
    frame,
    // The TLB entry that translated the code was written or invalidated.
    tlb,
};
inline constexpr std::size_t cache_miss_count = 5;

// Counted per block lookup, not per instruction, so they can stay on for benchmarks.
struct BlockStatistics {
    // Blocks entered, and the decoded operations executed in them (those entered from a
    // cache hit: cached_operations).
    std::uint64_t entries{};
    std::uint64_t operations{};
    std::uint64_t cached_operations{};
    // Reference steps taken because no block could be built, by reason.
    std::array<std::uint64_t, no_block_count> fallbacks{};
    // Interrupts and delayed watch exceptions taken before a lookup.
    std::uint64_t pending_at_entry{};
    // Cache lookups, hits, and misses by reason; blocks built; the most hits one block had.
    std::uint64_t lookups{};
    std::uint64_t hits{};
    std::array<std::uint64_t, cache_miss_count> misses{};
    std::uint64_t blocks_built{};
    std::uint64_t max_reuse{};
    // Static: built blocks by length and by why they end.
    std::array<std::uint64_t, max_block_length + 1> built_lengths{};
    std::array<std::uint64_t, block_end_count> built_ends{};
    // Dynamic: operations executed per entry, why execution left the block, and for host-path
    // exits, what the access was.
    std::array<std::uint64_t, max_block_length + 1> run_lengths{};
    std::array<std::uint64_t, block_exit_count> exits{};
    std::array<std::uint64_t, Cpu::bus_access_count> barriers{};
};

// Tier 0 (IR.adoc "Executor", "Keeping blocks valid"): decoded blocks in a direct-mapped
// cache, run through complete_cycle with the reference's per-instruction boundary
// accounting. A hit is checked against the translation context, the code epoch, the code
// frame's generation, and the generation of the TLB entry that translated the page, and then
// runs without fetching or decoding. Where no block can be built, the reference interpreter
// steps. Keeps no architectural state.
class BlockInterpreter {
  public:
    static constexpr std::size_t default_cache_entries = 4096;
    // Chosen from J2's measurements (JIT.adoc "Progress").
    static constexpr std::uint64_t default_native_threshold = 64;

    explicit BlockInterpreter(Cpu& cpu, std::size_t cache_entries = default_cache_entries);

    // Runs while the CPU's cycle count is below `limit`, which the caller may lower while
    // this runs (Interpreter::run_until).
    void run_until(const std::uint64_t& limit);

    // Replaces the cache with an empty one of `entries` (a power of two), between runs.
    void set_cache_entries(std::size_t entries);
    // Runs blocks as native code from `native` (tier 1), or decoded (tier 0) when null.
    // Between runs; not owned.
    void set_native_code(NativeCode* native) {
        native_ = native;
    }
    // Tier 1 translates a block once its cache entry has been hit `hits` times since it was
    // built, and runs it decoded until then: translation costs microseconds, and a block the
    // direct-mapped cache keeps evicting would otherwise be translated again every time
    // (JIT.adoc "Progress", J1). 0 translates on first execution (tests).
    void set_native_threshold(std::uint64_t hits) {
        native_threshold_ = hits;
    }
    [[nodiscard]] std::size_t cache_entries() const {
        return cache_.size();
    }

    [[nodiscard]] const BlockStatistics& statistics() const {
        return statistics_;
    }

  private:
    static constexpr std::uint64_t empty_pc = ~std::uint64_t{0};

    struct Entry {
        std::uint64_t pc{empty_pc};
        std::uint64_t context{};
        std::uint64_t epoch{};
        const std::uint64_t* frame_generation{};
        std::uint64_t frame_generation_value{};
        std::uint64_t tlb_generation{};
        std::int8_t tlb_index{-1};
        std::uint64_t uses{};
        Block block;
        // Tier 1: the block's native code, valid while the backend's epoch is native_epoch.
        const void* native{};
        std::uint64_t native_epoch{};
    };

    // The entry holding the valid block for pc, from the cache or newly built; null when no
    // block could be built (the reason is counted).
    Entry* lookup(std::uint64_t pc);
    void run_block(const Block& block, const std::uint64_t& limit, bool cached);
    // Tier 1: runs the entry's native code, translating it first when it has none.
    // Returns false when no native code could be made (the caller runs the block decoded).
    bool run_native(Entry& entry, const std::uint64_t& limit, bool cached);
    void count_exit(BlockExit exit, std::size_t operations, bool cached);

    Cpu& cpu_;
    Interpreter reference_;
    std::vector<Entry> cache_;
    std::uint64_t mask_{};
    BlockStatistics statistics_;
    NativeCode* native_{};
    std::uint64_t native_threshold_{default_native_threshold};
};

} // namespace ultraviolent::mips
