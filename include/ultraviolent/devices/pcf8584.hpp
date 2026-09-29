#pragma once

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/devices/i2c.hpp>

#include <cstdint>

namespace ultraviolent::devices {

// Philips/NXP PCF8584 I2C-bus controller, as seen through its two-register parallel port.
//
// Register semantics follow the Philips datasheet and the public Linux driver definitions
// (drivers/i2c/algos/i2c-algo-pcf.h): S1 (A0 = 1) is the control register on write and the
// status register on read; S0 (A0 = 0) is the data shift register when ESO is set, and
// otherwise the own-address, clock, or interrupt-vector register selected by ES1/ES2.
//
// As master it runs transfers on an I2cBus: START and repeated START with the address in S0,
// data bytes written to S0, and master reception, where each S0 read returns the received
// byte and starts the next (the first read after the address is a dummy read). When it is not
// the master its shift register latches every byte other masters send, so S0 reads the last
// byte on the bus. Slave operation (being addressed) is not modeled.
class Pcf8584 final : public I2cMonitor {
  public:
    Pcf8584(Scheduler& scheduler, Tracer& tracer, I2cBus& bus);

    void reset();

    // The INT output: a completed byte (PIN clear) with ENI set.
    [[nodiscard]] bool interrupt_asserted() const;

    // `a0` selects S1 (true) or S0 (false).
    std::uint8_t read(bool a0);
    void write(bool a0, std::uint8_t value);

    void i2c_observed_byte(std::uint8_t byte) override;
    void i2c_observed_stop() override;

    // Snapshot support (StateImage). Pending transfers are the scheduler's.
    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    enum class Transfer : std::uint8_t { none, address, transmit, receive };

    void begin(Transfer transfer);
    void finish_transfer();
    void end_mastership();
    [[nodiscard]] VirtualDuration byte_time() const;

    Scheduler& scheduler_;
    Tracer& tracer_;
    I2cBus& bus_;
    EventId transfer_done_;
    std::uint8_t control_{};
    std::uint8_t status_{};
    std::uint8_t own_address_{};
    std::uint8_t clock_{};
    std::uint8_t vector_{};
    std::uint8_t data_{};
    bool master_{};
    // Master receiver after an address byte with the read bit.
    bool receiving_{};
    // A repeated START was requested in S1; the next S0 write sends it with the address.
    bool start_pending_{};
    // Whether the last received byte was acknowledged; a NAK ends reception.
    bool last_acknowledged_{true};
    Transfer transfer_{Transfer::none};
};

} // namespace ultraviolent::devices
