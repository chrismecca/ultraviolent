#pragma once

#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/interrupt.hpp>
#include <ultraviolent/core/reset.hpp>
#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_time.hpp>
#include <ultraviolent/devices/one_wire.hpp>
#include <ultraviolent/devices/pcf8584.hpp>
#include <ultraviolent/machines/ip27/directory_memory.hpp>
#include <ultraviolent/xtalk/xtalk.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace ultraviolent::ip27 {

// The IP27 Hub ASIC's local register space, reached through IALIAS (IP27.adoc "IO", "Hub").
//
// Registers are modeled one at a time, each when the PROM is observed to depend on it and
// with a recorded source. Accesses to anything else are refused and traced under `hub`, so
// an observation run names the next register the PROM needs.
//
// Each CPU reaches the registers through its own port, as each processor has its own SysAD
// connection into the processor interface (PI): the port is how the Hub knows which CPU is
// asking (PI_CPU_NUM; ADR-023).
class Hub final {
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
    // `cpu_count` CPUs (1 or 2) are installed, from slice A: PI_CPU_PRESENT_A and _B.
    // `event_prefix` keeps a second Hub's scheduler events distinct (snapshots name events).
    Hub(Scheduler& scheduler, Tracer& tracer, unsigned revision, unsigned slot_id,
        unsigned cpu_count, devices::Pcf8584& i2c, DirectoryMemory& directory,
        AddressSpace& memory, std::function<void()> reset_node,
        std::string_view event_prefix = {});

    void reset(ResetKind kind);

    // Wires the interrupt outputs for CPU `slice` to the CPU: INT_PEND0 to input 0 (Cause IP2)
    // and INT_PEND1 to input 1 (IP3) (IP27.adoc "Hub interrupts").
    void connect_cpu(unsigned slice, InterruptSink& cpu);
    // A PIO by the CPU in `slice` of this node that no crosstalk widget answered, at physical
    // `address`: PI_ERR_STATUS0 of that slice records it (IP27.adoc "PIO errors").
    void record_pio_error(unsigned slice, std::uint64_t address, bool write);
    // The module system controller has a message: INT_PEND1 bit 63 (MSC_PANIC_INTR among the
    // INT_PEND1 bits the IRIX-derived Linux sn/intr.h lists as hard-coded; IRIX's sysctlr.c
    // msc_intr fetches the message on it). inferred: the controller sets the bit and IRIX
    // clears it, as it clears every INT_PEND1 bit before its handler (IP27.adoc "System
    // controller").
    void raise_system_controller_interrupt() {
        raise_interrupt(system_controller_level);
    }
    static constexpr unsigned system_controller_level = 127;
    // PI_RT_COUNT's offset in the Hub's register window.
    static constexpr std::uint64_t rt_count_offset = 0x03'0100;
    // Host threads, without the device lock (SMP.adoc "S4"): PI_RT_COUNT at `time`, the
    // reader's own time, consistent with a write made meanwhile under the lock.
    [[nodiscard]] std::uint64_t rt_count_at(VirtualTime time) const;
    // PI_RT_COUNT at device time: what a read returns, which the machine may take directly.
    [[nodiscard]] std::uint64_t rt_count() const;
    // PI_RT_COUNT at `time`, and the first time after it when the count changes (the next
    // RTC tick, or the first after the epoch of the last write). With the base and epoch
    // (rt_origin) unchanged, a read at any time in between returns the same count.
    struct RtWindow {
        std::uint64_t count;
        VirtualTime until;
    };
    [[nodiscard]] RtWindow rt_window(VirtualTime time) const;
    // Advances at every change of the origin (a write of PI_RT_COUNT, a snapshot load).
    [[nodiscard]] const std::atomic<std::uint64_t>& rt_sequence() const {
        return rt_sequence_;
    }
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> rt_origin() const {
        return {rt_base_.load(std::memory_order_relaxed),
                rt_epoch_ticks_.load(std::memory_order_relaxed)};
    }
    // PI_INT_PEND0 and PI_INT_PEND1's offsets, and on host threads (SMP.adoc "S4") their
    // values without the device lock: IRIX polls PI_INT_PEND0 (S4.0: 52 M reads per CPU in
    // `boot4`). A CPU that sees a level another CPU raised also sees the memory that CPU wrote
    // before raising it.
    static constexpr std::uint64_t int_pend0_offset = 0x00'0098;
    static constexpr std::uint64_t int_pend1_offset = 0x00'00a0;
    [[nodiscard]] std::uint64_t interrupt_pending(std::size_t word) const {
        return pending_[word].load(std::memory_order_acquire);
    }
    // Host-parallel CPUs (SMP.adoc "S4"): the time of the CPU accessing the Hub, or none.
    // PI_RT_COUNT reads then follow the reader's own time, not device time, which can be up to
    // a window ahead of it: the PROM checks that the count advances across a loop of about
    // 1,000 cycles on each CPU (IP27.adoc "Two CPUs on a Hub").
    void set_reader_time(std::optional<VirtualTime> time) {
        reader_time_ = time;
    }

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

