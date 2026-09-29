#include <ultraviolent/devices/ioc3_ethernet.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

namespace ultraviolent::devices {

namespace {

// ioc3.h struct ioc3_ethregs offsets and bits.
constexpr std::uint32_t emcr = 0xf0;
constexpr std::uint32_t eisr = 0xf4;
constexpr std::uint32_t eier = 0xf8;
constexpr std::uint32_t erbr_h = 0x100;
constexpr std::uint32_t erbr_l = 0x104;
constexpr std::uint32_t ercir = 0x10c;
constexpr std::uint32_t erpir = 0x110;
constexpr std::uint32_t etbr_h = 0x128;
constexpr std::uint32_t etbr_l = 0x12c;
constexpr std::uint32_t etcir = 0x130;
constexpr std::uint32_t etpir = 0x134;
constexpr std::uint32_t emar_h = 0x138;
constexpr std::uint32_t emar_l = 0x13c;
constexpr std::uint32_t ehar_h = 0x140;
constexpr std::uint32_t ehar_l = 0x144;
constexpr std::uint32_t micr = 0x148;
constexpr std::uint32_t midr_r = 0x14c;
constexpr std::uint32_t midr_w = 0x150;

constexpr std::uint32_t emcr_promisc = 0x0000'0002;
constexpr std::uint32_t emcr_rxoff_mask = 0x0000'01f8;
constexpr unsigned emcr_rxoff_shift = 3;
constexpr std::uint32_t emcr_rampar = 0x0000'0200;
constexpr std::uint32_t emcr_txdmaen = 0x0000'2000;
constexpr std::uint32_t emcr_txen = 0x0000'4000;
constexpr std::uint32_t emcr_rxdmaen = 0x0000'8000;
constexpr std::uint32_t emcr_rxen = 0x0001'0000;
constexpr std::uint32_t emcr_loopback = 0x0002'0000;
constexpr std::uint32_t emcr_arb_diag_idle = 0x0020'0000;
constexpr std::uint32_t emcr_rst = 0x8000'0000;

constexpr std::uint32_t eisr_rxtimerint = 0x0000'0001;
constexpr std::uint32_t eisr_rxmemerr = 0x0000'0010;
constexpr std::uint32_t eisr_txempty = 0x0001'0000;
constexpr std::uint32_t eisr_txexplicit = 0x0040'0000;
constexpr std::uint32_t eisr_txmemerr = 0x0200'0000;

constexpr std::uint32_t erpir_arm = 0x8000'0000;
// The receive ring: 512 eight-byte buffer addresses (RX_RING_ENTRIES "fixed in hardware"),
// ERCIR and ERPIR byte offsets into it.
constexpr std::uint32_t receive_ring_mask = 0xff8;
constexpr std::uint64_t erbr_alignment_mask = 0xfff; // ERBR_ALIGNMENT 4096

// The transmit ring: 128 or 512 descriptors of 128 bytes (ETBR_L_RINGSZ), ETCIR and ETPIR
// byte offsets into it.
constexpr std::uint32_t etbr_l_ringsz512 = 0x1;
constexpr std::uint64_t etbr_base_mask = 0xffff'c000;
constexpr std::uint32_t descriptor_size = 128;
constexpr std::uint32_t etcir_consume_mask = 0xffff;

constexpr std::uint32_t etxd_bytecnt_mask = 0x0000'07ff;
constexpr std::uint32_t etxd_intwhendone = 0x0000'1000;
constexpr std::uint32_t etxd_d0v = 0x0001'0000;
constexpr std::uint32_t etxd_b1v = 0x0002'0000;
constexpr std::uint32_t etxd_b2v = 0x0004'0000;
constexpr std::uint32_t etxd_dochecksum = 0x0008'0000;
constexpr std::uint32_t etxd_chkoff_mask = 0x07f0'0000;
constexpr unsigned etxd_chkoff_shift = 20;
constexpr std::uint32_t etxd_d0cnt_mask = 0x0000'007f;
constexpr std::uint32_t etxd_b1cnt_mask = 0x0007'ff00;
constexpr unsigned etxd_b1cnt_shift = 8;
constexpr std::uint32_t etxd_b2cnt_mask = 0x7ff0'0000;
constexpr unsigned etxd_b2cnt_shift = 20;
constexpr std::size_t etxd_data_offset = 24; // after cmd, bufcnt, p1, p2
constexpr std::size_t etxd_data_length = 104;

constexpr std::uint32_t erxbuf_v = 0x8000'0000;
constexpr unsigned erxbuf_bytecnt_shift = 16;
constexpr std::uint32_t erxbuf_lolen_shift = 12;
constexpr std::uint32_t erxbuf_hilen_shift = 16;
constexpr std::uint32_t erxbuf_multicast = 0x0400'0000;
constexpr std::uint32_t erxbuf_broadcast = 0x0800'0000;
constexpr std::uint32_t erxbuf_goodpkt = 0x4000'0000;

constexpr std::uint32_t micr_regaddr = 0x1f;
constexpr std::uint32_t micr_phyaddr_shift = 5;
constexpr std::uint32_t micr_readtrig = 0x400;
constexpr std::uint32_t midr_data = 0xffff;
// SSRAM words: data 15:0 (IOC3_SSRAM_DM) and parity 16 (IOC3_SSRAM_PM) are stored. observed
// (the enet_ssram diagnostic): written bit 17 is dropped; with EMCR_RAMPAR a read sets bit 17
// when the 17 stored bits have odd weight (even parity expected).
constexpr std::uint32_t ssram_stored = 0x1'ffff;
constexpr std::uint32_t ssram_parity_error = 0x2'0000;

constexpr std::size_t minimum_frame = 60; // without FCS (ETH_ZLEN)
constexpr std::size_t address_length = 6;
// 100 Mb/s: 80 ns a byte, plus preamble, FCS, and interframe gap (24 bytes).
constexpr std::uint64_t nanoseconds_per_byte = 80;
constexpr std::size_t frame_overhead = 24;

std::uint32_t big_endian32(std::span<const std::byte> bytes) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | std::to_integer<std::uint32_t>(bytes[i]);
    }
    return value;
}

