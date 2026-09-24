#pragma once

#include <ultraviolent/arch/mips/cpu.hpp>
#include <ultraviolent/arch/mips/instruction.hpp>

#include <cstdint>

namespace ultraviolent::mips {

// FCSR fields (UM 15.4, Figure 15-7).
namespace fcsr {
inline constexpr std::uint32_t rounding_mask = 3;
inline constexpr unsigned flag_shift = 2;
inline constexpr unsigned enable_shift = 7;
inline constexpr unsigned cause_shift = 12;
// IEEE exception bits, in the order used by the Flag, Enable, and Cause fields.
inline constexpr std::uint32_t inexact = 1u << 0;
inline constexpr std::uint32_t underflow = 1u << 1;
inline constexpr std::uint32_t overflow = 1u << 2;
inline constexpr std::uint32_t divide_by_zero = 1u << 3;
inline constexpr std::uint32_t invalid = 1u << 4;
// Cause.E, unimplemented operation: always enabled.
inline constexpr std::uint32_t unimplemented = 1u << 5;
inline constexpr std::uint32_t condition0 = 1u << 23;
inline constexpr std::uint32_t flush = 1u << 24;
// Bits 22:18 are unimplemented and read as zero.
inline constexpr std::uint32_t writable = ~(0x1fu << 18);
} // namespace fcsr

// Executes a COP1, COP1X, or FPU load/store instruction. The caller has already checked that
// coprocessor 1 is usable and, for COP1X, that MIPS IV is enabled.
ExecutionResult execute_fpu(Cpu& cpu, Instruction instruction);

// FCSR condition bit `cc` (0-7).
bool fpu_condition(const Cpu& cpu, unsigned cc);

} // namespace ultraviolent::mips
