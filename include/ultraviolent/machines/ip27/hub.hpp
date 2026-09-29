#pragma once

#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/interrupt.hpp>
#include <ultraviolent/core/reset.hpp>
#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_time.hpp>
#include <ultraviolent/devices/pcf8584.hpp>
#include <ultraviolent/machines/ip27/directory_memory.hpp>
#include <ultraviolent/xtalk/xtalk.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <utility>

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
    static constexpr std::size_t stored_register_count = 95;

    // Hub chip revision as reported in NI_STATUS_REV_ID (LINUX HUB_REV_*: 6 is Hub 2.4).
    static constexpr unsigned default_revision = 6;

    // `reset_node` resets the node's CPUs when the Hub performs a local reset; the machine
    // supplies it because it owns the wiring.
    // `i2c` is the PCF8584 on the MD junk bus (IP27.adoc "Junk bus"). `directory` is the
    // node's directory memory, whose entry format MD_MEMORY_CONFIG selects. `memory` is the
    // node's physical address space, which the block-transfer engines read and write.
    // `slot_id` is the backplane slot ID the node board reads in MD_SLOTID_USTAT bits 2:0.
    Hub(Scheduler& scheduler, Tracer& tracer, unsigned revision, unsigned slot_id,
        devices::Pcf8584& i2c, DirectoryMemory& directory, AddressSpace& memory,
        std::function<void()> reset_node);

    void reset(ResetKind kind);

    // Wires the interrupt outputs for CPU `slice` to the CPU: INT_PEND0 to input 0 (Cause IP2)
    // and INT_PEND1 to input 1 (IP3) (IP27.adoc "Hub interrupts").
    void connect_cpu(unsigned slice, InterruptSink& cpu);

    // The Hub's crosstalk side (IP27.adoc "Xtalk"). PIO to IO small window `widget` becomes a
    // crosstalk request on the link; with no link connected it fails.
    void connect_xtalk(xtalk::Link& link) {
        xtalk_ = &link;
    }
    [[nodiscard]] MmioTarget& io_window(unsigned widget) {
        return io_windows_[widget];
    }
    // Big window `index` (1-7, 512 MB each): crosstalk requests to the widget and offset its
    // IIO_ITTE register names at the time of each access.
    [[nodiscard]] MmioTarget& big_window(unsigned index) {
        return big_windows_.at(index - 1);
    }
    // The Hub as a crosstalk widget: its II registers (0x400000 up) are its widget
    // configuration space.
    [[nodiscard]] xtalk::Widget& widget() {
        return widget_;
    }

    // Snapshot support (StateImage), register by register.
    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth width) override;
    std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                std::uint64_t value) override;

  private:
    class IoWindow final : public MmioTarget {
      public:
        std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                            AccessWidth width) override;
        std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) override;
        Hub* hub{};
        unsigned widget{};
    };
    class BigWindow final : public MmioTarget {
      public:
        std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                            AccessWidth width) override;
        std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) override;
        // The widget and crosstalk address for `offset` in this window.
        [[nodiscard]] std::pair<unsigned, std::uint64_t> translate(std::uint64_t offset) const;
        Hub* hub{};
        unsigned index{};
    };
    class Widget final : public xtalk::Widget {
      public:
        std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                            AccessWidth width) override;
        std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) override;
        void xtalk_interrupt(std::uint64_t address, std::uint8_t vector) override;
        void xtalk_interrupt_clear(std::uint64_t address, std::uint8_t vector) override;
        bool xtalk_dma_read(std::uint64_t address, std::span<std::byte> bytes) override;
        bool xtalk_dma_write(std::uint64_t address, std::span<const std::byte> bytes) override;
        Hub* hub{};
    };

    [[nodiscard]] std::uint64_t rt_count() const;
    void set_rt_count(std::uint64_t value);
    void set_led(unsigned slice, std::uint64_t value);
    void set_memory_config(std::uint64_t value);
    // Block-transfer engine `engine` register access; offset is relative to its base.
    [[nodiscard]] std::uint64_t read_bte(unsigned engine, std::uint64_t offset) const;
    void write_bte(unsigned engine, std::uint64_t offset, std::uint64_t value);
    void run_bte(unsigned engine);
    void raise_interrupt(unsigned level);
    void local_reset();
    void update_interrupts();

    Scheduler& scheduler_;
    Tracer& tracer_;
    unsigned revision_;
    unsigned slot_id_;
    devices::Pcf8584& i2c_;
    DirectoryMemory& directory_;
    AddressSpace& memory_;
    std::function<void()> reset_node_;
    xtalk::Link* xtalk_{};
    std::array<IoWindow, xtalk::widget_count> io_windows_{};
    std::array<BigWindow, 7> big_windows_{};
    Widget widget_{};
    EventId local_reset_event_;
    // Storage registers (hub.cpp stored_registers).
    std::array<std::uint64_t, stored_register_count> stored_{};
    std::array<std::uint64_t, 2> cpu_enable_{};
    std::array<std::uint64_t, 2> err_stack_addr_{};
    std::array<std::uint64_t, 2> leds_{};
    std::uint64_t ilcsr_{};
    std::uint64_t memory_config_{};
    std::uint64_t refresh_control_{};
    std::uint64_t mlan_ctl_{};
    // IBLS, IBSA, IBDA, IBCT, IBNA, IBIA for BTE 0 and 1.
    std::array<std::array<std::uint64_t, 6>, 2> bte_{};
    std::uint64_t nsri_settings_{};
    // PI_INT_PEND0/1 and PI_INT_MASK0/1 per CPU slice.
    std::array<std::uint64_t, 2> pending_{};
    std::array<std::array<std::uint64_t, 2>, 2> interrupt_mask_{};
    // Per CPU slice: HUB_IP_PEND0, HUB_IP_PEND1_CC, HUB_IP_RT, HUB_IP_PROF.
    std::array<std::array<InterruptLine, 4>, 2> cpu_lines_;
    // RT compare A and B, profile compare.
    std::array<EventId, 3> comparator_events_;
    std::array<bool, 2> rt_pending_{};
    std::array<bool, 2> prof_pending_{};
    std::array<bool, 2> rt_enable_{};
    std::array<bool, 2> prof_enable_{};
    void schedule_comparators();
    void comparator_matched(unsigned comparator);
    // PI_RT_COUNT is derived from virtual time: the value written at `rt_epoch_ticks_`.
    std::uint64_t rt_base_{};
    std::uint64_t rt_epoch_ticks_{};
};

} // namespace ultraviolent::ip27