std::uint64_t big_endian64(std::span<const std::byte> bytes) {
    return (std::uint64_t{big_endian32(bytes.first(4))} << 32) | big_endian32(bytes.subspan(4, 4));
}

void put_big_endian32(std::span<std::byte> bytes, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[i] = static_cast<std::byte>(value >> (24 - 8 * i));
    }
}

// The ones' complement sum of `bytes` as big-endian 16-bit words (an odd last byte is the high
// half of a word), folded to 16 bits.
std::uint32_t ones_complement_sum(std::span<const std::byte> bytes) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        std::uint32_t word = std::to_integer<std::uint32_t>(bytes[i]) << 8;
        if (i + 1 < bytes.size()) {
            word |= std::to_integer<std::uint32_t>(bytes[i + 1]);
        }
        sum += word;
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return sum;
}

// Ethernet CRC-32, reflected (polynomial 0xedb88320), initial all ones, not complemented:
// Linux ether_crc_le.
std::uint32_t crc32_le(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xffff'ffff;
    for (const std::byte b : bytes) {
        crc ^= std::to_integer<std::uint32_t>(b);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb8'8320u : 0u);
        }
    }
    return crc;
}

// The multicast hash filter bit for a destination address (Linux ioc3_hash: the low six bits
// of the little-endian CRC, bit-reversed).
unsigned multicast_hash(std::span<const std::byte> address) {
    std::uint32_t crc = crc32_le(address) & 0x3f;
    unsigned index = 0;
    for (int bit = 0; bit < 6; ++bit) {
        index = (index << 1) | (crc & 1);
        crc >>= 1;
    }
    return index;
}

// Frames in a snapshot: each a 32-bit little-endian length, then the bytes.
std::vector<std::byte> pack_frames(const std::deque<std::vector<std::byte>>& frames) {
    std::vector<std::byte> packed;
    for (const auto& frame : frames) {
        const auto size = static_cast<std::uint32_t>(frame.size());
        for (unsigned i = 0; i < 4; ++i) {
            packed.push_back(static_cast<std::byte>(size >> (8 * i)));
        }
        packed.insert(packed.end(), frame.begin(), frame.end());
    }
    return packed;
}

