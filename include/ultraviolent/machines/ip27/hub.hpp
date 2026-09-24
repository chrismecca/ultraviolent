#pragma once

#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/interrupt.hpp>
#include <ultraviolent/core/reset.hpp>
#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_time.hpp>
#include <ultraviolent/devices/pcf8584.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>

namespace ultraviolent::ip27 {

// The IP27 Hub ASIC's local register space, reached through IALIAS (IP27.adoc "IO", "Hub").
//
// Registers are modeled one at a time, each when the PROM is observed to depend on it and
// with a recorded source. Accesses to anything else are refused and traced under `hub`, so
// an observation run names the next register the PROM needs.
class Hub final : public MmioTarget {
  public:
    // IALIAS: 8 MB at the start of widget 1's I/O window.
    static constexpr std::uint64_t window_size = 0x80'0000;

    // Real-time counter rate: 1.25 MHz, 800 ns per tick (IP27.adoc "Hub").
    static constexpr Frequency rtc_clock{1'250'000};

    // Registers modeled as plain storage (see hub.cpp).
    static constexpr std::size_t stored_register_count = 16;

    // Hub chip revision as reported in NI_STATUS_REV_ID (LINUX HUB_REV_*: 6 is Hub 2.4).
    static constexpr unsigned default_revision = 6;

    // `reset_node` resets the node's CPUs when the Hub performs a local reset; the machine
    // supplies it because it owns the wiring.
    // `i2c` is the PCF8584 on the MD junk bus (IP27.adoc "Junk bus").
    Hub(Scheduler& scheduler, Tracer& tracer, unsigned revision, devices::Pcf8584& i2c,
        std::function<void()> reset_node);

    void reset(ResetKind kind);

    // Wires the interrupt outputs for CPU `slice` to the CPU: INT_PEND0 to input 0 (Cause IP2)
    // and INT_PEND1 to input 1 (IP3) (IP27.adoc "Hub interrupts").
    void connect_cpu(unsigned slice, InterruptSink& cpu);

    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth width) override;
    std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                std::uint64_t value) override;

  private:
    [[nodiscard]] std::uint64_t rt_count() const;
    void set_rt_count(std::uint64_t value);
    void set_led(unsigned slice, std::uint64_t value);
    void local_reset();
    void update_interrupts();

    Scheduler& scheduler_;
    Tracer& tracer_;
    unsigned revision_;
    devices::Pcf8584& i2c_;
    std::function<void()> reset_node_;
    EventId local_reset_event_;
    // Storage registers (hub.cpp stored_registers).
    std::array<std::uint64_t, stored_register_count> stored_{};
    std::array<std::uint64_t, 2> cpu_enable_{};
    std::array<std::uint64_t, 2> err_stack_addr_{};
    std::array<std::uint64_t, 2> leds_{};
    std::uint64_t ilcsr_{};
    // PI_INT_PEND0/1 and PI_INT_MASK0/1 per CPU slice.
    std::array<std::uint64_t, 2> pending_{};
    std::array<std::array<std::uint64_t, 2>, 2> interrupt_mask_{};
    std::array<std::array<InterruptLine, 2>, 2> cpu_lines_;
    // PI_RT_COUNT is derived from virtual time: the value written at `rt_epoch_ticks_`.
    std::uint64_t rt_base_{};
    std::uint64_t rt_epoch_ticks_{};
};

} // namespace ultraviolent::ip27
