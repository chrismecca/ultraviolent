#pragma once

#include <ultraviolent/core/state_image.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace ultraviolent::devices {

// Where a serial port's transmitted bytes go; a host backend implements it (ARCHITECTURE:
// guest-visible devices and their host backends are separate).
class SerialSink {
  public:
    virtual ~SerialSink() = default;
    virtual void serial_transmit(std::uint8_t byte) = 0;

  protected:
    SerialSink() = default;
    SerialSink(const SerialSink&) = default;
    SerialSink& operator=(const SerialSink&) = default;
};

// Where a serial port's received bytes come from; a host backend implements it. The UART asks
// for one byte when software looks for input (reads LSR or RBR) and its receiver is empty, so
// input arrives at a rate the guest sets, independent of host timing.
class SerialSource {
  public:
    virtual ~SerialSource() = default;
    virtual std::optional<std::uint8_t> serial_receive() = 0;

  protected:
    SerialSource() = default;
    SerialSource(const SerialSource&) = default;
    SerialSource& operator=(const SerialSource&) = default;
};

// A 16550-compatible UART (National Semiconductor PC16550D datasheet), as the IOC3's SuperIO
// serial ports present it: eight byte registers selected by `reg` 0-7, with DLAB (LCR bit 7)
// selecting the divisor latch at 0 and 1.
//
// hypothesis: transmission and reception take no time (the transmitter is always empty);
// modem control inputs are inactive except in loopback, where they follow the outputs.
// Interrupts are not modeled yet.
class Uart16550 {
  public:
    Uart16550() = default;

    void connect(SerialSink& sink) {
        sink_ = &sink;
    }
    void connect(SerialSource& source) {
        source_ = &source;
    }
    // A byte arriving on the receive line.
    void receive(std::uint8_t byte);

    // The IOC3's serial DMA engine drives the transmitter and takes received bytes directly,
    // bypassing the registers: a byte to send (looped back in loopback mode), and the next
    // received byte, from the receive FIFO or else the input source.
    void transmit(std::uint8_t byte);
    std::optional<std::uint8_t> take_input();

    std::uint8_t read(unsigned reg);
    void write(unsigned reg, std::uint8_t value);

    void reset();

    void save_state(StateImage& image, const std::string& key) const;
    void load_state(const StateImage& image, const std::string& key);

  private:
    void poll_source();

    SerialSink* sink_{};
    SerialSource* source_{};
    std::deque<std::uint8_t> rx_;
    std::uint8_t ier_{};
    std::uint8_t fcr_{};
    std::uint8_t lcr_{};
    std::uint8_t mcr_{};
    std::uint8_t scr_{};
    std::uint8_t dll_{};
    std::uint8_t dlm_{};
    bool overrun_{};
};

} // namespace ultraviolent::devices
