#pragma once

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/devices/i2c.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ultraviolent::devices {

// The SGI entry-level system controller (ELSC) as a node's I2C bus sees it: the arbiter that
// hands the shared bus to node CPUs, and a command interface on which it answers the few
// commands the power switch needs (see IP27.adoc "System controller").
//
// Arbitration (observed in the IP27 PROM's i2c_arb, whose error "Timeout waiting for sysctlr
// arb" is I2C_ERROR_TO_ARB in the IRIX-derived Linux ksys/i2c.h): with the bus idle, a node
// CPU waits until the bus's last byte is its token and stays so for 250 microseconds, then
// takes the bus. The token for slot code s (1-4 for node slots n1-n4) and CPU slice c is
// 0xde + 2s + c, or that plus 8 when the controller has a message for the CPU. A node ends
// its transaction by addressing 0x7f instead of sending STOP; the controller takes the bus
// back and frees it.
//
// Commands (observed in IRIX 6.5.30's elsc_command, elsc_parse, and elsc_process): a node
// writes a packet to address 0x08, then reads the answer with a repeated START. Both packets
// are a length byte, that many bytes of text, and a checksum byte that makes the packet's
// bytes sum to zero. Answers are "ok" or "err <reason>"; the answer to "get" is the waiting
// message, such as "pd off" for the module's keyswitch turned off.
//
// hypothesis: the controller offers the eight node-CPU tokens in turn, each for
// `token_period`, whenever the bus is idle, all with the message flag while a message waits;
// it frees the bus one byte time after being addressed at 0x7f; of the commands it answers
// only "get" and "pwr d N", and to any other it does not acknowledge the read address, so the
// node sees the same I2C_ERROR_NAK as before commands were modeled. Address 0x20 (IRIX's
// elsc_ust_read and elsc_ust_write) does not answer.
class Elsc final : public I2cTarget {
  public:
    static constexpr std::uint8_t command_address = 0x08;
    static constexpr std::uint8_t release_address = 0x7f;
    static constexpr VirtualDuration token_period{1'000'000};
    static constexpr VirtualDuration release_delay{100'000};
    // hypothesis: power stays on this long after the keyswitch turns off unless the system
    // asks for more ("pwr d N"); IRIX first asks about two seconds after the message.
    static constexpr VirtualDuration keyswitch_grace{10'000'000'000};
    // The command and answer buffers (ELSC_PACKET_MAX in the Linux 2.4.3 ksys/elsc.h).
    static constexpr std::size_t packet_max = 96;

    Elsc(Scheduler& scheduler, Tracer& tracer, I2cBus& bus);

    void reset();

    // The module's keyswitch turned to OFF: the controller tells the system ("pd off") and
    // removes power after `keyswitch_grace` or when the system says.
    void turn_keyswitch_off();
    // Called when the controller has a message waiting (the node's MSC interrupt).
    void connect_message_interrupt(std::function<void()> raise) {
        message_interrupt_ = std::move(raise);
    }
    // Called when the controller removes power from the module.
    void connect_power_off(std::function<void()> off) {
        power_off_ = std::move(off);
    }
    [[nodiscard]] bool powered_off() const {
        return powered_off_;
    }

    bool i2c_start(std::uint8_t address, bool read) override;
    bool i2c_write(std::uint8_t byte) override;
    std::uint8_t i2c_read() override;
    void i2c_stop() override;

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    void next_token();
    void release_bus();
    void remove_power();
    // The answer to the command packet received, or none for a command not modeled.
    std::optional<std::string> answer();
    void queue_message(std::string_view message);

    Scheduler& scheduler_;
    Tracer& tracer_;
    I2cBus& bus_;
    EventId token_event_;
    EventId release_event_;
    EventId power_event_;
    std::function<void()> message_interrupt_;
    std::function<void()> power_off_;
    unsigned token_index_{};
    // The command packet being written, and the answer packet being read.
    std::array<std::uint8_t, packet_max + 2> command_{};
    std::uint8_t command_length_{};
    bool receiving_command_{};
    std::array<std::uint8_t, packet_max + 2> answer_{};
    std::uint8_t answer_length_{};
    std::uint8_t answer_position_{};
    // One waiting message; the power switch is the only source.
    std::array<char, 16> message_{};
    std::uint8_t message_length_{};
    bool message_waiting_{};
    bool powered_off_{};
};

} // namespace ultraviolent::devices
