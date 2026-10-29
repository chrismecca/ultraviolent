#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/ioc3.hpp>
#include <ultraviolent/pci/pci.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

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

// The serial rings' memory as PCI addresses it: byte a at address a.
class Rings final : public pci::DmaPort {
  public:
    bool dma_read(std::uint64_t address, std::span<std::byte> bytes) override {
        std::copy_n(memory.begin() + static_cast<std::ptrdiff_t>(address), bytes.size(),
                    bytes.begin());
        return true;
    }
    bool dma_write(std::uint64_t address, std::span<const std::byte> bytes) override {
        std::ranges::copy(bytes, memory.begin() + static_cast<std::ptrdiff_t>(address));
        return true;
    }
    std::vector<std::byte> memory = std::vector<std::byte>(0x4000);
};

// The RX packer (Linux ioc3_serial.c: bytes reach memory four at a time or when the RX
// timer, started by the first byte after SRCIR_ARM, runs out; RX_DRAIN flushes them).
const test::Registration serial_rx_packer{
    "ioc3.serial_rx_packer", [](test::Context& t) {
        Bench b;
        Rings rings;
        b.ioc3.connect_dma(rings);
        const auto write = [&](std::uint32_t offset, std::uint32_t value) {
            return b.ioc3.write(pci::Space::memory, 0x40'0000 + offset, 4, value);
        };
        const auto read = [&](std::uint32_t offset) {
            return b.ioc3.read(pci::Space::memory, 0x40'0000 + offset, 4).value_or(0xdead);
        };
        const auto byte = [&](std::size_t address) {
            return std::to_integer<char>(rings.memory.at(address));
        };
        // 1 KB rings at 0x1000 (port A's RX ring at 0x1400); port A's SSCR at 0xb8, SRPIR
        // 0xc4, SRCIR 0xc8, SRTR 0xcc; SIO_IR at 0x1c.
        t.check(write(0xb4, 0x1000));
        t.check(write(0xcc, 0xfff));         // SRTR: 4095 ticks of 16 kHz, 256 ms
        t.check(write(0xc8, 0x8000'0000));   // SRCIR: arm
        t.check(write(0xb8, 0x1000'0001));   // SSCR: DMA_EN, RX threshold 1 entry
        // UART A's FCR (register 2, PCI byte 0x20178 + (2 ^ 3) = 0x20179): FIFOs on.
        t.check(b.ioc3.write(pci::Space::memory, 0x42'0179, 1, 0x01));
        for (const char c : std::string{"hello"}) {
            b.ioc3.uart_a().receive(static_cast<std::uint8_t>(c));
        }
        // One byte per character time (about 1 ms): four make an entry, the fifth waits.
        b.scheduler.advance_by(VirtualDuration{6'000'000});
        // One entry, one byte in the packer (SRPIR_BYTE_CNT).
        t.check_equal(read(0xc4), std::uint32_t{0x0100'0008});
        t.check(byte(0x1403) == 'h' && byte(0x1402) == 'e' && byte(0x1401) == 'l' &&
                    byte(0x1400) == 'l',
                "byte k at entry byte 3 - k");
        t.check_equal(std::to_integer<unsigned>(rings.memory.at(0x1404)), 0xc0u);
        t.check((read(0x1c) & 0x4) != 0, "RX_HIGH at the threshold");
        t.check((read(0x1c) & 0x8) == 0, "the timer is still running");
        // The timer started at the first byte: the fifth goes out alone.
        b.scheduler.advance_by(VirtualDuration{260'000'000});
        t.check_equal(read(0xc4), std::uint32_t{0x10});
        t.check(byte(0x140b) == 'o');
        t.check_equal(std::to_integer<unsigned>(rings.memory.at(0x140f)), 0xc0u);
        t.check_equal(std::to_integer<unsigned>(rings.memory.at(0x140e)), 0u); // one byte valid
        t.check((read(0x1c) & 0x8) != 0, "RX_TIMER");
        // Not re-armed: a byte waits until RX_DRAIN.
        b.ioc3.uart_a().receive('x');
        b.scheduler.advance_by(VirtualDuration{300'000'000});
        t.check_equal(read(0xc4), std::uint32_t{0x0100'0010});
        t.check(write(0xb8, 0x1800'0001)); // SSCR with RX_DRAIN
        t.check_equal(read(0xc4), std::uint32_t{0x18});
        t.check(byte(0x1413) == 'x');
        t.check_equal(read(0xb8), std::uint32_t{0x1000'0001}); // RX_DRAIN reads back clear
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
