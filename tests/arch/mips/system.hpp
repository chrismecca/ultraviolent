#pragma once

#include "arch/mips/assembler.hpp"

#include <ultraviolent/arch/mips/cp0.hpp>
#include <ultraviolent/arch/mips/cpu.hpp>
#include <ultraviolent/arch/mips/interpreter.hpp>
#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/invariant.hpp>
#include <ultraviolent/core/memory_block.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>

#include <cstdint>
#include <initializer_list>

namespace ultraviolent::mips::testing {

// Virtual addresses of the unmapped kernel segments for a physical address.
constexpr std::uint64_t kseg0(std::uint64_t physical) {
    return 0xffff'ffff'8000'0000 + physical;
}
constexpr std::uint64_t kseg1(std::uint64_t physical) {
    return 0xffff'ffff'a000'0000 + physical;
}

// Exception vectors with BEV clear.
inline constexpr std::uint64_t general_vector = 0xffff'ffff'8000'0180;
inline constexpr std::uint64_t refill_vector = 0xffff'ffff'8000'0000;
inline constexpr std::uint64_t xrefill_vector = 0xffff'ffff'8000'0080;

// A CPU on a big-endian bus with 1 MiB of RAM at physical 0 and 64 KiB at the physical image
// of the reset vector. No machine: just enough memory to execute synthetic programs. Cached
// and uncached references (with any uncached attribute) reach the same memory.
struct System {
    static constexpr std::uint64_t ram_size = 0x10'0000;
    static constexpr std::uint64_t boot_base = 0x1fc0'0000;
    // Where test programs are placed, and the scratch data area they use.
    static constexpr std::uint64_t code = 0x1000;
    static constexpr std::uint64_t data = 0x8000;

    explicit System(const CpuConfig& config = {}) : cpu{bus, tracer, config} {
        const std::uint64_t windows[] = {
            0,
            system_address::uncached_window(0),
            system_address::uncached_window(1),
            system_address::uncached_window(2),
            system_address::uncached_window(3),
        };
        for (const std::uint64_t window : windows) {
            invariant(bus.map_memory({PhysicalAddress{window}, ram_size}, ram, 0,
                                     MemoryAccess::read_write)
                          .has_value(),
                      "test RAM mapping");
            invariant(bus.map_memory({PhysicalAddress{window + boot_base}, boot.size()}, boot, 0,
                                     MemoryAccess::read_write)
                          .has_value(),
                      "test boot mapping");
        }
    }

    // Stores instruction words at a physical address.
    void load(std::uint64_t physical, std::initializer_list<std::uint32_t> words) {
        for (const std::uint32_t word : words) {
            invariant(bus.write(PhysicalAddress{physical}, AccessWidth::bits32, word).has_value(),
                      "test program store");
            physical += 4;
        }
    }

    void poke(std::uint64_t physical, AccessWidth width, std::uint64_t value) {
        invariant(bus.write(PhysicalAddress{physical}, width, value).has_value(), "test poke");
    }
    std::uint64_t peek(std::uint64_t physical, AccessWidth width) {
        return bus.read(PhysicalAddress{physical}, width).value();
    }

    // Kernel mode with 64-bit addressing, BEV and ERL clear, executing from kseg0.
    void start_kernel(std::uint64_t pc = kseg0(code), std::uint32_t extra_status = status::kx) {
        cpu.mtc0(cp0::status, extra_status);
        jump(pc);
    }

    void jump(std::uint64_t pc) {
        cpu.state().pc = pc;
        cpu.state().next_pc = pc + 4;
        cpu.state().delay_slot = false;
    }

    // Loads a program at the code area, starts it in kernel mode, and runs one step per
    // instruction word.
    void run_program(std::initializer_list<std::uint32_t> words,
                     std::uint32_t extra_status = status::kx) {
        load(code, words);
        start_kernel(kseg0(code), extra_status);
        interpreter.run(words.size());
    }

    // Maps user virtual 0x0-0x1ffff to physical 0x0-0x1ffff with one global TLB entry of
    // 64 KiB pages, so tests can run code and touch data in User mode.
    void map_user_low() {
        const std::uint64_t flags =
            (3u << entry_lo::c_shift) | entry_lo::d | entry_lo::v | entry_lo::g;
        cpu.dmtc0(cp0::entry_hi, 0);
        cpu.dmtc0(cp0::entry_lo0, (std::uint64_t{0x00} << entry_lo::pfn_shift) | flags);
        cpu.dmtc0(cp0::entry_lo1, (std::uint64_t{0x10} << entry_lo::pfn_shift) | flags);
        cpu.mtc0(cp0::page_mask, 0xfu << 13);
        cpu.mtc0(cp0::index, 0);
        cpu.tlb_write_indexed();
    }

    // Runs a program in User mode from the code area (which map_user_low has mapped).
    void run_user_program(std::initializer_list<std::uint32_t> words, std::uint32_t extra_status) {
        load(code, words);
        cpu.mtc0(cp0::status, (2u << status::ksu_shift) | extra_status);
        jump(code);
        interpreter.run(words.size());
    }

    [[nodiscard]] std::uint64_t gpr(unsigned n) const {
        return cpu.state().gpr[n];
    }
    void set(unsigned n, std::uint64_t value) {
        cpu.set_gpr(n, value);
    }
    [[nodiscard]] std::uint64_t pc() const {
        return cpu.state().pc;
    }
    [[nodiscard]] std::uint32_t exception_code() const {
        return static_cast<std::uint32_t>((cpu.mfc0(cp0::cause) >> cause::exc_code_shift) & 0x1f);
    }

    VirtualClock clock;
    Tracer tracer{clock};
    MemoryBlock ram{ram_size};
    MemoryBlock boot{0x1'0000};
    AddressSpace bus{ByteOrder::big};
    Cpu cpu;
    Interpreter interpreter{cpu};
};

} // namespace ultraviolent::mips::testing
