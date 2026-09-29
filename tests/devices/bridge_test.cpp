#include "support/test.hpp"

#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/bridge.hpp>
#include <ultraviolent/devices/one_wire.hpp>
#include <ultraviolent/pci/pci.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

namespace {

using namespace ultraviolent;
using devices::Bridge;

// Records interrupts; widget 9 is 256 bytes of memory for DMA.
class Fabric final : public xtalk::Fabric {
  public:
    void send_interrupt(unsigned target, std::uint64_t address, std::uint8_t vector) override {
        sent.emplace_back(target, address, vector);
    }
    void send_interrupt_clear(unsigned target, std::uint64_t address,
                              std::uint8_t vector) override {
        cleared.emplace_back(target, address, vector);
    }
    std::vector<std::tuple<unsigned, std::uint64_t, std::uint8_t>> cleared;
    bool dma_read(unsigned target, std::uint64_t address, std::span<std::byte> bytes) override {
        if (target != 9 || address + bytes.size() > memory.size()) {
            return false;
        }
        std::copy_n(memory.begin() + static_cast<std::ptrdiff_t>(address), bytes.size(),
                    bytes.begin());
        return true;
    }
    bool dma_write(unsigned target, std::uint64_t address,
                   std::span<const std::byte> bytes) override {
        if (target != 9 || address + bytes.size() > memory.size()) {
            return false;
        }
        std::ranges::copy(bytes, memory.begin() + static_cast<std::ptrdiff_t>(address));
        return true;
    }
    std::vector<std::tuple<unsigned, std::uint64_t, std::uint8_t>> sent;
    std::vector<std::byte> memory = std::vector<std::byte>(0x100);
};

// A PCI device that records accesses and answers memory reads with its address.
class Recorder final : public pci::Device {
  public:
    std::uint32_t config_read(std::uint8_t reg) override {
        return reg == 0 ? 0x1234'5678 : reg;
    }
    void config_write(std::uint8_t reg, std::uint32_t value, unsigned byte_enable) override {
        config_writes.emplace_back(reg, value, byte_enable);
    }
    std::optional<std::uint32_t> read(pci::Space /*space*/, std::uint64_t address,
                                      unsigned /*size*/) override {
        last_address = address;
        return 0x1122'3344;
    }
    bool write(pci::Space /*space*/, std::uint64_t address, unsigned /*size*/,
               std::uint32_t value) override {
        last_address = address;
        last_value = value;
        return true;
    }
    void pci_reset() override {
        ++resets;
    }
    std::vector<std::tuple<std::uint8_t, std::uint32_t, unsigned>> config_writes;
    int resets = 0;
    std::uint64_t last_address = 0;
    std::uint32_t last_value = 0;
};

struct Bench {
    Bench() {
        bridge.connect(fabric);
        bridge.attach(2, device);
    }
    std::uint64_t read(std::uint64_t offset, AccessWidth width = AccessWidth::bits32) {
        return bridge.mmio_read(offset, width).value_or(0xdead);
    }
    bool write(std::uint64_t offset, std::uint64_t value, AccessWidth width = AccessWidth::bits32) {
        return bridge.mmio_write(offset, width, value).has_value();
    }
    VirtualClock clock;
    Tracer tracer{clock};
    Bridge bridge{tracer, 0xf};
    Fabric fabric;
    Recorder device;
};

const test::Registration reset_values{
    "bridge.reset_values", [](test::Context& t) {
        Bench b;
        t.check_equal(b.read(0x04), std::uint64_t{0x4c00'206d}); // rev 4, 0xc002, 0x036, LSB 1
        t.check_equal(b.read(0x0c) & 0x20, std::uint64_t{0x20}); // PCI mode
        // bridge_sanity's expectations: control with the widget ID, timeout, device registers.
        t.check_equal(b.read(0x24), std::uint64_t{0x7f00'33ff});
        t.check_equal(b.read(0x2c), std::uint64_t{0xf'ffff});
        t.check_equal(b.read(0x204), std::uint64_t{0x1800'1000});
        t.check_equal(b.read(0x20c), std::uint64_t{0x1800'1002});
        t.check_equal(b.read(0x23c), std::uint64_t{0x1800'1009});
    }};

const test::Registration errors{
    "bridge.invalid_address_interrupt", [](test::Context& t) {
        Bench b;
        b.write(0x34, 0x0009'8000); // INTDEST: target 9, address 47:32 0x8000
        b.write(0x3c, 0x0180'0090);
        b.write(0x12c, 0x05);       // INT_HOST_ERR vector
        b.write(0x10c, 1u << 24);   // enable INVLD_ADDR
        b.write(0x7c, 0xdead'beef); // undefined register
        t.check_equal(b.read(0x104), std::uint64_t{1u << 24});
        t.check(b.fabric.sent == std::vector<std::tuple<unsigned, std::uint64_t, std::uint8_t>>{
                                     {9, 0x8000'0180'0090, 5}});
        b.write(0x114, 1u << 3); // INT_RST_STAT: REQ_DSP group clears INVLD_ADDR
        t.check_equal(b.read(0x104), std::uint64_t{0});
    }};

const test::Registration config_space{
    "bridge.pci_configuration_space", [](test::Context& t) {
        Bench b;
        // Slot 2's dword 0; slot 3 is empty and faults.
        t.check_equal(b.read(0x2'2000), std::uint64_t{0x1234'5678});
        t.check(!b.bridge.mmio_read(0x2'3000, AccessWidth::bits32), "empty slot");
        // Byte lanes swap: CPU byte 0x22003 is PCI byte 0 (Linux ops-bridge.c).
        t.check_equal(b.read(0x2'2003, AccessWidth::bits8), std::uint64_t{0x78});
        b.write(0x2'2006, 0x6, AccessWidth::bits16); // PCI 0x04, the command register
        t.check(!b.device.config_writes.empty() &&
                std::get<0>(b.device.config_writes.back()) == 0x04 &&
                std::get<2>(b.device.config_writes.back()) == 0x3);
    }};

const test::Registration windows{"bridge.device_windows", [](test::Context& t) {
                                     Bench b;
                                     // Device 2's window (0x600000) at PCI 4 MB by its reset
                                     // DEVICE2 register. Without DEV_SWAP byte lanes are preserved:
                                     // values pass unchanged and a narrower access at byte A
                                     // reaches PCI byte A ^ (4 - size).
                                     t.check_equal(b.read(0x60'0010), std::uint64_t{0x1122'3344});
                                     t.check_equal(b.device.last_address, std::uint64_t{0x40'0010});
                                     b.write(0x60'0103, 0x5a, AccessWidth::bits8);
                                     t.check_equal(b.device.last_address, std::uint64_t{0x40'0100});
                                     b.write(0x60'010e, 0x1234, AccessWidth::bits16);
                                     t.check_equal(b.device.last_address, std::uint64_t{0x40'010c});
                                     t.check_equal(b.device.last_value, std::uint32_t{0x1234});
                                     // With DEV_SWAP bytes keep their addresses and values arrive
                                     // byte-reversed.
                                     b.write(0x214, 0x1800'3004);
                                     t.check_equal(b.read(0x60'0010), std::uint64_t{0x4433'2211});
                                     b.write(0x60'0103, 0x5a, AccessWidth::bits8);
                                     t.check_equal(b.device.last_address, std::uint64_t{0x40'0103});
                                     b.write(0x60'010e, 0x1234, AccessWidth::bits16);
                                     t.check_equal(b.device.last_value, std::uint32_t{0x3412});
                                 }};

const test::Registration dma{
    "bridge.direct_map_dma", [](test::Context& t) {
        Bench b;
        for (std::size_t i = 0; i < b.fabric.memory.size(); ++i) {
            b.fabric.memory[i] = static_cast<std::byte>(i);
        }
        b.write(0x84, 0x90'0000); // DIR_MAP: widget 9, offset 0
        pci::DmaPort& port = b.bridge.dma_port(2);
        std::array<std::byte, 6> bytes{};
        // Without SWAP_DIR byte lanes are preserved: PCI byte X is crosstalk byte X ^ 3.
        t.check(port.dma_read(0x8000'0012, bytes));
        t.check(bytes == std::array<std::byte, 6>{std::byte{0x11}, std::byte{0x10}, std::byte{0x17},
                                                  std::byte{0x16}, std::byte{0x15},
                                                  std::byte{0x14}});
        const std::array<std::byte, 2> word{std::byte{0xaa}, std::byte{0xbb}};
        t.check(port.dma_write(0x8000'0020, word));
        t.check(b.fabric.memory[0x23] == std::byte{0xaa} &&
                b.fabric.memory[0x22] == std::byte{0xbb} &&
                b.fabric.memory[0x21] == std::byte{0x21});
        // With SWAP_DIR (DEVICE2 bit 19) bytes keep their addresses.
        b.write(0x214, 0x1808'1004);
        t.check(port.dma_read(0x8000'0012, bytes));
        t.check(bytes[0] == std::byte{0x12} && bytes[5] == std::byte{0x17});
        // Below the direct map (the ATE range) is not modeled.
        t.check(!port.dma_read(0x4000'0000, bytes));
        // A 64-bit address names the widget in bits 63:60; the barrier attribute is ignored.
        t.check(port.dma_read(0x9100'0000'0000'0012, bytes));
        t.check(bytes[0] == std::byte{0x12});
        t.check(!port.dma_read(0x8100'0000'0000'0012, bytes), "widget 8 has nothing");
    }};

const test::Registration ates{
    "bridge.ate_ram_and_mapped_dma", [](test::Context& t) {
        Bench b;
        for (std::size_t i = 0; i < b.fabric.memory.size(); ++i) {
            b.fabric.memory[i] = static_cast<std::byte>(i);
        }
        // ATE 1: crosstalk page 0 on widget 9, valid. Word reads return its halves.
        const std::uint64_t ate = 0x0000'0000'0000'0901;
        t.check(b.bridge.mmio_write(0x1'0008, AccessWidth::bits64, ate).has_value());
        t.check_equal(b.read(0x1'000c), std::uint64_t{0});
        t.check_equal(b.read(0x1'100c), std::uint64_t{0x901});
        // PCI 0x40001000 is page 1 (4 KB pages): crosstalk 0x0 + 0x20, lanes preserved.
        std::array<std::byte, 4> bytes{};
        t.check(b.bridge.dma_port(2).dma_read(0x4000'1020, bytes));
        t.check(bytes[0] == std::byte{0x23} && bytes[3] == std::byte{0x20});
        // Page 0's ATE is not valid.
        t.check(!b.bridge.dma_port(2).dma_read(0x4000'0020, bytes));
        // No external SSRAM: a probe write is lost and reads back 0, without an error.
        t.check(b.bridge.mmio_write(0x8'fff8, AccessWidth::bits64, 0x1234).has_value());
        t.check_equal(b.bridge.mmio_read(0x8'fff8, AccessWidth::bits64).value_or(1),
                      std::uint64_t{0});
    }};

const test::Registration pins{
    "bridge.pci_interrupt_pins", [](test::Context& t) {
        Bench b;
        b.write(0x34, 0x0009'0000); // INTDEST: widget 9
        b.write(0x3c, 0x0180'0090);
        b.write(0x13c, 0x42); // INT_ADDR(1): vector 0x42
        b.bridge.set_interrupt_level(1, true);
        t.check_equal(b.read(0x104) & 0xff, std::uint64_t{0x2}); // INT_STATUS pin 1
        t.check(b.fabric.sent.empty(), "not enabled");
        b.write(0x10c, 0x2); // enable pin 1 while it is asserted: sent at once
        using Sent = std::vector<std::tuple<unsigned, std::uint64_t, std::uint8_t>>;
        t.check(b.fabric.sent == Sent{{9, 0x0180'0090, 0x42}});
        b.bridge.set_interrupt_level(1, false);
        t.check(b.fabric.cleared.empty(), "no clear packet without INT_MODE");
        b.write(0x11c, 0x2); // INT_MODE pin 1
        b.bridge.set_interrupt_level(1, true);
        b.bridge.set_interrupt_level(1, false);
        t.check_equal(b.fabric.sent.size(), std::size_t{2});
        t.check(b.fabric.cleared == Sent{{9, 0x0180'0090, 0x42}});
        t.check_equal(b.read(0x104) & 0xff, std::uint64_t{0});
    }};

const test::Registration link_reset{"bridge.link_reset_resets_pci", [](test::Context& t) {
                                        Bench b;
                                        b.write(0x214, 0x1808'1004);
                                        b.bridge.xtalk_reset();
                                        t.check_equal(b.device.resets, 1);
                                        t.check_equal(b.read(0x214), std::uint64_t{0x1800'1004});
                                    }};

const test::Registration nic{"bridge.nic_microlan", [](test::Context& t) {
                                 Bench b;
                                 devices::OneWireBus bus;
                                 b.bridge.connect_nic(bus);
                                 // A reset pulse with nothing on the bus: DONE, line high.
                                 b.write(0xb4, 0x8'2104);
                                 t.check_equal(b.read(0xb4) & 3, std::uint64_t{3});
                                 devices::Nic part{0x91, 1};
                                 bus.attach(part);
                                 b.write(0xb4, 0x8'2104);
                                 t.check_equal(b.read(0xb4) & 3,
                                               std::uint64_t{2}); // presence pulls the line low
                             }};

} // namespace