std::deque<std::vector<std::byte>> unpack_frames(std::optional<std::span<const std::byte>> bytes) {
    std::deque<std::vector<std::byte>> frames;
    if (!bytes) {
        return frames;
    }
    std::size_t at = 0;
    while (at + 4 <= bytes->size()) {
        std::uint32_t size = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            size |= std::to_integer<std::uint32_t>((*bytes)[at + i]) << (8 * i);
        }
        at += 4;
        if (at + size > bytes->size()) {
            break;
        }
        frames.emplace_back(bytes->begin() + static_cast<std::ptrdiff_t>(at),
                            bytes->begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
    }
    return frames;
}

} // namespace

std::array<std::byte, 4> ethernet_fcs(std::span<const std::byte> frame) {
    const std::uint32_t fcs = ~crc32_le(frame);
    return {static_cast<std::byte>(fcs), static_cast<std::byte>(fcs >> 8),
            static_cast<std::byte>(fcs >> 16), static_cast<std::byte>(fcs >> 24)};
}

Ioc3Ethernet::Ioc3Ethernet(Scheduler& scheduler, Tracer& tracer)
    : scheduler_{scheduler}, tracer_{tracer},
      transmit_event_{scheduler.add_event("ioc3.ethernet.tx", [this] { transmit_descriptor(); })},
      receive_event_{scheduler.add_event("ioc3.ethernet.rx", [this] { poll_receive(); })} {}

void Ioc3Ethernet::reset() {
    registers_ = {};
    reset_engines();
}

// observed (the BASEIO PROM's enet_ioc3_loop): software writes the ring bases (ETBR, ERBR)
// before EMCR_RST and the pointers after it, so the reset keeps the bases. hypothesis: it also
// keeps the station address, hash filter, and timing registers, and clears the pointers,
// EISR, and EIER.
void Ioc3Ethernet::reset_engines() {
    for (const std::uint32_t offset : {eisr, eier, ercir, erpir, etcir, etpir}) {
        reg(offset) = 0;
    }
    backlog_.clear();
    looped_.clear();
    receive_armed_ = false;
    receive_unannounced_ = false;
    scheduler_.cancel(transmit_event_);
    scheduler_.cancel(receive_event_);
    update_interrupt();
}

bool Ioc3Ethernet::transmit_enabled() const {
    const std::uint32_t control = registers_[(emcr - register_first) / 4];
    return (control & (emcr_txdmaen | emcr_txen)) == (emcr_txdmaen | emcr_txen) &&
           (control & emcr_rst) == 0;
}

bool Ioc3Ethernet::receive_enabled() const {
    const std::uint32_t control = registers_[(emcr - register_first) / 4];
    return (control & (emcr_rxdmaen | emcr_rxen)) == (emcr_rxdmaen | emcr_rxen) &&
           (control & emcr_rst) == 0;
}

bool Ioc3Ethernet::loopback() const {
    return (registers_[(emcr - register_first) / 4] & emcr_loopback) != 0;
}

std::optional<std::uint32_t> Ioc3Ethernet::read(std::uint32_t offset) {
    if (offset >= ssram_first && offset < ssram_end) {
        std::uint32_t value = ssram_[(offset - ssram_first) / 4];
        if ((reg(emcr) & emcr_rampar) != 0 && (std::popcount(value) & 1) != 0) {
            value |= ssram_parity_error;
        }
        return value;
    }
    if (offset < register_first || offset >= register_end) {
        return std::nullopt;
    }
    offset &= ~3u;
    if (offset == emcr) {
        // observed (IRIX ef_ioc3reset): after EMCR_RST software waits for ARB_DIAG_IDLE, the
        // DMA arbiter having no request. DMA completes within an event here, so it is idle.
        return reg(emcr) | emcr_arb_diag_idle;
    }
    return reg(offset);
}

