#pragma once

#include <ultraviolent/devices/uart16550.hpp>

#include <cstdio>

namespace ultraviolent::backends {

// Writes a serial port's transmitted bytes to a stdio stream the caller owns, unchanged.
class StreamSerialSink final : public devices::SerialSink {
  public:
    explicit StreamSerialSink(std::FILE* stream) : stream_{stream} {}

    void serial_transmit(std::uint8_t byte) override;

  private:
    std::FILE* stream_;
};

} // namespace ultraviolent::backends
