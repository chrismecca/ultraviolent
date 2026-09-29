#include <ultraviolent/devices/i2c.hpp>

namespace ultraviolent::devices {

bool I2cBus::start(std::uint8_t address_byte) {
    const std::uint8_t address = address_byte >> 1;
    const bool read = (address_byte & 1) != 0;
    // A repeated START ends the previous target's part in the transfer.
    selected_ = nullptr;
    busy_ = true;
    for (I2cTarget* target : targets_) {
        if (target->i2c_start(address, read)) {
            selected_ = target;
            return true;
        }
    }
    return false;
}

bool I2cBus::write(std::uint8_t byte) {
    return selected_ != nullptr && selected_->i2c_write(byte);
}

std::uint8_t I2cBus::read() {
    // Nobody drives SDA: the pulled-up line reads as ones.
    return selected_ != nullptr ? selected_->i2c_read() : std::uint8_t{0xff};
}

void I2cBus::stop() {
    for (I2cTarget* target : targets_) {
        target->i2c_stop();
    }
    selected_ = nullptr;
    busy_ = false;
}

void I2cBus::broadcast(std::uint8_t byte, const I2cMonitor* sender) {
    for (I2cMonitor* monitor : monitors_) {
        if (monitor != sender) {
            monitor->i2c_observed_byte(byte);
        }
    }
}

void I2cBus::release(const I2cMonitor* sender) {
    for (I2cTarget* target : targets_) {
        target->i2c_stop();
    }
    selected_ = nullptr;
    busy_ = false;
    for (I2cMonitor* monitor : monitors_) {
        if (monitor != sender) {
            monitor->i2c_observed_stop();
        }
    }
}

} // namespace ultraviolent::devices
