#pragma once

#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/interrupt.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/devices/am29f080.hpp>
#include <ultraviolent/devices/one_wire.hpp>
#include <ultraviolent/pci/pci.hpp>
#include <ultraviolent/xtalk/xtalk.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace ultraviolent::devices {

// The SGI Bridge crosstalk-to-PCI widget (IP27.adoc "Bridge"), register layout from the
// IRIX-derived Linux arch/mips/include/asm/pci/bridge.h. Registers are 32 bits, in the low half
// of a doubleword (offset 4 mod 8).
//
// Modeled: identity, reset values, register storage, invalid-address errors, interrupt
// status/enable/reset, error interrupts, PCI configuration space, device windows, the internal
// ATE RAM, and DMA through the direct map, the internal ATEs, and 64-bit addresses. Not
// modeled: external ATE SSRAM (none fitted), PCI interrupts, the external flash.
class Bridge final : public xtalk::Widget, public InterruptSink {
  public:
    // BRIDGE_WIDGET_PART_NUM, BRIDGE_WIDGET_MFGR_NUM. hypothesis: revision BRIDGE_REV_D (4).
    static constexpr std::uint32_t id = xtalk::make_widget_id(4, 0xc002, 0x036);
    // Registers occupy 0x000-0x2ff (BRIDGE_WID_* through BRIDGE_RESP_CLEAR).
    static constexpr std::uint64_t register_space = 0x300;

    // `port` is the Crossbow port the Bridge is cabled to; its control register's widget-ID
    // field reads it after reset.
    Bridge(Tracer& tracer, unsigned port);

    // Where error interrupts go.
    void connect(xtalk::Fabric& fabric) {
        fabric_ = &fabric;
    }
    // A PCI device in slot `slot` (0-7), reached through configuration space and the device
    // windows.
    void attach(unsigned slot, pci::Device& device) {
        slots_[slot] = &device;
    }
    // The bus-master port of the device in `slot`: DMA through the direct map.
    [[nodiscard]] pci::DmaPort& dma_port(unsigned slot) {
        return dma_ports_[slot];
    }
    // The board's NIC bus, driven through BRIDGE_NIC.
    // The board's flash PROM on the Bridge's external flash port (bridge.h
    // BRIDGE_EXTERNAL_FLASH: flash PROM 0 at 0xc00000; 1 at 0xe00000 is not fitted).
    void connect_flash(Am29f080& flash) {
        flash_ = &flash;
    }
    void connect_nic(OneWireBus& bus) {
        nic_bus_ = &bus;
    }

    void reset();

    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth width) override;
    std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                std::uint64_t value) override;
    void xtalk_interrupt(std::uint64_t address, std::uint8_t vector) override;
    // A link reset resets the Bridge, which asserts PCI RST# to its devices.
    void xtalk_reset() override {
        reset();
    }
    // The PCI interrupt pins INT0-7 (input = pin); the board wires each slot's INTA to the pin
    // of the same number.
    void set_interrupt_level(std::uint32_t pin, bool asserted) override;

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    class SlotDma final : public pci::DmaPort {
      public:
        bool dma_read(std::uint64_t address, std::span<std::byte> bytes) override {
            return bridge->dma(slot, address, bytes, {});
        }
        bool dma_write(std::uint64_t address, std::span<const std::byte> bytes) override {
            return bridge->dma(slot, address, {}, bytes);
        }
        Bridge* bridge{};
        unsigned slot{};
    };

    // A DMA transfer by the device in `slot`: into `read`, or from `write`.
    bool dma(unsigned slot, std::uint64_t address, std::span<std::byte> read,
             std::span<const std::byte> write);
    // Configuration space (BRIDGE_TYPE0_CFG_DEV0) and device windows (BRIDGE_DEVIO0-7).
    std::expected<std::uint64_t, AccessFault> config_access(std::uint64_t offset, unsigned size,
                                                            const std::uint64_t* write);
    std::expected<std::uint64_t, AccessFault> window_access(std::uint64_t offset, unsigned size,
                                                            const std::uint64_t* write);
    [[nodiscard]] std::uint32_t read_register(std::uint64_t offset);
    void write_register(std::uint64_t offset, std::uint32_t value);
    // Sets error bits in INT_STATUS and sends the error interrupt if one is enabled.
    void raise_error(std::uint32_t bits, std::uint64_t offset);
    void update_error_interrupt();

    Tracer& tracer_;
    unsigned port_;
    xtalk::Fabric* fabric_{};
    OneWireBus* nic_bus_{};
    Am29f080* flash_{};
    std::array<pci::Device*, 8> slots_{};
    std::array<SlotDma, 8> dma_ports_{};
    std::array<std::uint32_t, register_space / 8> registers_{};
    // Internal address translation entries (b_int_ate_ram).
    std::array<std::uint64_t, 128> ates_{};
    std::uint32_t int_status_{};
    // Interrupt pin levels, INT_STATUS bits 7:0.
    std::uint32_t pins_{};
    void send_pin_interrupt(unsigned pin, bool raise);
    bool error_interrupt_sent_{};
};

} // namespace ultraviolent::devices
