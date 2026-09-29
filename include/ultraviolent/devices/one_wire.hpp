#pragma once

#include <ultraviolent/core/state_image.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace ultraviolent::devices {

// A device on a Dallas 1-Wire bus (Dallas/Maxim "Book of iButton Standards", application note
// 937). Each time slot has two phases: every device offers its output (true releases the
// line, false pulls it low), then every device sees the resolved line.
class OneWireDevice {
  public:
    virtual ~OneWireDevice() = default;
    // A reset pulse. Returns whether the device answers with a presence pulse.
    virtual bool one_wire_reset() = 0;
    [[nodiscard]] virtual bool one_wire_output() const = 0;
    virtual void one_wire_slot(bool line) = 0;

  protected:
    OneWireDevice() = default;
    OneWireDevice(const OneWireDevice&) = default;
    OneWireDevice& operator=(const OneWireDevice&) = default;
};

// The wire: a wired-AND of the master and every device.
class OneWireBus {
  public:
    void attach(OneWireDevice& device) {
        devices_.push_back(&device);
    }
    // Returns whether any device answered with a presence pulse.
    bool reset();
    // One time slot in which the master leaves the line released (`master` true: a write-1 or
    // read slot) or holds it low (a write-0 slot). Returns the line the master samples.
    bool slot(bool master);

    // A MicroLAN master's operation (IRIX-derived Linux asm-ia64/sn/nic.h MCR_PACK: a low pulse
    // of `pulse` and a sample after `sample`, both in microseconds; Linux ioc3-eth.c uses
    // 500/65 for reset, 6/13 to read, 6/110 to write 1, 80/30 to write 0, 0/n to wait).
    // Returns the sampled line.
    bool pulse(unsigned pulse, unsigned sample);

  private:
    std::vector<OneWireDevice*> devices_;
};

// Dallas CRC-8 (x^8 + x^5 + x^4 + 1), as 1-Wire ROM IDs and memory reads use it.
std::uint8_t dallas_crc8(const std::uint8_t* bytes, std::size_t count, std::uint8_t crc = 0);

// Dallas CRC-16 (x^16 + x^15 + x^2 + 1, bit-reflected), as 1-Wire data records use it (Dallas
// application note 27). A record that ends in its CRC's complement, low byte first, leaves
// the residue 0xb001 when the CRC runs over the whole record.
std::uint16_t dallas_crc16(const std::uint8_t* bytes, std::size_t count, std::uint16_t crc = 0);

// A 1-Kbit add-only memory in the DS2502 family, the part SGI uses as a board's NIC ("Number
// In a Can"; Linux ioc3-eth.c reads family code 0x91 as a "DS1981U"): a 64-bit ROM ID, 128
// data bytes in four 32-byte pages, and 8 status bytes (page redirection). ROM functions: Read
// ROM (0x33), Match ROM (0x55), Skip ROM (0xcc), Search ROM (0xf0). Memory functions: Read
// Memory (0xf0), Read Data/Generate CRC (0xc3), Read Status (0xaa). Programming is not
// modeled.
class Nic final : public OneWireDevice {
  public:
    static constexpr std::size_t memory_size = 128;
    static constexpr std::size_t page_size = 32;
    static constexpr std::size_t status_size = 8;

    // `family` and the 48-bit `serial` form the ROM ID; its CRC byte is computed.
    Nic(std::uint8_t family, std::uint64_t serial);

    [[nodiscard]] std::array<std::uint8_t, memory_size>& memory() {
        return memory_;
    }
    [[nodiscard]] std::array<std::uint8_t, status_size>& status() {
        return status_;
    }

    bool one_wire_reset() override;
    [[nodiscard]] bool one_wire_output() const override;
    void one_wire_slot(bool line) override;

  private:
    enum class State : std::uint8_t {
        idle,        // not addressed until the next reset
        rom_command, // receiving the ROM function byte
        match_rom,   // receiving the ROM ID to match
        search_bit,  // Search ROM: sending a ROM bit
        search_complement,
        search_choice, // Search ROM: receiving the master's choice
        send,          // transmitting queued bytes
        memory_command,
        address,
    };

    void begin_receive(State state, unsigned bits);
    void queue(std::uint8_t byte);
    void on_byte(std::uint8_t byte);
    void queue_memory_read();

    std::array<std::uint8_t, 8> rom_{};
    std::array<std::uint8_t, memory_size> memory_{};
    std::array<std::uint8_t, status_size> status_{};
    State state_{State::idle};
    // Byte assembly for receiving states.
    std::uint8_t receive_byte_{};
    unsigned receive_bits_{};
    unsigned receive_count_{};
    // Match ROM progress.
    unsigned match_index_{};
    bool matching_{};
    // Search ROM: the current ROM bit index.
    unsigned search_index_{};
    // Memory function and its target address.
    std::uint8_t memory_function_{};
    std::uint16_t address_{};
    std::array<std::uint8_t, 3> header_{};
    // Transmission queue: the bytes still to send and the bit index in the current one.
    std::vector<std::uint8_t> send_queue_;
    std::size_t send_position_{};
    unsigned send_bit_{};
    bool select_after_send_{};
};

} // namespace ultraviolent::devices
