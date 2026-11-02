#pragma once

#include <ultraviolent/arch/mips/block.hpp>
#include <ultraviolent/arch/mips/cpu.hpp>
#include <ultraviolent/arch/mips/interpreter.hpp>
#include <ultraviolent/arch/mips/native_code.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
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
    // Tier 1: native runs (each continues through blocks until one leaves; counted as one
    // entry), runs refused because the first block might pass the run limit or the next
    // internal event, and blocks put into the dispatch table.
    std::uint64_t native_runs{};
    std::uint64_t native_operations{};
    std::uint64_t native_refusals{};
    std::uint64_t dispatch_installs{};
    // Dispatch entries validated again because something that can invalidate a block
    // happened since their stamp (Cpu::code_changes), and how many of those had become invalid.
    std::uint64_t dispatch_revalidations{};
    std::uint64_t dispatch_invalid{};
};

// Tier 0 (IR.adoc "Executor", "Keeping blocks valid"): decoded blocks in a direct-mapped
// cache, run through complete_cycle with the reference's per-instruction boundary
// accounting. A hit is checked against the translation context, the code epoch, the code
// frame's generation, and the generation of the TLB entry that translated the page, and then
// runs without fetching or decoding. Where no block can be built, the reference interpreter
// steps. Keeps no architectural state.
class BlockInterpreter {
  public:
    // 4096 was chosen for tier 0 alone (IR.adoc "Stage F"). Under tier 1, tier 0 runs only
    // blocks not yet translated, which a small cache keeps evicting before they reach the
    // translation threshold: 32768 measured best on every workload (JIT.adoc "Progress").
    static constexpr std::size_t default_cache_entries = 32768;
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
        clear_dispatch();
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
        const std::atomic<std::uint64_t>* frame_generation{};
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
    void count_exit(BlockExit exit, std::size_t operations, bool cached);

    // Tier 1 (JIT.adoc "Dispatch"). Native code for a block is found through the dispatch
    // table, which outlives tier 0's cache entries: a block evicted from the cache keeps its
    // native code until its dispatch entry is replaced or invalid.
    enum class NativeRun : std::uint8_t {
        ran,
        // Native code cannot run the block at pc now: the CPU is in a delay slot, or the block
        // might pass the run limit or the next internal event. It runs decoded.
        refused,
        // No valid native code for the pc.
        absent,
    };
    NativeRun run_native(const std::uint64_t& limit);
    // Puts the entry's block into the dispatch table, translating it first when it has no
    // native code and has been hit often enough. Returns whether it did.
    bool install_native(Entry& entry);
    // What validates a dispatch entry, besides its pc and context (as for Entry).
    struct DispatchValidity {
        std::uint64_t epoch{};
        const std::atomic<std::uint64_t>* frame_generation{};
        std::uint64_t frame_generation_value{};
        std::uint64_t tlb_generation{};
        std::int8_t tlb_index{-1};
    };
    [[nodiscard]] bool valid(const DispatchValidity& v) const;
    void clear_dispatch();
    // The blocks a translation may join (JIT.adoc "Regions"): built on the first block's code
    // page, in the current translation context (the first block's, which lookup validated).
    class RegionBlocks final : public BlockSource {
      public:
        RegionBlocks(Cpu& cpu, const Block& first, std::vector<std::unique_ptr<Block>>& storage)
            : cpu_{cpu}, first_{first}, storage_{storage} {}
        const Block* block_at(std::uint64_t pc) override;

      private:
        Cpu& cpu_;
        const Block& first_;
        std::vector<std::unique_ptr<Block>>& storage_;
        std::size_t used_{};
    };

    Cpu& cpu_;
    Interpreter reference_;
    std::vector<Entry> cache_;
    std::uint64_t mask_{};
    BlockStatistics statistics_;
    NativeCode* native_{};
    std::uint64_t native_threshold_{default_native_threshold};
    std::vector<DispatchEntry> dispatch_;
    std::vector<DispatchValidity> dispatch_validity_;
    // The backend epoch the dispatch table's code belongs to.
    std::uint64_t dispatch_epoch_{};
    // Reused between translations: RegionBlocks' blocks and the translation's other entries.
    std::vector<std::unique_ptr<Block>> region_storage_;
    std::vector<RegionEntry> region_entries_;
};

} // namespace ultraviolent::mips
