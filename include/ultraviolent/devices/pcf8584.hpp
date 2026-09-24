#pragma once

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>

#include <cstdint>

namespace ultraviolent::devices {

// Philips/NXP PCF8584 I2C-bus controller, as seen through its two-register parallel port.
//
// Register semantics follow the public Linux driver definitions (drivers/i2c/algos/
// i2c-algo-pcf.h): S1 (A0 = 1) is the control register on write and the status register on
// read; S0 (A0 = 0) is the data shift register when ESO is set, and otherwise the own-address,
// clock, or interrupt-vector register selected by ES1/ES2.
//
// No I2C devices are attached yet: an addressed byte is never acknowledged (LRB reads 1).
// Add a narrow I2C device interface when the first device is modeled.
class Pcf8584 {
  public:
    Pcf8584(Scheduler& scheduler, Tracer& tracer);

    void reset();

    // The INT output: a completed byte (PIN clear) with ENI set.
    [[nodiscard]] bool interrupt_asserted() const;

    // `a0` selects S1 (true) or S0 (false).
    std::uint8_t read(bool a0);
    void write(bool a0, std::uint8_t value);

  private:
    void start_transfer();
    void finish_transfer();
    [[nodiscard]] VirtualDuration byte_time() const;

    Scheduler& scheduler_;
    Tracer& tracer_;
    EventId transfer_done_;
    std::uint8_t control_{};
    std::uint8_t status_{};
    std::uint8_t own_address_{};
    std::uint8_t clock_{};
    std::uint8_t vector_{};
    std::uint8_t data_{};
    bool master_{};
};

} // namespace ultraviolent::devices