    // The node ID in NI_STATUS_REV_ID (bits 16:8), which places the node in the system's
    // physical address space (IP27.adoc "Node address space").
    [[nodiscard]] unsigned node_id() const {
        return static_cast<unsigned>(nsri_settings_ >> 8) & 0x1ff;
    }
    // Prefix for this Hub's trace lines, such as "n2 " (none with one node).
    void set_trace_name(std::string name) {
        trace_name_ = std::move(name);
    }
    // Called after the node ID changes, so the machine can route requests by it.
    void connect_node_id(std::function<void()> changed) {
        node_id_changed_ = std::move(changed);
    }

    // The NUMAlink cable to `neighbor`'s NI, with no router between (SMP.adoc "S5"): the link
    // reports up, and vector PIO reaches the neighbor's NI registers. Connect both ends.
    void connect_link(Hub& neighbor) {
        neighbor_ = &neighbor;
    }
    // The node board's MicroLAN (MD_MLAN_CTL), with its NICs (IP27.adoc "NICs").
    void connect_mlan(devices::OneWireBus& bus) {
        mlan_bus_ = &bus;
    }

    // The register window as CPU `slice` (0 for A, 1 for B) sees it.
    [[nodiscard]] MmioTarget& port(unsigned slice) {
        return ports_.at(slice);
    }

  private:
    class Port final : public MmioTarget {
      public:
        std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                            AccessWidth width) override;
        std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) override;
        Hub* hub{};
        unsigned slice{};
    };
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

    std::expected<std::uint64_t, AccessFault> read(unsigned slice, std::uint64_t offset,
                                                   AccessWidth width);
    std::expected<void, AccessFault> write(std::uint64_t offset, AccessWidth width,
                                           std::uint64_t value);
    void set_rt_count(std::uint64_t value);
    void set_led(unsigned slice, std::uint64_t value);
    void set_memory_config(std::uint64_t value);
    // Block-transfer engine `engine` register access; offset is relative to its base.
    [[nodiscard]] std::uint64_t read_bte(unsigned engine, std::uint64_t offset) const;
    void write_bte(unsigned engine, std::uint64_t offset, std::uint64_t value);
    void run_bte(unsigned engine);
    void raise_interrupt(unsigned level);
    void local_reset();
    void set_nsri_settings(std::uint64_t value);
    void update_interrupts();
    // Vector PIO (IP27.adoc "Vector PIO"): sends the request NI_VECTOR_PARMS describes, and
    // serves one that arrives from the neighbor, returning the read value or no value for an
    // address error.
    void start_vector_pio();
    std::optional<std::uint64_t> serve_vector_pio(unsigned type, std::uint64_t address,
                                                  std::uint64_t data);

    Scheduler& scheduler_;
    Tracer& tracer_;
    unsigned revision_;
    unsigned slot_id_;
    devices::Pcf8584& i2c_;
    DirectoryMemory& directory_;
    AddressSpace& memory_;
    std::function<void()> reset_node_;
    std::function<void()> node_id_changed_;
    std::string trace_name_;
    xtalk::Link* xtalk_{};
    std::array<std::uint64_t, 2> cpu_present_{};
    std::array<Port, 2> ports_{};
    std::array<IoWindow, xtalk::widget_count> io_windows_{};
    std::array<BigWindow, 7> big_windows_{};
    Widget widget_{};
    EventId local_reset_event_;
    // Storage registers (hub.cpp stored_registers).
    std::array<std::uint64_t, stored_register_count> stored_{};
    std::array<std::uint64_t, 2> cpu_enable_{};
    std::array<std::uint64_t, 2> err_stack_addr_{};
    // PI_ERR_STATUS0_A/B.
    std::array<std::uint64_t, 2> err_status0_{};
    std::array<std::uint64_t, 2> leds_{};
    std::uint64_t ilcsr_{};
    std::uint64_t memory_config_{};
    std::uint64_t refresh_control_{};
    std::uint64_t mlan_ctl_{};
    // The line sampled by the last MicroLAN operation; released (1) with nothing attached.
    bool mlan_line_{true};
    devices::OneWireBus* mlan_bus_{};
    Hub* neighbor_{};
    // NI_META_TABLE(0-31) then NI_LOCAL_TABLE(0-15): routing table exit ports.
    std::array<std::uint64_t, 48> routing_tables_{};
    // NI_VECTOR_STATUS, NI_RETURN_VECTOR, NI_VECTOR_READ_DATA: the last vector PIO response.
    std::uint64_t vector_status_{};
    std::uint64_t return_vector_{};
    std::uint64_t vector_read_data_{};
    // IBLS, IBSA, IBDA, IBCT, IBNA, IBIA for BTE 0 and 1.
    std::array<std::array<std::uint64_t, 6>, 2> bte_{};
    std::uint64_t nsri_settings_{};
    // PI_INT_PEND0/1 and PI_INT_MASK0/1 per CPU slice. The pending words are written under
    // the device lock and, on host threads, read without it (interrupt_pending).
    std::array<std::atomic<std::uint64_t>, 2> pending_{};
    void set_pending(std::size_t word, std::uint64_t value) {
        pending_[word].store(value, std::memory_order_release);
    }
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
    // Atomics with a sequence count (odd while a write is in progress), so rt_count_at needs
    // no lock.
    std::atomic<std::uint64_t> rt_base_{};
    std::atomic<std::uint64_t> rt_epoch_ticks_{};
    std::atomic<std::uint64_t> rt_sequence_{};
    std::optional<VirtualTime> reader_time_;
};

} // namespace ultraviolent::ip27
