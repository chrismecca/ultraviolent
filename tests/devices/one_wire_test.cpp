#include "support/test.hpp"

#include <ultraviolent/devices/one_wire.hpp>

#include <cstdint>
#include <vector>

namespace {

using namespace ultraviolent;
using devices::Nic;
using devices::OneWireBus;

// The master side as Linux ioc3-eth.c drives a MicroLAN register: (pulse, sample) pairs.
struct Master {
    OneWireBus& bus;
    bool reset() {
        const bool line = bus.pulse(500, 65);
        (void)bus.pulse(0, 500);
        return !line; // presence pulls the line low
    }
    bool read_bit() {
        const bool bit = bus.pulse(6, 13);
        (void)bus.pulse(0, 100);
        return bit;
    }
    void write_bit(bool bit) {
        (void)(bit ? bus.pulse(6, 110) : bus.pulse(80, 30));
    }
    std::uint8_t read_byte() {
        unsigned result = 0;
        for (int i = 0; i < 8; ++i) {
            result = (result >> 1) | (read_bit() ? 0x80u : 0u);
        }
        return static_cast<std::uint8_t>(result);
    }
    void write_byte(unsigned byte) {
        for (int i = 0; i < 8; ++i) {
            write_bit((byte & 1) != 0);
            byte >>= 1;
        }
    }
    // Search ROM ("Book of iButton Standards" algorithm, as in ioc3-eth.c nic_find).
    std::uint64_t find(int& last) {
        std::uint64_t address = 0;
        int disc = 0;
        (void)reset();
        write_byte(0xf0);
        for (int index = 0; index < 64; ++index) {
            const bool a = read_bit();
            const bool b = read_bit();
            if (a && b) {
                last = 0;
                return 0;
            }
            if (!a && !b) {
                if (index == last) {
                    address |= std::uint64_t{1} << index;
                } else if (index > last) {
                    address &= ~(std::uint64_t{1} << index);
                    disc = index;
                } else if ((address & (std::uint64_t{1} << index)) == 0) {
                    disc = index;
                }
                write_bit((address & (std::uint64_t{1} << index)) != 0);
            } else {
                if (a) {
                    address |= std::uint64_t{1} << index;
                } else {
                    address &= ~(std::uint64_t{1} << index);
                }
                write_bit(a);
            }
        }
        last = disc;
        return address;
    }
};

std::uint64_t rom_value(std::uint8_t family, std::uint64_t serial) {
    std::uint8_t bytes[8] = {family};
    for (int i = 0; i < 6; ++i) {
        bytes[1 + i] = static_cast<std::uint8_t>(serial >> (8 * i));
    }
    bytes[7] = devices::dallas_crc8(bytes, 7);
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

const test::Registration crc{"one_wire.dallas_crc8", [](test::Context& t) {
                                 // Application note 27's example ROM: CRC 0xa2.
                                 const std::uint8_t rom[7] = {0x02, 0x1c, 0xb8, 0x01,
                                                              0x00, 0x00, 0x00};
                                 t.check_equal(devices::dallas_crc8(rom, 7), std::uint8_t{0xa2});
                             }};

const test::Registration crc16{
    "one_wire.dallas_crc16", [](test::Context& t) {
        // The CRC-16/ARC check value (same polynomial, reflected, initial value 0).
        const std::uint8_t text[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        t.check_equal(devices::dallas_crc16(text, 9), std::uint16_t{0xbb3d});
        // A record ending in the CRC's complement, low byte first, leaves the residue 0xb001.
        std::uint8_t record[11] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        const auto stored = static_cast<std::uint16_t>(~devices::dallas_crc16(record, 9));
        record[9] = static_cast<std::uint8_t>(stored);
        record[10] = static_cast<std::uint8_t>(stored >> 8);
        t.check_equal(devices::dallas_crc16(record, 11), std::uint16_t{0xb001});
    }};

const test::Registration presence{"one_wire.presence_and_read_rom", [](test::Context& t) {
                                      OneWireBus bus;
                                      Master m{bus};
                                      t.check(!m.reset(), "no device, no presence");
                                      Nic nic{0x91, 0x2};
                                      bus.attach(nic);
                                      t.check(m.reset());
                                      m.write_byte(0x33);
                                      std::uint64_t rom = 0;
                                      for (int i = 0; i < 8; ++i) {
                                          rom |= std::uint64_t{m.read_byte()} << (8 * i);
                                      }
                                      t.check_equal(rom, rom_value(0x91, 0x2));
                                  }};

const test::Registration search{"one_wire.search_finds_every_device", [](test::Context& t) {
                                    OneWireBus bus;
                                    Nic mio{0x91, 0x1};
                                    Nic baseio{0x91, 0x2};
                                    bus.attach(mio);
                                    bus.attach(baseio);
                                    Master m{bus};
                                    int last = 0;
                                    std::vector<std::uint64_t> found{m.find(last)};
                                    t.check(last != 0, "a discrepancy remains");
                                    found.push_back(m.find(last));
                                    t.check_equal(last, 0);
                                    const auto a = rom_value(0x91, 0x1);
                                    const auto b = rom_value(0x91, 0x2);
                                    t.check((found[0] == a && found[1] == b) ||
                                            (found[0] == b && found[1] == a));
                                }};

const test::Registration read_memory{
    "one_wire.match_rom_and_read_memory", [](test::Context& t) {
        OneWireBus bus;
        Nic other{0x91, 0x1};
        Nic nic{0x91, 0x2};
        nic.memory()[0x20] = 0x12;
        nic.memory()[0x21] = 0x34;
        bus.attach(other);
        bus.attach(nic);
        Master m{bus};
        t.check(m.reset());
        m.write_byte(0x55);
        const std::uint64_t rom = rom_value(0x91, 0x2);
        for (int i = 0; i < 8; ++i) {
            m.write_byte(static_cast<std::uint8_t>(rom >> (8 * i)));
        }
        m.write_byte(0xf0); // Read Memory from 0x20
        m.write_byte(0x20);
        m.write_byte(0x00);
        const std::uint8_t header[3] = {0xf0, 0x20, 0x00};
        t.check_equal(m.read_byte(), devices::dallas_crc8(header, 3));
        t.check_equal(m.read_byte(), std::uint8_t{0x12});
        t.check_equal(m.read_byte(), std::uint8_t{0x34});
        t.check_equal(m.read_byte(), std::uint8_t{0xff}); // unprogrammed
        // Read Status (Skip ROM with one device): 0xff means no page is redirected.
        OneWireBus single;
        Master s{single};
        single.attach(nic);
        t.check(s.reset());
        s.write_byte(0xcc);
        s.write_byte(0xaa);
        s.write_byte(0x01);
        s.write_byte(0x00);
        (void)s.read_byte(); // CRC of the command and address
        t.check_equal(s.read_byte(), std::uint8_t{0xff});
    }};

} // namespace
