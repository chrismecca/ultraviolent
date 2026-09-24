#pragma once

#include <ultraviolent/arch/mips/cp0.hpp>
#include <ultraviolent/arch/mips/tlb.hpp>
#include <ultraviolent/core/address.hpp>
#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/interrupt.hpp>
#include <ultraviolent/core/reset.hpp>
#include <ultraviolent/core/trace.hpp>

#include <array>
#include <cstdint>
#include <expected>

namespace ultraviolent::mips {

// Reset-time identity of the processor, supplied by the machine.
struct CpuConfig {
    // PRId: implementation number 0x09 in bits 15:8, revision in bits 7:0 (UM 14.13).
    std::uint32_t processor_id{0x0900};
    // Config register as latched from the reset mode bits (UM 14.14). Bit 15 (BE) must match
    // the byte order of the system bus.
    std::uint32_t config{config_big_endian};
    // FPU Implementation and Revision register, CP1 control register 0 (UM 15.4).
    std::uint32_t fpu_id{0x0900};
};

enum class OperatingMode : std::uint8_t {
    kernel,
    supervisor,
    user,
};

enum class AccessKind : std::uint8_t {
    fetch,
    load,
    store,
};

// A synchronous or asynchronous exception, before it is taken.
struct Exception {
    ExceptionCode code{};
    // Faulting virtual address for address-error and TLB exceptions.
    std::uint64_t bad_address{};
    // TLB refill (no matching entry): uses the refill vectors when EXL is clear.
    bool tlb_refill{};
    // Refill through the 64-bit (XTLB) vector (UM 17.3).
    bool extended_refill{};
    // Cause.CE for coprocessor-unusable exceptions.
    std::uint8_t coprocessor{};
};

// Encoding of a system-interface address in a PhysicalAddress (ADR-019). The machine decodes
// all three parts: an Origin Hub sends uncached attribute 0 to its special spaces and cached
// references to memory, even at the same physical address.
namespace system_address {
// Physical address, bits 39:0.
inline constexpr std::uint64_t physical_mask = (std::uint64_t{1} << 40) - 1;
// Set for uncached (double/single/partial-word) requests.
inline constexpr std::uint64_t uncached = std::uint64_t{1} << 59;
// Uncached attribute, bits 58:57 (UM 6.23).
inline constexpr unsigned attribute_shift = 57;
// Base of the system addresses of uncached references with `attribute`.
constexpr std::uint64_t uncached_window(unsigned attribute) {
    return uncached | (std::uint64_t{attribute & 3} << attribute_shift);
}
} // namespace system_address

// Result of address translation.
struct Translation {
    // System address as driven on the system interface; see system_address.
    PhysicalAddress address;
    bool uncached{};
};

// Integer architectural state that execution engines read and write directly.
struct IntegerState {
    std::array<std::uint64_t, 32> gpr{};
    std::uint64_t hi{};
    std::uint64_t lo{};
    // Address of the next instruction to execute.
    std::uint64_t pc{};
    // Address of the instruction after it: pc + 4, or a branch target when pc is a delay slot.
    std::uint64_t next_pc{};
    // The instruction at pc is in a branch delay slot.
    bool delay_slot{};
    bool ll_bit{};
};

// Floating-point architectural state (UM 15.3, 15.4). Register numbering follows Status.FR:
// with FR=1 there are 32 64-bit registers; with FR=0 the 32 logical 32-bit registers are the
// halves of the even-numbered physical registers.
struct FpuState {
    std::array<std::uint64_t, 32> fpr{};
    // FCSR, CP1 control register 31.
    std::uint32_t fcsr{};
};

// R10000 architectural CPU (ARCHITECTURE "The CPU is not the interpreter").
//
// Owns every guest-visible register and implements the semantics that all execution engines
// share: operating modes, address translation, memory access, CP0, the TLB, exceptions, and
// interrupts. It does not decode or execute instructions.
//
// Time: one step of an execution engine (one instruction, or one exception taken instead of
// an instruction) is one processor cycle. Count advances every other cycle (UM 14.8).
class Cpu final : public InterruptSink {
  public:
    static constexpr std::uint64_t reset_vector = 0xffff'ffff'bfc0'0000;
    // External interrupt inputs 0-4 set Cause.IP[2]-IP[6].
    static constexpr std::uint32_t external_interrupt_count = 5;

    Cpu(AddressSpace& bus, Tracer& tracer, const CpuConfig& config);
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    // power_on and cold take the Cold Reset exception; warm takes the Soft Reset exception.
    void reset(ResetKind kind);
    void nonmaskable_interrupt();

    void set_interrupt_level(std::uint32_t input, bool asserted) override;

    [[nodiscard]] IntegerState& state() {
        return state_;
    }
    [[nodiscard]] const IntegerState& state() const {
        return state_;
    }
    [[nodiscard]] FpuState& fpu() {
        return fpu_;
    }
    [[nodiscard]] const FpuState& fpu() const {
        return fpu_;
    }
    [[nodiscard]] std::uint32_t fpu_id() const {
        return config_.fpu_id;
    }
    // Status.FR: 32 64-bit floating-point registers.
    [[nodiscard]] bool fpu_64bit_registers() const {
        return (cp0_.status & status::fr) != 0;
    }
    void set_gpr(unsigned index, std::uint64_t value) {
        if (index != 0) {
            state_.gpr[index] = value;
        }
    }

