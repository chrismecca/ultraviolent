#pragma once

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/devices/i2c.hpp>

#include <cstdint>

namespace ultraviolent::devices {

// The SGI entry-level system controller (ELSC) as a node's I2C bus sees it: the arbiter that
// hands the shared bus to node CPUs. Only arbitration is modeled; the ELSC's command
// interface is not (see IP27.adoc "System controller").
//
// Arbitration (observed in the IP27 PROM's i2c_arb, whose error "Timeout waiting for sysctlr
// arb" is I2C_ERROR_TO_ARB in the IRIX-derived Linux ksys/i2c.h): with the bus idle, a node
// CPU waits until the bus's last byte is its token and stays so for 250 microseconds, then
// takes the bus. The token for slot code s (1-4 for node slots n1-n4) and CPU slice c is
// 0xde + 2s + c, or that plus 8 when the controller has a message for the CPU. A node ends
// its transaction by addressing 0x7f instead of sending STOP; the controller takes the bus
// back and frees it.
//
// hypothesis: the controller offers the eight node-CPU tokens in turn, each for
// `token_period`, whenever the bus is idle; it never has messages; it frees the bus one byte
// time after being addressed at 0x7f; command addresses (0x20, 0x08) do not answer.
class Elsc final : public I2cTarget {
  public:
    static constexpr std::uint8_t release_address = 0x7f;
    static constexpr VirtualDuration token_period{1'000'000};
    static constexpr VirtualDuration release_delay{100'000};

    Elsc(Scheduler& scheduler, Tracer& tracer, I2cBus& bus);

    void reset();

    bool i2c_start(std::uint8_t address, bool read) override;
    bool i2c_write(std::uint8_t byte) override;
    std::uint8_t i2c_read() override;
    void i2c_stop() override;

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    void next_token();
    void release_bus();

    Scheduler& scheduler_;
    Tracer& tracer_;
    I2cBus& bus_;
    EventId token_event_;
    EventId release_event_;
    unsigned token_index_{};
};

} // namespace ultraviolent::devices