bool Ioc3Ethernet::write(std::uint32_t offset, std::uint32_t value) {
    if (offset >= ssram_first && offset < ssram_end) {
        ssram_[(offset - ssram_first) / 4] = value & ssram_stored;
        return true;
    }
    if (offset < register_first || offset >= register_end) {
        return false;
    }
    offset &= ~3u;
    if (offset != micr && offset != midr_w) {
        tracer_.log(TraceCategory::ethernet, "write {:#05x} = {:#x}", offset, value);
    }
    switch (offset) {
    case emcr:
        if ((value & emcr_rst) != 0) {
            // hypothesis: RST reads back until software clears it (IRIX and Linux write 0
            // next).
            reset_engines();
        }
        reg(emcr) = value & ~emcr_arb_diag_idle;
        start_transmit();
        if (receive_enabled() && !scheduler_.is_pending(receive_event_)) {
            scheduler_.schedule_after(receive_event_, receive_poll);
        }
        return true;
    case eisr:
        // Interrupt status bits clear when written with 1 (Linux ioc3-eth.c writes back what
        // it read).
        reg(eisr) &= ~value;
        update_interrupt();
        return true;
    case eier:
        reg(eier) = value;
        update_interrupt();
        return true;
    case erpir:
        reg(erpir) = value & ~erpir_arm;
        if ((value & erpir_arm) != 0) {
            receive_armed_ = true;
            if (receive_unannounced_) {
                receive_unannounced_ = false;
                receive_armed_ = false;
                raise(eisr_rxtimerint);
            }
        }
        return true;
    case etpir:
        reg(etpir) = value;
        start_transmit();
        return true;
    case micr:
        reg(micr) = value;
        mii_command(value);
        return true;
    case midr_r:
        return true; // read-only
    default:
        reg(offset) = value;
        return true;
    }
}

// MII management (Linux ioc3_mdio_read/write): MICR names the PHY and register; with
// READTRIG the PHY's register is read into MIDR_R, otherwise MIDR_W is written to it. A PHY
// address with no PHY reads all ones, as an undriven MDIO line does. hypothesis: the
// transaction completes at once, so MICR_BUSY never reads set.
void Ioc3Ethernet::mii_command(std::uint32_t command) {
    const unsigned address = (command >> micr_phyaddr_shift) & 31;
    const unsigned phy_reg = command & micr_regaddr;
    const bool present = phy_ != nullptr && address == phy_address_;
    if ((command & micr_readtrig) != 0) {
        reg(midr_r) = present ? phy_->read(phy_reg) : midr_data;
        return;
    }
    if (present) {
        phy_->write(phy_reg, static_cast<std::uint16_t>(reg(midr_w) & midr_data));
    }
}

void Ioc3Ethernet::raise(std::uint32_t eisr_bits) {
    reg(eisr) |= eisr_bits;
    update_interrupt();
}

void Ioc3Ethernet::update_interrupt() {
    interrupt_.set_level((reg(eisr) & reg(eier)) != 0);
}

bool Ioc3Ethernet::read_memory(std::uint64_t address, std::span<std::byte> out) {
    if (out.empty()) {
        return true;
    }
    const std::uint64_t first = address & ~std::uint64_t{3};
    const std::uint64_t end = (address + out.size() + 3) & ~std::uint64_t{3};
    std::vector<std::byte> lanes(end - first);
    if (dma_ == nullptr || !dma_->dma_read(first, lanes)) {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = lanes[((address + i) ^ 3) - first];
    }
    return true;
}

bool Ioc3Ethernet::write_memory(std::uint64_t address, std::span<const std::byte> in) {
    if (in.empty()) {
        return true;
    }
    const std::uint64_t first = address & ~std::uint64_t{3};
    const std::uint64_t end = (address + in.size() + 3) & ~std::uint64_t{3};
    std::vector<std::byte> lanes(end - first);
    if (dma_ == nullptr) {
        return false;
    }
    // Partial dwords at the ends keep their other bytes.
    if ((first != address || end != address + in.size()) && !dma_->dma_read(first, lanes)) {
        return false;
    }
    for (std::size_t i = 0; i < in.size(); ++i) {
        lanes[((address + i) ^ 3) - first] = in[i];
    }
    return dma_->dma_write(first, lanes);
}

void Ioc3Ethernet::start_transmit() {
    if (transmit_enabled() && reg(etcir) != (reg(etpir) & etcir_consume_mask) &&
        !scheduler_.is_pending(transmit_event_)) {
        scheduler_.schedule_after(transmit_event_, VirtualDuration{0});
    }
}

