#pragma once

#include <ultraviolent/core/interrupt.hpp>
#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/devices/ioc3_ethernet.hpp>
#include <ultraviolent/devices/m48t35.hpp>
#include <ultraviolent/devices/one_wire.hpp>
#include <ultraviolent/devices/uart16550.hpp>
#include <ultraviolent/pci/pci.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace ultraviolent::devices {

// The SGI IOC3 I/O controller on the BaseIO (IP27.adoc "IOC3"): serial, parallel, keyboard,
// Ethernet, and a NIC interface behind one memory BAR. Register layout: the IRIX-derived
// Linux asm/sn/ioc3.h (struct ioc3). First slice: PCI identity and traced register storage.
class Ioc3 final : public pci::Device {
  public:
    // hypothesis: the serial DMA engine moves one byte per character time at 9600 baud (ten
    // bits); the console's rate.
    static constexpr VirtualDuration character_time{1'041'667};

    Ioc3(Scheduler& scheduler, Tracer& tracer);

    // The Bridge slot port its DMA goes through, and the PCI INTB wire that carries its SIO
    // (serial, keyboard, parallel) interrupts (Linux ioc3.c ip27_baseio_setup). INTA, the
    // Ethernet interrupt, is the Ethernet block's (ethernet().connect_interrupt).
    void connect_dma(pci::DmaPort& port) {
        dma_ = &port;
        ethernet_.connect_dma(port);
    }
    void connect_interrupt(InterruptSink& sink, std::uint32_t input) {
        interrupt_.connect(sink, input);
    }
    // The time-of-day chip on bytebus device 0 (Linux ioc3.c ip27_baseio_setup: "m48t35").
    void connect_timekeeper(M48t35& timekeeper) {
        timekeeper_ = &timekeeper;
    }

    // The SuperIO serial ports (ioc3.h uarta at 0x20178, uartb at 0x20170).
    [[nodiscard]] Uart16550& uart_a() {
        return uart_a_;
    }
    [[nodiscard]] Uart16550& uart_b() {
        return uart_b_;
    }

    // The NIC bus behind MCR (the Ethernet address NIC).
    void connect_nic(OneWireBus& bus) {
        nic_bus_ = &bus;
    }

    // General-purpose I/O pin `pin` (0-15) drives `output` with its level when software sets
    // it through GPPR or GPDR (ioc3.h; on the BaseIO pin 5 is the PHY's reset, GPPR_PHY_RESET_PIN).
    void connect_gpio(unsigned pin, std::function<void(bool high)> output) {
        gpio_outputs_.at(pin) = std::move(output);
    }

    // The Ethernet MAC (registers 0xf0-0x153 and the SSRAM window).
    [[nodiscard]] Ioc3Ethernet& ethernet() {
        return ethernet_;
    }

    std::uint32_t config_read(std::uint8_t reg) override;
    void config_write(std::uint8_t reg, std::uint32_t value, unsigned byte_enable) override;
    std::optional<std::uint32_t> read(pci::Space space, std::uint64_t address,
                                      unsigned size) override;
    bool write(pci::Space space, std::uint64_t address, unsigned size,
               std::uint32_t value) override;

    void pci_reset() override;

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    // One serial port's DMA engine registers (ioc3.h struct ioc3_serialregs).
    struct SerialDma {
        std::uint32_t sscr{};
        std::uint32_t stpir{};
        std::uint32_t stcir{};
        std::uint32_t srpir{};
        std::uint32_t srcir{};
        std::uint32_t srtr{};
        std::uint32_t shadow{};
    };
    [[nodiscard]] std::optional<std::uint32_t> read_serial(std::uint32_t offset);
    bool write_serial(std::uint32_t offset, std::uint32_t value);
    void start_serial(unsigned port);
    void transmit_entry(unsigned port);
    void receive_entry(unsigned port);
    [[nodiscard]] std::uint64_t ring_address(unsigned ring) const;
    [[nodiscard]] std::uint32_t ring_mask() const;
    // SIO_IR with the TX-empty levels.
    [[nodiscard]] std::uint32_t sio_ir() const;
    void update_interrupt();
    Uart16550& uart(unsigned port) {
        return port == 0 ? uart_a_ : uart_b_;
    }

    Scheduler& scheduler_;
    Tracer& tracer_;
    pci::ConfigHeader config_;
    pci::DmaPort* dma_{};
    M48t35* timekeeper_{};
    InterruptLine interrupt_;
    std::array<SerialDma, 2> serial_{};
    std::array<EventId, 2> tx_events_;
    std::array<EventId, 2> rx_events_;
    std::uint32_t sbbr_h_{};
    std::uint32_t sbbr_l_{};
    std::uint32_t sio_ir_{};
    std::uint32_t sio_enable_{};
    std::map<std::uint32_t, std::uint32_t> registers_;
    OneWireBus* nic_bus_{};
    Uart16550 uart_a_;
    Uart16550 uart_b_;
    Ioc3Ethernet ethernet_;
    void set_gpio(std::uint32_t levels);
    std::array<std::function<void(bool)>, 16> gpio_outputs_;
    std::uint32_t gpio_levels_{0xffff}; // pins float high until driven
};

} // namespace ultraviolent::devices