    // Processor cycles since construction.
    [[nodiscard]] std::uint64_t cycles() const {
        return cycles_;
    }

    // Modes and permissions (UM 16.1).
    [[nodiscard]] OperatingMode mode() const;
    [[nodiscard]] bool allows_64bit_operations() const;
    [[nodiscard]] bool allows_mips4() const;
    [[nodiscard]] bool coprocessor_usable(unsigned unit) const;
    // Effective byte order of data and instruction references in the current mode.
    [[nodiscard]] bool big_endian() const;

    // Memory references. Alignment is the caller's concern for translate/read/write; load,
    // store, and fetch check natural alignment first.
    std::expected<Translation, Exception> translate(std::uint64_t address, AccessKind kind);
    std::expected<std::uint64_t, Exception> read(const Translation& translation, AccessWidth width,
                                                 AccessKind kind);
    std::expected<void, Exception> write(const Translation& translation, AccessWidth width,
                                         std::uint64_t value);
    std::expected<std::uint64_t, Exception> load(std::uint64_t address, AccessWidth width);
    std::expected<void, Exception> store(std::uint64_t address, AccessWidth width,
                                         std::uint64_t value);
    std::expected<std::uint32_t, Exception> fetch(std::uint64_t address);

    // CP0 moves with the R10000 width rules (UM 14.26, Table 14-26).
    [[nodiscard]] std::uint64_t mfc0(unsigned reg) const;
    [[nodiscard]] std::uint64_t dmfc0(unsigned reg) const;
    void mtc0(unsigned reg, std::uint64_t value);
    void dmtc0(unsigned reg, std::uint64_t value);

    // TLB instructions (UM 14.34-14.37).
    void tlb_probe();
    void tlb_read();
    void tlb_write_indexed();
    void tlb_write_random();
    [[nodiscard]] const Tlb& tlb() const {
        return tlb_;
    }

    // ERET (UM 14.30): leaves the error or exception level and returns the resume address.
    std::uint64_t exception_return();

    // Takes `exception` for the instruction at state().pc.
    void take_exception(const Exception& exception);

    // At an instruction boundary: takes a pending interrupt or delayed watch exception.
    // Returns true when one was taken.
    bool service_pending_exceptions();

    // Accounts one processor cycle; `retired` when an instruction completed in it.
    void end_cycle(bool retired);

  private:
    struct Cp0Registers {
        std::uint32_t index{};
        std::uint32_t random{};
        std::uint64_t entry_lo0{};
        std::uint64_t entry_lo1{};
        std::uint64_t context{};
        std::uint32_t page_mask{};
        std::uint32_t wired{};
        std::uint64_t bad_vaddr{};
        std::uint32_t count_offset{};
        std::uint64_t entry_hi{};
        std::uint32_t compare{};
        std::uint32_t status{};
        // BD, CE, ExcCode, and the software interrupt bits. IP[7:2] are computed.
        std::uint32_t cause{};
        std::uint64_t epc{};
        std::uint32_t config{};
        std::uint32_t ll_addr{};
        std::uint32_t watch_lo{};
        std::uint32_t watch_hi{};
        std::uint64_t xcontext{};
        std::uint32_t frame_mask{};
        std::uint64_t diagnostic{};
        std::uint32_t ecc{};
        std::uint32_t tag_lo{};
        std::uint32_t tag_hi{};
        std::uint64_t error_epc{};
    };

    [[nodiscard]] std::uint32_t count() const;
    [[nodiscard]] std::uint32_t cause_value() const;
    [[nodiscard]] std::uint64_t read_cp0(unsigned reg) const;
    void write_cp0(unsigned reg, std::uint64_t value);
    [[nodiscard]] TlbEntry tlb_entry_from_registers() const;
    void write_tlb(std::size_t index);

    std::expected<Translation, Exception> translate_mapped(std::uint64_t address, AccessKind kind);
    std::expected<Translation, Exception> translate_compatibility(std::uint64_t address,
                                                                  AccessKind kind);
    std::expected<void, Exception> check_watch(const Translation& translation, AccessKind kind);
    [[nodiscard]] PhysicalAddress bus_address(const Translation& translation,
                                              AccessWidth width) const;
    void enter_reset_vector();

    AddressSpace& bus_;
    Tracer& tracer_;
    CpuConfig config_;
    IntegerState state_;
    FpuState fpu_;
    Cp0Registers cp0_;
    Tlb tlb_;
    std::uint64_t cycles_{};
    // Latched external interrupt requests, Cause.IP[6:2].
    std::uint32_t external_interrupts_{};
    bool timer_interrupt_{};
    bool watch_pending_{};
};

} // namespace ultraviolent::mips
