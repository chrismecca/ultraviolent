#pragma once

// What BlockInterpreter needs from a tier-1 backend (JIT.adoc): native code for decoded
// blocks, and running it. The backend is host-specific (x86-64 today); the block cache, its
// invalidation, and the boundary rules stay BlockInterpreter's.

#include <ultraviolent/arch/mips/block.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ultraviolent::mips {

enum class BlockExit : std::uint8_t;
class Cpu;

// Why native code left its block, and how many of its operations ran (BlockInterpreter
// counts them as tier 0 does).
struct NativeExit {
    BlockExit reason;
    std::uint32_t operations;
};

struct NativeStatistics {
    std::uint64_t blocks_translated{};
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
    // May discard every earlier translation (advancing epoch()): called only between blocks,
    // when no native code is running.
    virtual const void* translate(const Block& block) = 0;
    // Runs `code` until it leaves its block. `limit` is read at every boundary (a device
    // access may lower it).
    virtual NativeExit run(const void* code, const std::uint64_t& limit) = 0;
    [[nodiscard]] virtual std::uint64_t epoch() const = 0;
    [[nodiscard]] virtual const NativeStatistics& statistics() const = 0;
};

// Generated-code memory by default: discarded and refilled when full.
inline constexpr std::size_t default_native_code_bytes = std::size_t{64} << 20;

// The backend for this host, or null where tier 1 is not built.
std::unique_ptr<NativeCode> make_native_code(Cpu& cpu,
                                             std::size_t code_bytes = default_native_code_bytes);

} // namespace ultraviolent::mips
