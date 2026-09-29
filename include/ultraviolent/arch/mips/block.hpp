#pragma once

#include <ultraviolent/arch/mips/cpu.hpp>
#include <ultraviolent/arch/mips/decoded.hpp>
#include <ultraviolent/core/byte_order.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ultraviolent::mips {

// Why a block ends where it does (IR.adoc "Blocks").
enum class BlockEnd : std::uint8_t {
    // After a branch or jump and its delay slot.
    control_flow,
    // After an operation that changes translation, mode, interrupt state, or the caches, or
    // after SYNC (OperationClass::state_change).
    state_change,
    // Before an operation left to a reference step.
    unsupported,
    // Before a branch or jump whose delay slot is on the next page, is itself a branch or
    // jump, or is unsupported: the reference interpreter steps the branch.
    delay_slot,
    // Before the end of the 4 KiB page.
    page_boundary,
    // At the maximum length (a branch and its slot are never split).
    length_limit,
};
inline constexpr std::size_t block_end_count = 6;

// Why no block starts at a pc; the reference interpreter steps it instead.
enum class NoBlock : std::uint8_t {
    // A fetch there would fault or go through the bus (Cpu::code_page).
    not_host_backed,
    // The first operation is unsupported.
    unsupported,
    // The first operation is a branch whose delay slot cannot join it (BlockEnd::delay_slot).
    delay_slot,
};
inline constexpr std::size_t no_block_count = 3;

// A run of decoded operations from one 4 KiB virtual page, executed in order from its first
// (IR.adoc "Blocks"). Page containment is a correctness rule; the storage is sized by the
// operations actually decoded.
struct Block {
    std::uint64_t start_pc{};
    // Host bytes of the code page and their byte order, as Cpu::code_page gave them: valid
    // while execution stays on the host fast path.
    const std::byte* page{};
    ByteOrder order{ByteOrder::big};
    BlockEnd end{BlockEnd::control_flow};
    std::vector<DecodedInstruction> operations;

    // The word now in memory for operation `index`.
    [[nodiscard]] std::uint32_t word_now(std::size_t index) const {
        const std::uint64_t offset = (start_pc & 0xfff) + 4 * index;
        return ultraviolent::detail::load_word<std::uint32_t>(page + offset, order);
    }
};

// A conservative first maximum, to be measured (IR.adoc "Open questions"). At least 2, so a
// branch and its delay slot always fit.
inline constexpr std::size_t max_block_length = 64;

// Builds the block that starts at `pc` into `block`, reusing its storage. Returns why no
// block could be built, or nothing when `block` holds at least one operation.
std::optional<NoBlock> build_block(Cpu& cpu, std::uint64_t pc, Block& block,
                                   std::size_t max_length = max_block_length);

} // namespace ultraviolent::mips