// One transmit descriptor (ioc3.h struct ioc3_etxd, big-endian): the frame is the
// descriptor's own data (D0V), then buffer 1 (B1V), then buffer 2 (B2V). With DOCHECKSUM the
// ones' complement sum of the whole frame is complemented into the 16 bits at CHKOFF (Linux
// ioc3_start_xmit leaves the pseudo-header and MAC-header corrections there). INTWHENDONE
// raises TXEXPLICIT; an empty ring raises TXEMPTY. The next descriptor starts after this
// frame's time on the wire.
void Ioc3Ethernet::transmit_descriptor() {
    if (!transmit_enabled()) {
        return;
    }
    const std::uint32_t ring_bytes =
        ((reg(etbr_l) & etbr_l_ringsz512) != 0 ? 512 : 128) * descriptor_size;
    const std::uint32_t consume = reg(etcir) & etcir_consume_mask & (ring_bytes - 1);
    if (consume == ((reg(etpir) & etcir_consume_mask) & (ring_bytes - 1))) {
        return;
    }
    const std::uint64_t base = (std::uint64_t{reg(etbr_h)} << 32) | (reg(etbr_l) & etbr_base_mask);
    std::array<std::byte, descriptor_size> descriptor{};
    if (!read_memory(base + consume, descriptor)) {
        tracer_.log(TraceCategory::ethernet, "TX descriptor DMA failed");
        raise(eisr_txmemerr);
        return;
    }
    const std::uint32_t command = big_endian32(std::span{descriptor}.subspan(0, 4));
    const std::uint32_t counts = big_endian32(std::span{descriptor}.subspan(4, 4));
    std::vector<std::byte> frame;
    if ((command & etxd_d0v) != 0) {
        const std::size_t length =
            std::min<std::size_t>(counts & etxd_d0cnt_mask, etxd_data_length);
        const auto data = std::span{descriptor}.subspan(etxd_data_offset, length);
        frame.insert(frame.end(), data.begin(), data.end());
    }
    const auto append_buffer = [&](std::size_t pointer_offset, std::size_t length) {
        const std::uint64_t pointer =
            big_endian64(std::span{descriptor}.subspan(pointer_offset, 8));
        const std::size_t at = frame.size();
        frame.resize(at + length);
        return read_memory(pointer, std::span{frame}.subspan(at, length));
    };
    bool ok = true;
    if ((command & etxd_b1v) != 0) {
        ok = append_buffer(8, (counts & etxd_b1cnt_mask) >> etxd_b1cnt_shift);
    }
    if (ok && (command & etxd_b2v) != 0) {
        ok = append_buffer(16, (counts & etxd_b2cnt_mask) >> etxd_b2cnt_shift);
    }
    if (!ok) {
        tracer_.log(TraceCategory::ethernet, "TX buffer DMA failed");
        raise(eisr_txmemerr);
        return;
    }
    // hypothesis: the descriptor's total byte count bounds the frame.
    frame.resize(std::min<std::size_t>(frame.size(), command & etxd_bytecnt_mask));
    if ((command & etxd_dochecksum) != 0) {
        const std::size_t at = (command & etxd_chkoff_mask) >> etxd_chkoff_shift;
        if (at + 2 <= frame.size()) {
            const std::uint32_t sum = ~ones_complement_sum(frame) & 0xffff;
            frame[at] = static_cast<std::byte>(sum >> 8);
            frame[at + 1] = static_cast<std::byte>(sum);
        }
    }
    tracer_.log(TraceCategory::ethernet, "TX {} bytes{}", frame.size(),
                loopback() ? " (loopback)" : "");
    // The IOC3's internal loopback returns the frame before the MAC adds its FCS; the PHY's
    // returns it from the wire side, like a received frame (observed: enet_ioc3_loop and
    // enet_phy_loop expect counts without and with the FCS).
    if (loopback() || (phy_ != nullptr && phy_->loopback())) {
        auto& queue = loopback() ? looped_ : backlog_;
        if (queue.size() < receive_backlog) {
            queue.push_back(frame);
        }
        if (receive_enabled() && !scheduler_.is_pending(receive_event_)) {
            scheduler_.schedule_after(receive_event_, VirtualDuration{0});
        }
    } else if (link_ != nullptr && phy_ != nullptr && phy_->link_up()) {
        link_->send(frame);
    }
    reg(etcir) =
        (reg(etcir) & ~etcir_consume_mask) | ((consume + descriptor_size) & (ring_bytes - 1));
    std::uint32_t bits = (command & etxd_intwhendone) != 0 ? eisr_txexplicit : 0;
    if ((reg(etcir) & etcir_consume_mask) ==
        ((reg(etpir) & etcir_consume_mask) & (ring_bytes - 1))) {
        bits |= eisr_txempty;
    }
    raise(bits);
    const std::size_t wire = std::max(frame.size(), minimum_frame) + frame_overhead;
    scheduler_.schedule_after(transmit_event_, VirtualDuration{wire * nanoseconds_per_byte});
}

