#include "support/test.hpp"

#include <ultraviolent/devices/i2c.hpp>
#include <ultraviolent/devices/i2c_eeprom.hpp>

#include <cstdint>

namespace {

using namespace ultraviolent;
using devices::I2cBus;
using devices::I2cEeprom;

struct Bench {
    Bench() {
        bus.attach(eeprom);
    }
    // Random read: word address write, repeated START with the read bit.
    std::uint8_t read_at(std::uint16_t address) {
        (void)bus.start(static_cast<std::uint8_t>((0x50 | (address >> 8)) << 1));
        (void)bus.write(static_cast<std::uint8_t>(address));
        (void)bus.start(static_cast<std::uint8_t>((0x50 | (address >> 8)) << 1 | 1));
        const std::uint8_t value = bus.read();
        bus.stop();
        return value;
    }
    I2cEeprom eeprom;
    I2cBus bus;
};

const test::Registration random_read{
    "i2c_eeprom.random_and_sequential_read", [](test::Context& t) {
        Bench b;
        t.check_equal(b.read_at(0x123), std::uint8_t{0xff}); // erased
        b.eeprom.bytes()[0x708] = std::byte{1};
        b.eeprom.bytes()[0x709] = std::byte{2};
        // The block (A10:A8) is in the device address (0x57 for 0x7xx).
        t.check_equal(b.read_at(0x708), std::uint8_t{1});
        // A current-address read continues after the last byte read.
        t.check(b.bus.start(0x57 << 1 | 1));
        t.check_equal(b.bus.read(), std::uint8_t{2});
        b.bus.stop();
        t.check(!b.bus.start(0x60 << 1), "other addresses do not answer");
    }};

const test::Registration page_write{
    "i2c_eeprom.page_write_wraps", [](test::Context& t) {
        Bench b;
        t.check(b.bus.start(0x51 << 1));
        t.check(b.bus.write(0x0e)); // word address 0x10e
        t.check(b.bus.write(0xaa));
        t.check(b.bus.write(0xbb));
        t.check(b.bus.write(0xcc)); // wraps to the start of the 16-byte page
        b.bus.stop();
        t.check_equal(b.read_at(0x10e), std::uint8_t{0xaa});
        t.check_equal(b.read_at(0x10f), std::uint8_t{0xbb});
        t.check_equal(b.read_at(0x100), std::uint8_t{0xcc});
        t.check_equal(b.read_at(0x110), std::uint8_t{0xff});
    }};

} // namespace
