#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/ioc3.hpp>
#include <ultraviolent/pci/pci.hpp>

#include <cstdint>
#include <string>

namespace {

using namespace ultraviolent;

const test::Registration bars{
    "pci.bar_sizing_and_decode", [](test::Context& t) {
        pci::ConfigHeader header{0x10a9,
                                 0x0003,
                                 0x0280,
                                 0x0000'0001,
                                 {pci::ConfigHeader::Bar{pci::Space::memory, 0x10'0000}},
                                 1};
        t.check_equal(header.read(0x00), std::uint32_t{0x0003'10a9});
        t.check_equal(header.read(0x04), std::uint32_t{0x0280'0000}); // status, command 0
        // Writing all ones reveals the size.
        header.write(0x10, 0xffff'ffff, 0xf);
        t.check_equal(header.read(0x10), std::uint32_t{0xfff0'0000});
        header.write(0x10, 0x0040'0000, 0xf);
        t.check(!header.decode(pci::Space::memory, 0x40'0010), "memory decode disabled");
        header.write(0x04, 0x6, 0x3);
        const auto hit = header.decode(pci::Space::memory, 0x40'0010);
        t.check(hit && hit->offset == 0x10);
        t.check(!header.decode(pci::Space::memory, 0x50'0000), "outside the BAR");
        t.check_equal(header.read(0x3c), std::uint32_t{0x100}); // interrupt pin A
    }};

class Terminal final : public devices::SerialSink {
  public:
    void serial_transmit(std::uint8_t byte) override {
        output.push_back(static_cast<char>(byte));
    }
    std::string output;
};

struct Bench {
    Bench() {
        ioc3.config_write(0x10, 0x0040'0000, 0xf);
        ioc3.config_write(0x04, 0x6, 0x3);
    }
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    devices::Ioc3 ioc3{scheduler, tracer};
};

const test::Registration registers{
    "ioc3.little_endian_registers", [](test::Context& t) {
        Bench b;
        // An ordinary little-endian PCI device: byte lane 0 is the least significant byte.
        t.check(b.ioc3.write(pci::Space::memory, 0x40'0034, 4, 0xe000'3000));
        t.check_equal(b.ioc3.read(pci::Space::memory, 0x40'0034, 4).value_or(0),
                      std::uint32_t{0xe000'3000});
        t.check_equal(b.ioc3.read(pci::Space::memory, 0x40'0034, 1).value_or(0),
                      std::uint32_t{0x00});
        t.check_equal(b.ioc3.read(pci::Space::memory, 0x40'0037, 1).value_or(0),
                      std::uint32_t{0xe0});
    }};

const test::Registration sio_cr{
    "ioc3.sio_cr_arbiter_idle", [](test::Context& t) {
        Bench b;
        // Written bits read back; the arbiter status bits read idle with no request.
        t.check(b.ioc3.write(pci::Space::memory, 0x40'0028, 4, 0x0078'0102));
        t.check_equal(b.ioc3.read(pci::Space::memory, 0x40'0028, 4).value_or(0),
                      std::uint32_t{0x0040'0102});
    }};

const test::Registration uart{"ioc3.uart_a_bytes", [](test::Context& t) {
                                  Bench b;
                                  Terminal terminal;
                                  b.ioc3.uart_a().connect(terminal);
                                  // ioc3.h struct ioc3_uartregs: register n at PCI byte 0x20178 +
                                  // (n ^ 3). LCR (3) is first: 8N1, then a byte to THR (0) at the
                                  // dword's last byte.
                                  t.check(b.ioc3.write(pci::Space::memory, 0x42'0178, 1, 0x03));
                                  t.check_equal(
                                      b.ioc3.read(pci::Space::memory, 0x42'0178, 1).value_or(0),
                                      std::uint32_t{0x03});
                                  t.check(b.ioc3.write(pci::Space::memory, 0x42'017b, 1, 'P'));
                                  t.check_equal(terminal.output, std::string{"P"});
                              }};

const test::Registration microlan{
    "ioc3.mcr_nic", [](test::Context& t) {
        Bench b;
        devices::OneWireBus bus;
        devices::Nic nic{0x91, 7};
        bus.attach(nic);
        b.ioc3.connect_nic(bus);
        // MCR (0x30): a reset pulse (MCR_PACK(520, 65) = 0x82104), then DONE with the
        // presence pulse sampled (data 0).
        t.check(b.ioc3.write(pci::Space::memory, 0x40'0030, 4, 0x0008'2104));
        const std::uint32_t value = b.ioc3.read(pci::Space::memory, 0x40'0030, 4).value_or(0);
        t.check_equal(value & 3, std::uint32_t{2});
    }};

} // namespace