// Station address (Linux __ioc3_set_mac_address: EMAR_L holds bytes 0-3, least significant
// first; EMAR_H bytes 4-5), broadcast, the multicast hash filter (EHAR_H:EHAR_L, bit
// multicast_hash), or everything in promiscuous mode.
bool Ioc3Ethernet::accepts(std::span<const std::byte> frame) const {
    if (frame.size() < address_length) {
        return false;
    }
    const auto destination = frame.first(address_length);
    const auto byte = [&destination](std::size_t i) {
        return std::to_integer<std::uint32_t>(destination[i]);
    };
    const std::uint32_t control = registers_[(emcr - register_first) / 4];
    if ((control & emcr_promisc) != 0) {
        return true;
    }
    if ((byte(0) & 1) != 0) {
        if (std::ranges::all_of(destination, [](std::byte b) { return b == std::byte{0xff}; })) {
            return true;
        }
        const std::uint64_t hash =
            (std::uint64_t{registers_[(ehar_h - register_first) / 4]} << 32) |
            registers_[(ehar_l - register_first) / 4];
        return ((hash >> multicast_hash(destination)) & 1) != 0;
    }
    const std::uint32_t low = registers_[(emar_l - register_first) / 4];
    const std::uint32_t high = registers_[(emar_h - register_first) / 4];
    return byte(0) == (low & 0xff) && byte(1) == ((low >> 8) & 0xff) &&
           byte(2) == ((low >> 16) & 0xff) && byte(3) == (low >> 24) && byte(4) == (high & 0xff) &&
           byte(5) == ((high >> 8) & 0xff);
}

void Ioc3Ethernet::poll_receive() {
    if (!receive_enabled()) {
        return;
    }
    scheduler_.schedule_after(receive_event_, receive_poll);
    if (backlog_.empty() && link_ != nullptr && phy_ != nullptr && phy_->link_up()) {
        while (auto frame = link_->receive()) {
            if (accepts(*frame)) {
                backlog_.push_back(std::move(*frame));
                break;
            }
        }
    }
    while (!looped_.empty() && deliver(looped_.front(), false)) {
        looped_.pop_front();
    }
    while (looped_.empty() && !backlog_.empty() && deliver(backlog_.front(), true)) {
        backlog_.pop_front();
    }
}

