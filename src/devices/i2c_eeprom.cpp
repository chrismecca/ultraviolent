#include <ultraviolent/devices/i2c_eeprom.hpp>

#include <algorithm>
#include <string>

namespace ultraviolent::devices {

namespace {

constexpr std::uint16_t page_size = 16;

} // namespace

I2cEeprom::I2cEeprom() {
    // An erased EEPROM reads as ones.
    std::ranges::fill(contents_, std::byte{0xff});
}

bool I2cEeprom::i2c_start(std::uint8_t address, bool read) {
    selected_ = (address & 0x78) == base_address;
    if (!selected_) {
        return false;
    }
    block_ = static_cast<std::uint16_t>((address & 7) << 8);
    reading_ = read;
    have_word_address_ = false;
    return true;
}

bool I2cEeprom::i2c_write(std::uint8_t byte) {
    if (!selected_ || reading_) {
        return false;
    }
    if (!have_word_address_) {
        pointer_ = static_cast<std::uint16_t>(block_ | byte);
        have_word_address_ = true;
        return true;
    }
    contents_[pointer_] = static_cast<std::byte>(byte);
    // Page writes wrap within the 16-byte page.
    pointer_ = static_cast<std::uint16_t>((pointer_ & ~(page_size - 1)) |
                                          ((pointer_ + 1) & (page_size - 1)));
    return true;
}

std::uint8_t I2cEeprom::i2c_read() {
    const auto value = std::to_integer<std::uint8_t>(contents_[pointer_]);
    pointer_ = static_cast<std::uint16_t>((pointer_ + 1) % size);
    return value;
}

void I2cEeprom::i2c_stop() {
    selected_ = false;
}

void I2cEeprom::save_state(StateImage& image, const std::string& key) const {
    image.put_bytes(key + ".contents", contents_);
    image.put(key + ".pointer", pointer_);
}

void I2cEeprom::load_state(const StateImage& image, const std::string& key) {
    if (const auto contents = image.get_bytes(key + ".contents");
        contents && contents->size() == size) {
        std::ranges::copy(*contents, contents_.begin());
    }
    image.get(key + ".pointer", pointer_);
}

} // namespace ultraviolent::devices
