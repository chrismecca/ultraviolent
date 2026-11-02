#pragma once

// What BlockInterpreter needs from a tier-1 backend (JIT.adoc): native code for decoded
// blocks, and running it. The backend is host-specific (x86-64 today); the block cache, its
// invalidation, and which blocks native code may continue into stay BlockInterpreter's.

#include <ultraviolent/arch/mips/block.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ultraviolent::mips {

enum class BlockExit : std::uint8_t;
class Cpu;

// Where native code finds the next block when one ends (JIT.adoc "Dispatch"): a direct-mapped
// table by pc that BlockInterpreter fills with blocks it has validated. Native code continues
// into an entry only when its pc, translation context, and stamp match the CPU's pc,
// Cpu::code_changes() (nothing that can invalidate a block happened since it was validated),
// and translation context; anything else returns to BlockInterpreter.
struct DispatchEntry {
    std::uint64_t pc{~std::uint64_t{0}};
    std::uint64_t context{};
    std::uint64_t stamp{};
    const void* code{};
};
inline constexpr std::size_t dispatch_entries = std::size_t{1} << 15;
[[nodiscard]] constexpr std::size_t dispatch_index(std::uint64_t pc) {
    return (pc >> 2) & (dispatch_entries - 1);
}

// Why native code returned (BlockExit), and how many blocks it entered.
//
// BlockExit::completed: a block ended and the next was not in the dispatch table.
// BlockExit::run_limit: the next block might pass the run limit or the CPU's next internal
// event (blocks == 0: the first block; BlockInterpreter runs it decoded, exactly), or the
// limit or event arrived. BlockExit::pending_exception: an interrupt or delayed watch exception
// is pending at a boundary; it is not taken. The others as for tier 0.
struct NativeExit {
    BlockExit reason;
    std::uint64_t blocks;
};

// Blocks a translation may join into one region (JIT.adoc "Regions"), from BlockInterpreter.
class BlockSource {
  public:
    // The block at `pc`, decoded from the code page of the block being translated and in its
    // translation context, so valid exactly while that block is; null when there is none.
    virtual const Block* block_at(std::uint64_t pc) = 0;

  protected:
    ~BlockSource() = default;
};

// A block of a region after its first, and where its code starts.
struct RegionEntry {
    std::uint64_t pc;
    const void* code;
};

struct NativeStatistics {
    std::uint64_t blocks_translated{};
    // Translations, and the blocks they joined after their first.
    std::uint64_t regions{};
    std::uint64_t region_blocks{};
    std::uint64_t native_bytes{};
    std::uint64_t translation_nanoseconds{};
    // Times the code region was full and everything was discarded.
    std::uint64_t discards{};
    // Operations emitted natively, and through call_semantics.
    std::uint64_t native_operations{};
    std::uint64_t helper_operations{};
};

class NativeCode {
  public:
    virtual ~NativeCode() = default;

    // Native code for `block`, valid while epoch() is unchanged; null when it cannot be made.
    // The translation may join blocks from `more` that the block can reach directly; each is
    // appended to `entries`, and its code may be entered on its own while the region is
    // valid. May discard every earlier translation (advancing epoch()): called only between
    // blocks, when no native code is running.
    virtual const void* translate(const Block& block, BlockSource& more,
                                  std::vector<RegionEntry>& entries) = 0;
    // Runs `code`, the block at the CPU's pc, and the blocks it continues into through
    // `dispatch` (dispatch_entries long), until one of them leaves. The CPU is at a boundary
    // with no pending exception and not in a delay slot. `limit` is the run limit.
    virtual NativeExit run(const void* code, const std::uint64_t& limit,
                           const DispatchEntry* dispatch) = 0;
    [[nodiscard]] virtual std::uint64_t epoch() const = 0;
    [[nodiscard]] virtual const NativeStatistics& statistics() const = 0;
};

// Generated-code memory by default: discarded and refilled when full.
inline constexpr std::size_t default_native_code_bytes = std::size_t{64} << 20;

// The backend for this host, or null where tier 1 is not built.
std::unique_ptr<NativeCode> make_native_code(Cpu& cpu,
                                             std::size_t code_bytes = default_native_code_bytes);

} // namespace ultraviolent::mips
