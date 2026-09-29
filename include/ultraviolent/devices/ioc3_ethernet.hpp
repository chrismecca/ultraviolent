#pragma once

#include <ultraviolent/core/ethernet_link.hpp>
#include <ultraviolent/core/interrupt.hpp>
#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/devices/dp83840.hpp>
#include <ultraviolent/pci/pci.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace ultraviolent::devices {

// The IOC3's Ethernet MAC (IP27.adoc "IOC3 Ethernet"): the registers at 0xf0-0x153 (ioc3.h
// struct ioc3_ethregs), the SSRAM diagnostic window at 0x40000-0x7ffff, MII management of the
// PHY, and the DMA engines: a transmit ring of 128-byte descriptors and a receive ring of
// buffer addresses. Sources: the IRIX-derived Linux ioc3.h and ioc3-eth.c (v5.10), and the
// observed behavior of the BASEIO PROM's diagnostics and IRIX's ef driver. It is a part of
// devices::Ioc3, which routes these offsets to it.
class Ioc3Ethernet {
  public:
    static constexpr std::uint32_t register_first = 0xf0;
    static constexpr std::uint32_t register_end = 0x154;
    static constexpr std::uint32_t ssram_first = 0x4'0000;
    static constexpr std::uint32_t ssram_end = 0x8'0000;
    // hypothesis: how often the receiver looks for a frame from the link while enabled.
    static constexpr VirtualDuration receive_poll{100'000};
    // Frames that arrived while the guest had no receive buffer; more are dropped.
    static constexpr std::size_t receive_backlog = 64;

    Ioc3Ethernet(Scheduler& scheduler, Tracer& tracer);

    // The PHY on the MII management bus at `address`.
    void connect_phy(unsigned address, Dp83840& phy) {
        phy_address_ = address & 31;
        phy_ = &phy;
    }
    // The IOC3's INTA, the Ethernet interrupt.
    void connect_interrupt(InterruptSink& sink, std::uint32_t input) {
        interrupt_.connect(sink, input);
    }
    // The IOC3's DMA port.
    void connect_dma(pci::DmaPort& port) {
        dma_ = &port;
    }
    // The cable. Frames go to and come from `link` while the PHY reports a link.
    void connect_link(EthernetLink& link) {
        link_ = &link;
    }

    // 32-bit accesses at an IOC3 offset in one of the ranges above; others are refused.
    [[nodiscard]] std::optional<std::uint32_t> read(std::uint32_t offset);
    bool write(std::uint32_t offset, std::uint32_t value);

    // PCI reset: every register.
    void reset();

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    [[nodiscard]] std::uint32_t& reg(std::uint32_t offset) {
        return registers_[(offset - register_first) / 4];
    }
    // EMCR_RST: the engines, their pointers, and the interrupt state.
    void reset_engines();
    void mii_command(std::uint32_t micr);
    void update_interrupt();
    void raise(std::uint32_t eisr_bits);

    // Guest memory in memory byte order, through the Bridge's lane-preserving window (memory
    // byte a is at PCI lane a ^ 3).
    bool read_memory(std::uint64_t address, std::span<std::byte> out);
    bool write_memory(std::uint64_t address, std::span<const std::byte> in);

    void start_transmit();
    void transmit_descriptor();
    void poll_receive();
    // Delivers `frame` to the guest's next receive buffer; false when there is none. A frame
    // from the wire is padded and gets its FCS; a looped-back one arrives as sent.
    bool deliver(std::span<const std::byte> frame, bool from_wire);
    [[nodiscard]] bool accepts(std::span<const std::byte> frame) const;
    [[nodiscard]] bool transmit_enabled() const;
    [[nodiscard]] bool receive_enabled() const;
    [[nodiscard]] bool loopback() const;

    Scheduler& scheduler_;
    Tracer& tracer_;
    EventId transmit_event_;
    EventId receive_event_;
    InterruptLine interrupt_;
    pci::DmaPort* dma_{};
    EthernetLink* link_{};
    std::array<std::uint32_t, (register_end - register_first) / 4> registers_{};
    // 64K words of 16 data bits and a parity bit.
    std::vector<std::uint32_t> ssram_ = std::vector<std::uint32_t>((ssram_end - ssram_first) / 4);
    Dp83840* phy_{};
    unsigned phy_address_{};
    // Frames waiting for a receive buffer: from the link, and looped back.
    std::deque<std::vector<std::byte>> backlog_;
    std::deque<std::vector<std::byte>> looped_;
    // ERPIR_ARM: the receive interrupt is armed; a frame delivered while disarmed is announced
    // when software arms it again.
    bool receive_armed_{};
    bool receive_unannounced_{};
};

// The IEEE 802.3 frame check sequence of `frame` (CRC-32, reflected, as transmitted: the four
// bytes to append, least significant first).
std::array<std::byte, 4> ethernet_fcs(std::span<const std::byte> frame);

} // namespace ultraviolent::devices