// A received frame (ioc3.h struct ioc3_erxbuf): at the next buffer the receive ring offers
// (ERCIR short of ERPIR), the frame and its FCS go at EMCR_RXOFF half-words, then the error
// word and the valid word are written: V, the byte count with the FCS, and the ones'
// complement sum of the frame and FCS (Linux ioc3_tcpudp_checksum removes the MAC header and
// the FCS from it). Short frames are padded to 60 bytes. RXTIMERINT goes up when armed.
// observed (enet_ioc3_loop): in the IOC3's internal loopback the count and sum cover the frame
// as transmitted, with no FCS (hypothesis: and no padding), and the status word is 0.
bool Ioc3Ethernet::deliver(std::span<const std::byte> frame, bool from_wire) {
    const std::uint32_t consume = reg(ercir) & receive_ring_mask;
    if (consume == (reg(erpir) & receive_ring_mask)) {
        return false; // no buffer
    }
    const std::uint64_t ring =
        ((std::uint64_t{reg(erbr_h)} << 32) | reg(erbr_l)) & ~erbr_alignment_mask;
    std::array<std::byte, 8> pointer{};
    std::vector<std::byte> wire(frame.begin(), frame.end());
    if (from_wire) {
        wire.resize(std::max(wire.size(), minimum_frame));
        const auto fcs = ethernet_fcs(wire);
        wire.insert(wire.end(), fcs.begin(), fcs.end());
    }
    if (!read_memory(ring + consume, pointer)) {
        raise(eisr_rxmemerr);
        return true; // dropped
    }
    const std::uint64_t buffer = big_endian64(pointer);
    const std::uint32_t offset = ((reg(emcr) & emcr_rxoff_mask) >> emcr_rxoff_shift) * 2;
    const auto length = static_cast<std::uint32_t>(wire.size());
    // The MAC's receive status vector: a good frame, its length with the FCS in LOLEN (bits
    // 2:0) and HILEN (bits 12:3), broadcast or multicast. observed (enet_phy_loop expects
    // 0x40261000 for 305 bytes; IRIX's ef_recv tests V, GOODPKT, the byte count, and CRC and
    // code errors). observed (enet_ioc3_loop): internal loopback leaves the status 0.
    std::uint32_t status = from_wire ? erxbuf_goodpkt | ((length & 7) << erxbuf_lolen_shift) |
                                           ((length >> 3) << erxbuf_hilen_shift)
                                     : 0;
    if (from_wire && (std::to_integer<unsigned>(frame[0]) & 1) != 0) {
        status |= std::ranges::all_of(frame.first(address_length),
                                      [](std::byte b) { return b == std::byte{0xff}; })
                      ? erxbuf_broadcast
                      : erxbuf_multicast;
    }
    std::array<std::byte, 8> header{};
    put_big_endian32(std::span{header}.subspan(0, 4),
                     erxbuf_v | (length << erxbuf_bytecnt_shift) | ones_complement_sum(wire));
    put_big_endian32(std::span{header}.subspan(4, 4), status);
    if (!write_memory(buffer + offset, wire) ||
        !write_memory(buffer + 4, std::span{header}.subspan(4, 4)) ||
        !write_memory(buffer, std::span{header}.subspan(0, 4))) {
        raise(eisr_rxmemerr);
        return true; // dropped
    }
    tracer_.log(TraceCategory::ethernet, "RX {} bytes", frame.size());
    reg(ercir) = (reg(ercir) & ~receive_ring_mask) | ((consume + 8) & receive_ring_mask);
    if (receive_armed_) {
        receive_armed_ = false;
        raise(eisr_rxtimerint);
    } else {
        receive_unannounced_ = true;
    }
    return true;
}

void Ioc3Ethernet::save_state(StateImage& image) const {
    image.put_bytes("ioc3.ethernet.registers", std::as_bytes(std::span{registers_}));
    image.put_bytes("ioc3.ethernet.ssram", std::as_bytes(std::span{ssram_}));
    image.put("ioc3.ethernet.receive_armed", receive_armed_);
    image.put("ioc3.ethernet.receive_unannounced", receive_unannounced_);
    image.put_bytes("ioc3.ethernet.backlog", pack_frames(backlog_));
    image.put_bytes("ioc3.ethernet.looped", pack_frames(looped_));
}

void Ioc3Ethernet::load_state(const StateImage& image) {
    if (const auto bytes = image.get_bytes("ioc3.ethernet.registers");
        bytes && bytes->size() == sizeof registers_) {
        std::memcpy(registers_.data(), bytes->data(), sizeof registers_);
    }
    if (const auto bytes = image.get_bytes("ioc3.ethernet.ssram");
        bytes && bytes->size() == ssram_.size() * sizeof(std::uint32_t)) {
        std::memcpy(ssram_.data(), bytes->data(), bytes->size());
    }
    image.get("ioc3.ethernet.receive_armed", receive_armed_);
    image.get("ioc3.ethernet.receive_unannounced", receive_unannounced_);
    backlog_ = unpack_frames(image.get_bytes("ioc3.ethernet.backlog"));
    looped_ = unpack_frames(image.get_bytes("ioc3.ethernet.looped"));
    update_interrupt();
}

} // namespace ultraviolent::devices
