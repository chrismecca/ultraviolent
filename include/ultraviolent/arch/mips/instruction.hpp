#pragma once

#include <ultraviolent/arch/mips/cpu.hpp>

#include <cstdint>
#include <expected>

namespace ultraviolent::mips {

// Field view of one 32-bit MIPS instruction word.
struct Instruction {
    std::uint32_t word;

    [[nodiscard]] unsigned opcode() const {
        return word >> 26;
    }
    [[nodiscard]] unsigned rs() const {
        return (word >> 21) & 31;
    }
    [[nodiscard]] unsigned rt() const {
        return (word >> 16) & 31;
    }
    [[nodiscard]] unsigned rd() const {
        return (word >> 11) & 31;
    }
    [[nodiscard]] unsigned sa() const {
        return (word >> 6) & 31;
    }
    [[nodiscard]] unsigned funct() const {
        return word & 63;
    }
    // 16-bit immediate, zero-extended.
    [[nodiscard]] std::uint64_t immediate() const {
        return word & 0xffff;
    }
    // 16-bit immediate, sign-extended to 64 bits.
    [[nodiscard]] std::uint64_t signed_immediate() const {
        return static_cast<std::uint64_t>(std::int64_t{static_cast<std::int16_t>(word & 0xffff)});
    }
    // 26-bit jump index.
    [[nodiscard]] std::uint64_t target() const {
        return word & 0x03ff'ffff;
    }
};

// How an executed instruction changes the flow of control.
enum class Flow : std::uint8_t {
    sequential,
    // Delay slot always executes.
    branch,
    // Delay slot executes only when taken.
    branch_likely,
    // ERET: no delay slot.
    exception_return,
};

struct Control {
    Flow flow{Flow::sequential};
    bool taken{};
    std::uint64_t target{};
};

using ExecutionResult = std::expected<Control, Exception>;

} // namespace ultraviolent::mips
