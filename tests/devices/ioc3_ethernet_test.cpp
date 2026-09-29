#include "support/test.hpp"

#include <ultraviolent/core/ethernet_link.hpp>
#include <ultraviolent/core/interrupt.hpp>
#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/dp83840.hpp>
#include <ultraviolent/devices/ioc3.hpp>
#include <ultraviolent/pci/pci.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

namespace {

using namespace ultraviolent;

constexpr std::uint64_t base = 0x40'0000; // the IOC3's BAR in these tests

// Guest memory behind the Bridge's lane-preserving window: memory byte a is PCI lane a ^ 3.
class Memory final : public pci::DmaPort {
  public:
    // PCI64 attribute and widget bits (63:48) are the Bridge's; memory is the rest.
    static constexpr std::uint64_t address_mask = 0x0000'ffff'ffff'ffff;
    bool dma_read(std::uint64_t address, std::span<std::byte> bytes) override {
        address &= address_mask;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = bytes_.at((address + i) ^ 3);
        }
        return true;
    }
    bool dma_write(std::uint64_t address, std::span<const std::byte> bytes) override {
        address &= address_mask;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes_.at((address + i) ^ 3) = bytes[i];
        }
        return true;
    }
    // Memory as the CPU sees it (big-endian words).
    void put32(std::uint64_t address, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) {
            bytes_.at(address + i) = static_cast<std::byte>(value >> (24 - 8 * i));
        }
    }
    void put64(std::uint64_t address, std::uint64_t value) {
        put32(address, static_cast<std::uint32_t>(value >> 32));
        put32(address + 4, static_cast<std::uint32_t>(value));
    }
    [[nodiscard]] std::uint32_t get32(std::uint64_t address) const {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value = (value << 8) | std::to_integer<std::uint32_t>(bytes_.at(address + i));
        }
        return value;
    }
    std::vector<std::byte> bytes_ = std::vector<std::byte>(0x4'0000);
};

// A cable that records what the guest sends and offers queued frames.
class Queue final : public EthernetLink {
  public:
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    std::optional<std::vector<std::byte>> receive() override {
        if (incoming.empty()) {
            return std::nullopt;
        }
        auto frame = std::move(incoming.front());
        incoming.pop_front();
        return frame;
    }
    std::vector<std::vector<std::byte>> sent;
    std::deque<std::vector<std::byte>> incoming;
};

std::vector<std::byte> frame_to(std::initializer_list<unsigned> destination, std::size_t size) {
    std::vector<std::byte> frame(size);
    std::size_t i = 0;
    for (const unsigned b : destination) {
        frame[i++] = static_cast<std::byte>(b);
    }
    for (; i < size; ++i) {
        frame[i] = static_cast<std::byte>(i);
    }
    return frame;
}

