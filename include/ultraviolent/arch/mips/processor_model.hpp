#pragma once

#include <ultraviolent/arch/mips/cp0.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ultraviolent::mips {

// decoded.hpp: one value per operation the MIPS processors distinguish.
enum class DecodedOpcode : std::uint8_t;

// Reset-time identity of the processor, supplied by the machine (the fitted part).
struct CpuConfig {
    // PRId: implementation number in bits 15:8, revision in bits 7:0 (R10000: UM 14.13).
    std::uint32_t processor_id{0x0900};
    // Config register as latched from the reset mode bits (UM 14.14). Bit 15 (BE) must match
    // the byte order of the system bus.
    std::uint32_t config{config_big_endian};
    // FPU Implementation and Revision register, CP1 control register 0 (UM 15.4).
    std::uint32_t fpu_id{0x0900};
};

// Instruction-set capabilities a processor personality has (JIT.adoc "Three axes").
enum class Feature : std::uint8_t {
    mips3,
    mips4,
};

// Entries in decode's table (decoded.hpp "decode").
inline constexpr std::size_t decode_table_size = 241;

// A processor personality (ADR-022): the guest-visible CPU implementation, as data that the
// CPU, the decoder, and the semantics consult and that execution engines may query in
// semantic terms (supports, the decode table), never by identity. Immutable; built by the
// machine and owned by Cpu. The R10000 and the R12000 differ only in identity so far; JIT.adoc
// "The processor personality today" lists what moves here when a behavior that differs is
// observed.
class ProcessorModel {
  public:
    [[nodiscard]] std::string_view name() const {
        return name_;
    }
    [[nodiscard]] const CpuConfig& identity() const {
        return identity_;
    }
    [[nodiscard]] bool supports(Feature feature) const {
        return (features_ & (1u << static_cast<unsigned>(feature))) != 0;
    }
    // Which operation each encoding is: the instruction set this processor implements, with
    // encodings it lacks decoding as reserved.
    [[nodiscard]] const std::array<DecodedOpcode, decode_table_size>& operations() const {
        return operations_;
    }
    // Timing: Count advances once every 2^count_shift processor cycles.
    [[nodiscard]] unsigned count_shift() const {
        return count_shift_;
    }

    // The R10000 (UM v2.0): MIPS IV, Count every other cycle.
    friend ProcessorModel r10000(const CpuConfig& identity);
    // The R12000 (ADR-021, slate's processor; SMP.adoc "S6: IP31 / R12000"): as the R10000 in
    // everything observed so far.
    friend ProcessorModel r12000(const CpuConfig& identity);

  private:
    ProcessorModel() = default;

    std::string_view name_;
    CpuConfig identity_;
    unsigned features_{};
    std::array<DecodedOpcode, decode_table_size> operations_{};
    unsigned count_shift_{};
};

ProcessorModel r10000(const CpuConfig& identity = {});
ProcessorModel r12000(const CpuConfig& identity);

} // namespace ultraviolent::mips
