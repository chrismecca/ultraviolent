#pragma once

#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/devices/i2c.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ultraviolent::devices {

// A 24C16-class serial EEPROM: 2 KB in eight 256-byte blocks, answering I2C addresses 0x50-0x57
// (1010 A10 A9 A8), with an 8-bit word address, 16-byte page writes, and sequential reads that
// wrap at the end of the memory (for example the Atmel AT24C16 datasheet).
//
// hypothesis: the internal write cycle completes before the next transfer (the device is never
// busy).
class I2cEeprom final : public I2cTarget {
  public:
    static constexpr std::size_t size = 2048;
    static constexpr std::uint8_t base_address = 0x50;

    I2cEeprom();

    [[nodiscard]] std::span<std::byte, size> bytes() {
        return contents_;
    }
    [[nodiscard]] std::span<const std::byte, size> bytes() const {
        return contents_;
    }

    bool i2c_start(std::uint8_t address, bool read) override;
    bool i2c_write(std::uint8_t byte) override;
    std::uint8_t i2c_read() override;
    void i2c_stop() override;

    void save_state(StateImage& image, const std::string& key) const;
    void load_state(const StateImage& image, const std::string& key);

  private:
    std::array<std::byte, size> contents_{};
    std::uint16_t pointer_{};
    std::uint16_t block_{};
    bool selected_{};
    bool reading_{};
    // A write transfer's first byte is the word address.
    bool have_word_address_{};
};

} // namespace ultraviolent::devices