struct Bench {
    Bench() {
        ioc3.config_write(0x10, 0x0040'0000, 0xf);
        ioc3.config_write(0x04, 0x6, 0x3);
        ioc3.ethernet().connect_phy(31, phy);
        ioc3.connect_dma(memory);
        ioc3.ethernet().connect_link(cable);
    }
    // The station address 08:00:69:0e:17:01, as Linux __ioc3_set_mac_address writes it.
    void set_station() {
        write(0x138, 0x0117);     // EMAR_H: bytes 5, 4
        write(0x13c, 0x0e690008); // EMAR_L: bytes 3..0
    }
    void link_up() {
        phy.set_link_partner(true);
        scheduler.advance_by(devices::Dp83840::negotiation_time);
    }
    std::uint32_t read(std::uint32_t offset) {
        return ioc3.read(pci::Space::memory, base + offset, 4).value_or(0xdead'beef);
    }
    void write(std::uint32_t offset, std::uint32_t value) {
        ioc3.write(pci::Space::memory, base + offset, 4, value);
    }
    // MII management as Linux ioc3_mdio_read/write does it.
    std::uint32_t mdio_read(unsigned phy_address, unsigned reg) {
        write(0x148, (phy_address << 5) | reg | 0x400);
        return read(0x14c);
    }
    void mdio_write(unsigned phy_address, unsigned reg, std::uint16_t value) {
        write(0x150, value);
        write(0x148, (phy_address << 5) | reg);
    }
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    devices::Ioc3 ioc3{scheduler, tracer};
    devices::Dp83840 phy{scheduler, tracer, "phy"};
    Memory memory;
    Queue cable;
};

const test::Registration reset{"ioc3_ethernet.reset_and_arbiter_idle", [](test::Context& t) {
                                   Bench b;
                                   b.write(0x100, 0x1234'5000); // ERBR_H
                                   b.write(0x110, 0xff8);       // ERPIR
                                   b.write(0xf0, 0x8000'0000);  // EMCR_RST
                                   // IRIX waits for ARB_DIAG_IDLE after the reset.
                                   t.check_equal(b.read(0xf0), std::uint32_t{0x8020'0000});
                                   // The PROM sets the ring bases before the reset and the pointers
                                   // after it.
                                   t.check_equal(b.read(0x100), std::uint32_t{0x1234'5000});
                                   t.check_equal(b.read(0x110), std::uint32_t{0});
                                   b.write(0xf0, 0);
                                   t.check_equal(b.read(0xf0), std::uint32_t{0x0020'0000});
                               }};

const test::Registration ssram{"ioc3_ethernet.ssram_parity", [](test::Context& t) {
                                   Bench b;
                                   b.write(0xf0, 0x200); // EMCR_RAMPAR
                                   // Bit 17 is not stored (the PROM's address walk).
                                   b.write(0x4'0004, 0x2'0101);
                                   t.check_equal(b.read(0x4'0004), std::uint32_t{0x0'0101});
                                   // The PROM's parity test: odd weight over 17 bits reads with bit
                                   // 17 set.
                                   b.write(0x4'0000, 0x1'57aa);
                                   t.check_equal(b.read(0x4'0000), std::uint32_t{0x1'57aa});
                                   b.write(0x4'0000, 0xbad1);
                                   t.check_equal(b.read(0x4'0000), std::uint32_t{0x2'bad1});
                                   b.write(0x4'0000, 0x1'bad0);
                                   t.check_equal(b.read(0x4'0000), std::uint32_t{0x3'bad0});
                                   // Without RAMPAR no parity is reported. No aliasing across the
                                   // 128 KB.
                                   b.write(0xf0, 0);
                                   t.check_equal(b.read(0x4'0000), std::uint32_t{0x1'bad0});
                                   b.write(0x6'0000, 0xaaaa);
                                   t.check_equal(b.read(0x4'0000), std::uint32_t{0x1'bad0});
                               }};

const test::Registration mii{"ioc3_ethernet.mii_and_phy", [](test::Context& t) {
                                 Bench b;
                                 // The PROM's expectations: identifier and reset values.
                                 t.check_equal(b.mdio_read(31, 2), std::uint32_t{0x2000});
                                 t.check_equal(b.mdio_read(31, 3) & 0xfffe, std::uint32_t{0x5c00});
                                 t.check_equal(b.mdio_read(31, 0) & 0xff80, std::uint32_t{0x3100});
                                 t.check_equal(b.mdio_read(31, 1) & 0xf83f, std::uint32_t{0x7809});
                                 t.check_equal(b.mdio_read(31, 4) & 0xe3ff, std::uint32_t{0x01e1});
                                 t.check_equal(b.mdio_read(31, 23) & 0xb8d6, std::uint32_t{0x8040});
                                 t.check_equal(b.mdio_read(31, 25) & 0x001f, std::uint32_t{0x001f});
                                 t.check_equal(b.mdio_read(31, 28) & 0x003d, std::uint32_t{0x0039});
                                 // An empty address reads all ones.
                                 t.check_equal(b.mdio_read(1, 2), std::uint32_t{0xffff});
                                 // The write test.
                                 b.mdio_write(31, 0, 0x0080);
                                 t.check_equal(b.mdio_read(31, 0) & 0xff80, std::uint32_t{0x0080});
                                 b.mdio_write(31, 24, 0x0b00);
                                 t.check_equal(b.mdio_read(31, 24) & 0x0b00, std::uint32_t{0x0b00});
                                 // BMCR reset restores the reset values.
                                 b.mdio_write(31, 0, 0x8000);
                                 t.check_equal(b.mdio_read(31, 0) & 0xff80, std::uint32_t{0x3100});
                             }};

const test::Registration link{
    "dp83840.negotiation", [](test::Context& t) {
        Bench b;
        // No partner: no link.
        b.scheduler.advance_by(VirtualDuration{3'000'000'000});
        t.check_equal(b.mdio_read(31, 1) & 0x24, std::uint32_t{0});
        b.phy.set_link_partner(true);
        b.scheduler.advance_by(VirtualDuration{1'000'000'000});
        t.check_equal(b.mdio_read(31, 1) & 0x24, std::uint32_t{0});
        b.scheduler.advance_by(devices::Dp83840::negotiation_time);
        // Link status latches low: the first read after the link comes up still shows it down.
        b.mdio_read(31, 1);
        t.check_equal(b.mdio_read(31, 1) & 0x24, std::uint32_t{0x24}); // link, AN complete
        t.check_equal(b.mdio_read(31, 5), std::uint32_t{0x41e1});      // partner, acknowledged
        t.check(b.phy.link_up());
        // Restarting negotiation drops the link until it completes again.
        b.mdio_write(31, 0, 0x3300);
        t.check(!b.phy.link_up());
    }};

const test::Registration phy_reset_pin{
    "ioc3_ethernet.phy_reset_gpio", [](test::Context& t) {
        Bench b;
        b.ioc3.connect_gpio(5, [&b](bool high) { b.phy.set_reset(!high); });
        b.link_up();
        t.check(b.phy.link_up());
        // The PROM's pulse: GPPR[5] low, then high. The link drops and negotiates again.
        b.write(0x54, 0);
        b.scheduler.advance_by(devices::Dp83840::negotiation_time);
        t.check(!b.phy.link_up()); // held in reset
        b.write(0x54, 1);
        t.check(!b.phy.link_up());
        t.check_equal(b.mdio_read(31, 1) & 0x24, std::uint32_t{0}); // as the PROM expects
        b.scheduler.advance_by(devices::Dp83840::negotiation_time);
        t.check(b.phy.link_up());
    }};

// The Ethernet interrupt follows EISR & EIER; status bits clear when written with 1.
class Line final : public InterruptSink {
  public:
    void set_interrupt_level(std::uint32_t input, bool asserted) override {
        pin = input;
        level = asserted;
    }
    std::uint32_t pin{};
    bool level{};
};

const test::Registration interrupt{"ioc3_ethernet.interrupt", [](test::Context& t) {
                                       Bench b;
                                       Line line;
                                       b.ioc3.ethernet().connect_interrupt(line, 2);
                                       b.write(0xf8, 0x1); // EIER RXTIMERINT
                                       t.check(!line.level);
                                       b.write(0xf4,
                                               0x1); // writing 1 to an idle bit changes nothing
                                       t.check(!line.level);
                                   }};

constexpr std::uint64_t tx_ring = 0x1'0000;                 // 64 KB aligned (ETBR_ALIGNMENT)
constexpr std::uint64_t rx_ring = 0x2'0000;                 // 4 KB aligned
constexpr std::uint64_t rx_buffer = 0x2'1000;               // 128-byte aligned
constexpr std::uint64_t tx_buffer = 0x3'0102;               // unaligned, as skb data may be
constexpr std::uint32_t emcr_run = 0x0001'e000 | (5u << 3); // TX/RX DMA and enables, RXOFF 5

const test::Registration transmit{
    "ioc3_ethernet.transmit", [](test::Context& t) {
        Bench b;
        b.link_up();
        Line line;
        b.ioc3.ethernet().connect_interrupt(line, 2);
        // Descriptor 0: 60 bytes in the descriptor (D0V). Descriptor 1: 200 bytes from an
        // unaligned buffer (B1V).
        const auto short_frame = frame_to({0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 60);
        b.memory.put32(tx_ring, 60 | 0x1000 | 0x1'0000); // INTWHENDONE | D0V
        b.memory.put32(tx_ring + 4, 60);
        for (std::size_t i = 0; i < 60; ++i) {
            b.memory.bytes_.at(tx_ring + 24 + i) = short_frame[i];
        }
        const auto long_frame = frame_to({0x08, 0x00, 0x69, 0x01, 0x02, 0x03}, 200);
        b.memory.put32(tx_ring + 128, 200 | 0x2'0000); // B1V, no interrupt
        b.memory.put32(tx_ring + 128 + 4, 200u << 8);
        b.memory.put64(tx_ring + 128 + 8, 0xa000'0000'0000'0000 | tx_buffer); // PCI64 attrs
        std::ranges::copy(long_frame, b.memory.bytes_.begin() + tx_buffer);
        b.write(0x128, 0);          // ETBR_H
        b.write(0x12c, tx_ring);    // ETBR_L, 128 entries
        b.write(0xf8, 0x0041'0000); // EIER: TXEXPLICIT, TXEMPTY
        b.write(0xf0, emcr_run);
        b.write(0x134, 2 << 7); // ETPIR: two descriptors
        b.scheduler.advance_by(VirtualDuration{1'000'000});
        t.check_equal(b.cable.sent.size(), std::size_t{2});
        t.check(b.cable.sent.size() == 2 && b.cable.sent[0] == short_frame);
        t.check(b.cable.sent.size() == 2 && b.cable.sent[1] == long_frame);
        t.check_equal(b.read(0x130) & 0xffff, std::uint32_t{2 << 7}); // ETCIR
        t.check_equal(b.read(0xf4), std::uint32_t{0x0041'0000});      // EISR
        t.check(line.level);
        b.write(0xf4, 0x0041'0000);
        t.check(!line.level);
    }};

const test::Registration transmit_checksum{
    "ioc3_ethernet.transmit_checksum", [](test::Context& t) {
        Bench b;
        b.link_up();
        auto frame = frame_to({0x08, 0x00, 0x69, 0x01, 0x02, 0x03}, 64);
        frame[40] = std::byte{0};
        frame[41] = std::byte{0};
        // DOCHECKSUM at offset 40.
        b.memory.put32(tx_ring, 64 | 0x1'0000 | 0x8'0000 | (40u << 20));
        b.memory.put32(tx_ring + 4, 64);
        std::ranges::copy(frame, b.memory.bytes_.begin() + tx_ring + 24);
        b.write(0x12c, tx_ring);
        b.write(0xf0, emcr_run);
        b.write(0x134, 1 << 7);
        b.scheduler.advance_by(VirtualDuration{1'000'000});
        t.check_equal(b.cable.sent.size(), std::size_t{1});
        if (b.cable.sent.size() == 1) {
            // The inserted checksum makes the whole frame sum to 0xffff.
            std::uint32_t sum = 0;
            const auto& sent = b.cable.sent[0];
            for (std::size_t i = 0; i < sent.size(); i += 2) {
                sum += (std::to_integer<std::uint32_t>(sent[i]) << 8) |
                       std::to_integer<std::uint32_t>(sent[i + 1]);
                sum = (sum & 0xffff) + (sum >> 16);
            }
            t.check_equal(sum, std::uint32_t{0xffff});
        }
    }};

const test::Registration receive{
    "ioc3_ethernet.receive_and_filter", [](test::Context& t) {
        Bench b;
        b.link_up();
        b.set_station();
        Line line;
        b.ioc3.ethernet().connect_interrupt(line, 2);
        b.memory.put64(rx_ring, 0xa000'0000'0000'0000 | rx_buffer);
        b.memory.put64(rx_ring + 8, 0xa000'0000'0000'0000 | (rx_buffer + 0x800));
        b.write(0x100, 0);
        b.write(0x104, rx_ring);
        b.write(0x10c, 0);                      // ERCIR
        b.write(0x110, (2 << 3) | 0x8000'0000); // ERPIR: two buffers, armed
        b.write(0xf8, 0x1);                     // EIER RXTIMERINT
        b.write(0xf0, emcr_run);
        // Not for this station, then a short frame for it, then a broadcast.
        b.cable.incoming.push_back(frame_to({0x08, 0x00, 0x69, 0x0e, 0x17, 0x02}, 60));
        const auto mine = frame_to({0x08, 0x00, 0x69, 0x0e, 0x17, 0x01}, 42);
        b.cable.incoming.push_back(mine);
        b.cable.incoming.push_back(frame_to({0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 100));
        b.scheduler.advance_by(VirtualDuration{1'000'000});
        // The first buffer: V, 64 bytes (padded to 60, then the FCS), frame at 10 bytes.
        const std::uint32_t w0 = b.memory.get32(rx_buffer);
        t.check_equal(w0 >> 16, std::uint32_t{0x8000 | 64});
        // GOODPKT and the length with FCS (64 = 8 << 3 | 0), as enet_phy_loop expects it.
        t.check_equal(b.memory.get32(rx_buffer + 4), std::uint32_t{0x4000'0000 | (8u << 16)});
        t.check(std::equal(mine.begin(), mine.end(), b.memory.bytes_.begin() + rx_buffer + 10));
        t.check_equal(unsigned{std::to_integer<unsigned>(b.memory.bytes_.at(rx_buffer + 10 + 50))},
                      0u); // padding
        // The FCS: the CRC over frame and FCS leaves the IEEE residue.
        std::vector<std::byte> wire(b.memory.bytes_.begin() + rx_buffer + 10,
                                    b.memory.bytes_.begin() + rx_buffer + 10 + 64);
        const auto fcs = devices::ethernet_fcs(std::span{wire}.first(60));
        t.check(std::equal(fcs.begin(), fcs.end(), wire.begin() + 60));
        // The broadcast in the second buffer, flagged.
        t.check_equal(b.memory.get32(rx_buffer + 0x800) >> 16, std::uint32_t{0x8000 | 104});
        t.check_equal(b.memory.get32(rx_buffer + 0x800 + 4) & 0x0800'0000,
                      std::uint32_t{0x0800'0000});
        t.check_equal(b.read(0x10c), std::uint32_t{2 << 3}); // ERCIR
        t.check(line.level);
        // The hardware checksum: the ones' complement sum of frame and FCS.
        std::uint32_t sum = 0;
        for (std::size_t i = 0; i < 64; i += 2) {
            sum += (std::to_integer<std::uint32_t>(wire[i]) << 8) |
                   std::to_integer<std::uint32_t>(wire[i + 1]);
            sum = (sum & 0xffff) + (sum >> 16);
        }
        t.check_equal(w0 & 0xffff, sum);
    }};

const test::Registration loopback{
    "ioc3_ethernet.internal_loopback", [](test::Context& t) {
        Bench b;
        b.set_station();
        b.memory.put64(rx_ring, rx_buffer);
        b.write(0x104, rx_ring);
        b.write(0x110, 1 << 3);
        const auto frame = frame_to({0x08, 0x00, 0x69, 0x0e, 0x17, 0x01}, 60);
        b.memory.put32(tx_ring, 60 | 0x1'0000);
        b.memory.put32(tx_ring + 4, 60);
        std::ranges::copy(frame, b.memory.bytes_.begin() + tx_ring + 24);
        b.write(0x12c, tx_ring);
        b.write(0xf0, emcr_run | 0x2'0000); // EMCR_LOOPBACK, no link needed
        b.write(0x134, 1 << 7);
        b.scheduler.advance_by(VirtualDuration{1'000'000});
        t.check(b.cable.sent.empty());
        // The frame as sent: no FCS (the PROM's enet_ioc3_loop expects this count).
        t.check_equal(b.memory.get32(rx_buffer) >> 16, std::uint32_t{0x8000 | 60});
        t.check(std::equal(frame.begin(), frame.end(), b.memory.bytes_.begin() + rx_buffer + 10));
        t.check_equal(b.memory.get32(rx_buffer + 4), std::uint32_t{0});  // no MAC status
        t.check_equal(b.read(0xf4) & 0x1'0000, std::uint32_t{0x1'0000}); // TXEMPTY
    }};

const test::Registration fcs{"ioc3_ethernet.fcs", [](test::Context& t) {
                                 // CRC-32 check value 0xcbf43926 for "123456789".
                                 const char text[] = "123456789";
                                 const auto bytes = std::as_bytes(std::span{text, 9});
                                 const auto value = devices::ethernet_fcs(bytes);
                                 t.check(
                                     value[0] == std::byte{0x26} && value[1] == std::byte{0x39} &&
                                     value[2] == std::byte{0xf4} && value[3] == std::byte{0xcb});
                             }};

} // namespace
