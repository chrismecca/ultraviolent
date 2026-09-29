#pragma once

#include <cstdint>
#include <vector>

namespace ultraviolent::devices {

// A device on an I2C bus (NXP UM10204, "I2C-bus specification and user manual"), addressed by
// a 7-bit address.
class I2cTarget {
  public:
    virtual ~I2cTarget() = default;

    // A START or repeated START addressed to `address` in direction `read`. Returns the
    // target's acknowledge; a target that does not acknowledge takes no part in the transfer.
    virtual bool i2c_start(std::uint8_t address, bool read) = 0;
    // A byte from the master. Returns the target's acknowledge.
    virtual bool i2c_write(std::uint8_t byte) = 0;
    // The next byte the target sends to the master.
    virtual std::uint8_t i2c_read() = 0;
    // A STOP condition ends the transfer.
    virtual void i2c_stop() = 0;

  protected:
    I2cTarget() = default;
    I2cTarget(const I2cTarget&) = default;
    I2cTarget& operator=(const I2cTarget&) = default;
};

// Something that watches the bus while it is not the master: it sees every byte other masters
// send and every STOP.
class I2cMonitor {
  public:
    virtual ~I2cMonitor() = default;
    virtual void i2c_observed_byte(std::uint8_t byte) = 0;
    virtual void i2c_observed_stop() = 0;

  protected:
    I2cMonitor() = default;
    I2cMonitor(const I2cMonitor&) = default;
    I2cMonitor& operator=(const I2cMonitor&) = default;
};

// The wires: targets answer a master's transfers; monitors see traffic from other masters.
// Transfers are immediate; the master models byte timing.
class I2cBus {
  public:
    void attach(I2cTarget& target) {
        targets_.push_back(&target);
    }
    void attach(I2cMonitor& monitor) {
        monitors_.push_back(&monitor);
    }

    // Master side. start() selects the first target that acknowledges the address byte
    // (7-bit address << 1 | read) and returns whether one did.
    bool start(std::uint8_t address_byte);
    bool write(std::uint8_t byte);
    std::uint8_t read();
    void stop();

    // Another master's traffic, as monitors see it: a byte, and the STOP that frees the bus.
    void broadcast(std::uint8_t byte, const I2cMonitor* sender = nullptr);
    void release(const I2cMonitor* sender = nullptr);

    // Whether a master holds the bus: from a START until the STOP that ends it.
    [[nodiscard]] bool busy() const {
        return busy_;
    }

  private:
    std::vector<I2cTarget*> targets_;
    std::vector<I2cMonitor*> monitors_;
    I2cTarget* selected_{};
    bool busy_{};
};

} // namespace ultraviolent::devices
