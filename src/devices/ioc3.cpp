#include <ultraviolent/devices/ioc3.hpp>

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace ultraviolent::devices {

namespace {

// PCI_VENDOR_ID_SGI, PCI_DEVICE_ID_SGI_IOC3 (Linux; slate's BaseIO reports vendor 0x10a9
// device 0x0003 in slot 2, OBS-SLATE-0001). hypothesis: class 0, revision 1, one 1 MB memory
// BAR, interrupt pin A.
constexpr std::uint32_t ioc3_class_revision = 0x0000'0001;
constexpr std::uint32_t ioc3_bar_size = 0x10'0000;
// observed (io_config_space): after reset the IOC3's status reads 0x0280 (medium DEVSEL timing,
// fast back-to-back capable).
constexpr std::uint16_t ioc3_status = 0x0280;

// MCR, the MicroLAN master for the NIC (ioc3.h `mcr` at 0x30): the MCR encoding of the
// IRIX-derived asm-ia64/sn/nic.h (pulse << 10 | sample << 2; DONE bit 1, DATA bit 0), the same
// as BRIDGE_NIC. hypothesis: operations complete before the next access.
constexpr std::uint32_t mcr = 0x30;
// General-purpose I/O (ioc3.h gpdr at 0x3c, gppr[16] at 0x40).
constexpr std::uint32_t gpdr = 0x3c;
constexpr std::uint32_t gppr = 0x40;
// The SuperIO UARTs (ioc3.h struct ioc3_uartregs): eight byte registers each, 16550 register n
// at PCI byte base + (n ^ 3) (iu_lcr, register 3, first).
constexpr std::uint32_t uart_b_base = 0x2'0170;
constexpr std::uint32_t uart_a_base = 0x2'0178;
constexpr std::uint32_t mcr_done = 0x2;
constexpr std::uint32_t mcr_data = 0x1;
// SIO_CR (ioc3.h 0x28): bits 22:19 are read-only PCI arbiter status, ARB_DIAG (the current
// serial DMA request) and ARB_DIAG_IDLE ("0 -> active request"). Serial DMA is not modeled,
// so the arbiter is always idle (observed: IRIX waits for IDLE after "Setting rbaud").
constexpr std::uint32_t sio_cr = 0x28;
constexpr std::uint32_t sio_cr_arbiter_status = 0x0078'0000;
constexpr std::uint32_t sio_cr_arb_diag_idle = 0x0040'0000;

// Serial interrupts and the serial DMA engine (ioc3.h; Linux drivers/serial/ioc3_serial.c,
// v2.6.16, for the ring format and the driver's use).
constexpr std::uint32_t sio_ir_offset = 0x1c;
constexpr std::uint32_t sio_ies_offset = 0x20;
constexpr std::uint32_t sio_iec_offset = 0x24;
constexpr std::uint32_t sbbr_h_offset = 0xb0;
constexpr std::uint32_t sbbr_l_offset = 0xb4;
constexpr std::uint32_t port_a_offset = 0xb8; // struct ioc3_serialregs port_a, then port_b
constexpr std::uint32_t port_stride = 0x1c;
constexpr std::uint32_t sbbr_l_size = 0x1; // SBBR_L_SIZE: 4 KB rings, else 1 KB
constexpr std::uint32_t sscr_dma_en = 0x1000'0000;
constexpr std::uint32_t sscr_dma_pause = 0x2000'0000;
constexpr std::uint32_t sscr_pause_state = 0x4000'0000;
constexpr std::uint32_t sscr_rx_drain = 0x0800'0000;
constexpr std::uint32_t sscr_reset = 0x8000'0000;
constexpr std::uint32_t srcir_arm = 0x8000'0000;
// SIO_IR bits for port A; port B's are the same shifted left 9.
constexpr std::uint32_t sio_ir_tx_mt = 0x001;
constexpr std::uint32_t sio_ir_rx_timer = 0x008;
constexpr std::uint32_t sio_ir_tx_explicit = 0x080;
constexpr std::uint32_t sio_ir_memerr = 0x100;
constexpr unsigned sio_ir_port_shift = 9;
// Ring entry status/control bytes.
constexpr std::uint8_t txcb_kind = 0xc0;
constexpr std::uint8_t txcb_valid = 0x40;
constexpr std::uint8_t txcb_int_when_done = 0x20;
constexpr std::uint8_t rxsb_modem_valid = 0x40;
constexpr std::uint8_t rxsb_data_valid = 0x80;
constexpr std::size_t ring_entry = 8;

// Bytebus device 0 (ioc3.h IOC3_BYTEBUS_DEV0), the M48T35 on the BaseIO. inferred: device
// byte n is PCI byte n ^ 3, as for the UARTs (observed: IRIX writes the clock's control byte,
// device 0x7ff8, at PCI 0x87ffb; Linux rtc-m48t35.c lists IP27's registers byte-swapped).
constexpr std::uint32_t bytebus_dev0 = 0x8'0000;
constexpr std::uint32_t bytebus_dev1 = 0xa'0000;
// The M48T35's clock registers, from its control byte.
constexpr std::uint32_t timekeeper_clock = 0x7ff8;

// The 16550 register behind a UART byte offset from the port's base.
unsigned uart_register(std::uint32_t byte) {
    return byte ^ 3;
}

// A register value placed in the byte lanes of `address` within its dword.
std::uint32_t lanes(std::uint32_t dword, std::uint64_t address, unsigned size) {
    const unsigned shift = 8 * static_cast<unsigned>(address & 3);
    const std::uint32_t mask = size >= 4 ? 0xffff'ffffu : (1u << (8 * size)) - 1;
    return (dword >> shift) & mask;
}

std::uint32_t merge_lanes(std::uint32_t dword, std::uint64_t address, unsigned size,
                          std::uint32_t value) {
    const unsigned shift = 8 * static_cast<unsigned>(address & 3);
    const std::uint32_t mask = (size >= 4 ? 0xffff'ffffu : (1u << (8 * size)) - 1) << shift;
    return (dword & ~mask) | ((value << shift) & mask);
}

} // namespace

Ioc3::Ioc3(Scheduler& scheduler, Tracer& tracer)
    : scheduler_{scheduler}, tracer_{tracer},
      config_{0x10a9,
              0x0003,
              ioc3_status,
              ioc3_class_revision,
              {pci::ConfigHeader::Bar{pci::Space::memory, ioc3_bar_size}},
              1},
      tx_events_{scheduler.add_event("ioc3.tx_a", [this] { transmit_entry(0); }),
                 scheduler.add_event("ioc3.tx_b", [this] { transmit_entry(1); })},
      rx_events_{scheduler.add_event("ioc3.rx_a", [this] { receive_entry(0); }),
                 scheduler.add_event("ioc3.rx_b", [this] { receive_entry(1); })},
      ethernet_{scheduler, tracer} {}

// The serial DMA rings (struct ring_buffer): TX A, RX A, TX B, RX B, each one ring size, at
// SBBR. Ring pointers are byte offsets of 8-byte entries.
std::uint64_t Ioc3::ring_address(unsigned ring) const {
    const std::uint64_t size = ring_mask() + ring_entry;
    const std::uint64_t base = (std::uint64_t{sbbr_h_} << 32) | (sbbr_l_ & ~(4 * size - 1));
    return base + ring * size;
}

std::uint32_t Ioc3::ring_mask() const {
    // PROD_CONS_PTR_4K, PROD_CONS_PTR_1K.
    return (sbbr_l_ & sbbr_l_size) != 0 ? 0xff8 : 0x3f8;
}

// hypothesis: TX_MT is a level, set while a port's TX ring is empty (the driver disables the
// interrupt once output drains rather than clearing it); the others latch until software
// writes them to SIO_IR.
std::uint32_t Ioc3::sio_ir() const {
    std::uint32_t value = sio_ir_;
    for (unsigned port = 0; port < 2; ++port) {
        if (serial_[port].stcir == serial_[port].stpir) {
            value |= sio_ir_tx_mt << (port * sio_ir_port_shift);
        }
    }
    return value;
}

void Ioc3::update_interrupt() {
    interrupt_.set_level((sio_ir() & sio_enable_) != 0);
}

std::optional<std::uint32_t> Ioc3::read_serial(std::uint32_t offset) {
    switch (offset) {
    case sio_ir_offset:
        return sio_ir();
    case sio_ies_offset:
    case sio_iec_offset:
        return sio_enable_; // hypothesis: both read the enable mask
    case sbbr_h_offset:
        return sbbr_h_;
    case sbbr_l_offset:
        return sbbr_l_;
    default:
        break;
    }
    if (offset < port_a_offset || offset >= port_a_offset + 2 * port_stride) {
        return std::nullopt;
    }
    const SerialDma& s = serial_[(offset - port_a_offset) / port_stride];
    // SSCR_PAUSE_STATE "sets when PAUSE takes effect". hypothesis: at once, since the model
    // moves a whole ring entry in one step (observed: IRIX pauses and waits for it).
    const std::uint32_t sscr =
        (s.sscr & ~sscr_pause_state) | ((s.sscr & sscr_dma_pause) != 0 ? sscr_pause_state : 0);
    const std::uint32_t registers[] = {sscr, s.stpir, s.stcir, s.srpir, s.srcir, s.srtr, s.shadow};
    return registers[((offset - port_a_offset) % port_stride) / 4];
}

bool Ioc3::write_serial(std::uint32_t offset, std::uint32_t value) {
    switch (offset) {
    case sio_ir_offset:
        sio_ir_ &= ~value;
        update_interrupt();
        return true;
    case sio_ies_offset:
        sio_enable_ |= value;
        update_interrupt();
        return true;
    case sio_iec_offset:
        sio_enable_ &= ~value;
        update_interrupt();
        return true;
    case sbbr_h_offset:
        sbbr_h_ = value;
        return true;
    case sbbr_l_offset:
        sbbr_l_ = value;
        return true;
    default:
        break;
    }
    if (offset < port_a_offset || offset >= port_a_offset + 2 * port_stride) {
        return false;
    }
    const unsigned port = (offset - port_a_offset) / port_stride;
    SerialDma& s = serial_[port];
    const std::uint32_t mask = ring_mask();
    switch (((offset - port_a_offset) % port_stride) / 4) {
    case 0:
        if ((value & sscr_reset) != 0) {
            // SSCR_RESET: the DMA channels restart with empty rings.
            s.stpir = s.stcir = s.srpir = s.srcir = 0;
        }
        // RX_DRAIN completes at once: nothing waits in the model's packer.
        s.sscr = value & ~(sscr_reset | sscr_rx_drain | sscr_pause_state);
        tracer_.log(TraceCategory::ioc3, "serial {} sscr {:#x}", port, value);
        break;
    case 1:
        s.stpir = value & mask;
        break;
    case 2:
        s.stcir = value & mask;
        break;
    case 3:
        s.srpir = value & mask;
        break;
    case 4:
        s.srcir = value & (mask | srcir_arm);
        break;
    case 5:
        s.srtr = value;
        break;
    default:
        s.shadow = value;
        break;
    }
    start_serial(port);
    update_interrupt();
    return true;
}

void Ioc3::start_serial(unsigned port) {
    const SerialDma& s = serial_[port];
    if ((s.sscr & sscr_dma_en) == 0 || (s.sscr & sscr_dma_pause) != 0) {
        return;
    }
    if (s.stcir != s.stpir && !scheduler_.is_pending(tx_events_[port])) {
        scheduler_.schedule_after(tx_events_[port], character_time);
    }
    if (!scheduler_.is_pending(rx_events_[port])) {
        scheduler_.schedule_after(rx_events_[port], character_time);
    }
}

// One TX ring entry: its four data bytes go out in order, each whose control byte is
// TXCB_VALID; TXCB_INT_WHEN_DONE raises TX_EXPLICIT. inferred: an entry's bytes are in memory
// order, which the lane-preserving Bridge window presents most significant byte first in each
// PCI dword (as for the UART registers). hypothesis: MCR and DELAY entries are skipped.
void Ioc3::transmit_entry(unsigned port) {
    SerialDma& s = serial_[port];
    if ((s.sscr & sscr_dma_en) == 0 || (s.sscr & sscr_dma_pause) != 0 || s.stcir == s.stpir) {
        return;
    }
    std::array<std::byte, ring_entry> entry{};
    if (dma_ == nullptr || !dma_->dma_read(ring_address(2 * port) + s.stcir, entry)) {
        tracer_.log(TraceCategory::ioc3, "serial {} TX DMA failed", port);
        sio_ir_ |= sio_ir_memerr << (port * sio_ir_port_shift);
        update_interrupt();
        return;
    }
    unsigned sent = 0;
    for (unsigned k = 0; k < 4; ++k) {
        const auto data = std::to_integer<std::uint8_t>(entry[3 - k]);
        const auto control = std::to_integer<std::uint8_t>(entry[7 - k]);
        if ((control & txcb_kind) == txcb_valid) {
            uart(port).transmit(data);
            ++sent;
        }
        if ((control & txcb_int_when_done) != 0) {
            sio_ir_ |= sio_ir_tx_explicit << (port * sio_ir_port_shift);
        }
    }
    s.stcir = (s.stcir + ring_entry) & ring_mask();
    update_interrupt();
    if (s.stcir != s.stpir) {
        scheduler_.schedule_after(tx_events_[port],
                                  VirtualDuration{character_time.nanoseconds * std::max(sent, 1u)});
    }
}

// hypothesis: each received byte is written at once as its own RX ring entry (data valid,
// modem status valid), and raises RX_TIMER; the IOC3's four-byte packer and its timer are not
// modeled. A full ring (one entry short of the consumer) leaves input waiting.
void Ioc3::receive_entry(unsigned port) {
    SerialDma& s = serial_[port];
    if ((s.sscr & sscr_dma_en) == 0 || (s.sscr & sscr_dma_pause) != 0) {
        return;
    }
    scheduler_.schedule_after(rx_events_[port], character_time);
    const std::uint32_t next = (s.srpir + ring_entry) & ring_mask();
    if (next == (s.srcir & ring_mask())) {
        return;
    }
    const auto byte = uart(port).take_input();
    if (!byte) {
        return;
    }
    std::array<std::byte, ring_entry> entry{};
    entry[3] = static_cast<std::byte>(*byte);
    entry[7] = static_cast<std::byte>(rxsb_data_valid | rxsb_modem_valid);
    if (dma_ == nullptr || !dma_->dma_write(ring_address(2 * port + 1) + s.srpir, entry)) {
        tracer_.log(TraceCategory::ioc3, "serial {} RX DMA failed", port);
        return;
    }
    s.srpir = next;
    sio_ir_ |= sio_ir_rx_timer << (port * sio_ir_port_shift);
    update_interrupt();
}

void Ioc3::pci_reset() {
    config_.reset();
    registers_.clear();
    serial_ = {};
    sbbr_h_ = 0;
    sbbr_l_ = 0;
    sio_ir_ = 0;
    sio_enable_ = 0;
    for (const EventId event : tx_events_) {
        scheduler_.cancel(event);
    }
    for (const EventId event : rx_events_) {
        scheduler_.cancel(event);
    }
    uart_a_.reset();
    uart_b_.reset();
    ethernet_.reset();
    // The pins become inputs again and float high (hypothesis: pulled up on the BaseIO).
    set_gpio(0xffff);
    update_interrupt();
}

std::uint32_t Ioc3::config_read(std::uint8_t reg) {
    return config_.read(reg);
}

void Ioc3::config_write(std::uint8_t reg, std::uint32_t value, unsigned byte_enable) {
    config_.write(reg, value, byte_enable);
}

std::optional<std::uint32_t> Ioc3::read(pci::Space space, std::uint64_t address, unsigned size) {
    const auto hit = config_.decode(space, address);
    if (!hit) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::uint32_t>(hit->offset);
    if (size == 4) {
        if (const auto value = ethernet_.read(offset)) {
            return value;
        }
    }
    if (size == 1 && offset >= uart_b_base && offset < uart_a_base + 8) {
        return offset >= uart_a_base ? uart_a_.read(uart_register(offset - uart_a_base))
                                     : uart_b_.read(uart_register(offset - uart_b_base));
    }
    if (size == 1 && timekeeper_ != nullptr && offset >= bytebus_dev0 && offset < bytebus_dev1) {
        const std::uint32_t device_offset = (offset - bytebus_dev0) ^ 3;
        const std::uint8_t value = timekeeper_->read(device_offset);
        if (device_offset >= timekeeper_clock) {
            tracer_.log(TraceCategory::ioc3, "timekeeper read {:#x} = {:#04x}", device_offset,
                        value);
        }
        return value;
    }
    const auto dword_offset = offset & ~3u;
    if (size == 4) {
        if (const auto value = read_serial(offset)) {
            return value;
        }
    }
    std::uint32_t dword = 0;
    if (dword_offset < 0x10) {
        // struct ioc3 pci_*: the configuration registers. hypothesis: shown by value.
        dword = config_.read(static_cast<std::uint8_t>(dword_offset));
    } else if (const auto found = registers_.find(dword_offset); found != registers_.end()) {
        dword = found->second;
    }
    if (dword_offset == mcr) {
        dword |= mcr_done; // polled; not traced
    }
    if (dword_offset == sio_cr) {
        dword = (dword & ~sio_cr_arbiter_status) | sio_cr_arb_diag_idle;
    }
    const std::uint32_t value = lanes(dword, offset, size);
    if (dword_offset == mcr) {
        return value;
    }
    tracer_.log(TraceCategory::ioc3, "read {:#07x} size {} = {:#x}", offset, size, value);
    return value;
}

bool Ioc3::write(pci::Space space, std::uint64_t address, unsigned size, std::uint32_t value) {
    const auto hit = config_.decode(space, address);
    if (!hit) {
        return false;
    }
    const auto offset = static_cast<std::uint32_t>(hit->offset);
    if (size == 4 && ethernet_.write(offset, value)) {
        return true;
    }
    if (size == 1 && offset >= uart_b_base && offset < uart_a_base + 8) {
        tracer_.log(TraceCategory::ioc3, "uart write {:#07x} = {:#04x}", offset, value);
        if (offset >= uart_a_base) {
            uart_a_.write(uart_register(offset - uart_a_base), static_cast<std::uint8_t>(value));
        } else {
            uart_b_.write(uart_register(offset - uart_b_base), static_cast<std::uint8_t>(value));
        }
        return true;
    }
    if (size == 1 && timekeeper_ != nullptr && offset >= bytebus_dev0 && offset < bytebus_dev1) {
        const std::uint32_t device_offset = (offset - bytebus_dev0) ^ 3;
        if (device_offset >= timekeeper_clock) {
            tracer_.log(TraceCategory::ioc3, "timekeeper write {:#x} = {:#04x}", device_offset,
                        value);
        }
        timekeeper_->write(device_offset, static_cast<std::uint8_t>(value));
        return true;
    }
    if (size == 4 && write_serial(offset, value)) {
        return true;
    }
    std::uint32_t& dword = registers_[offset & ~3u];
    dword = merge_lanes(dword, offset, size, value);
    // General-purpose pins (ioc3.h): GPDR holds every pin's level, GPPR[n] pin n's alone.
    if ((offset & ~3u) == gpdr) {
        set_gpio(dword & 0xffff);
    } else if ((offset & ~3u) >= gppr && (offset & ~3u) < gppr + 64) {
        const unsigned pin = ((offset & ~3u) - gppr) / 4;
        set_gpio((gpio_levels_ & ~(1u << pin)) | ((dword & 1u) << pin));
    }
    if ((offset & ~3u) == mcr) {
        const bool line =
            nic_bus_ == nullptr || nic_bus_->pulse((dword >> 10) & 0x3ff, (dword >> 2) & 0xff);
        dword = (dword & ~(mcr_done | mcr_data)) | (line ? mcr_data : 0);
        return true;
    }
    tracer_.log(TraceCategory::ioc3, "write {:#07x} size {} = {:#x}", offset, size, value);
    return true;
}

void Ioc3::set_gpio(std::uint32_t levels) {
    const std::uint32_t changed = levels ^ gpio_levels_;
    gpio_levels_ = levels;
    for (unsigned pin = 0; pin < gpio_outputs_.size(); ++pin) {
        if (((changed >> pin) & 1) != 0 && gpio_outputs_[pin]) {
            gpio_outputs_[pin](((levels >> pin) & 1) != 0);
        }
    }
}

void Ioc3::save_state(StateImage& image) const {
    config_.save_state(image, "ioc3.config");
    uart_a_.save_state(image, "ioc3.uart_a");
    uart_b_.save_state(image, "ioc3.uart_b");
    std::vector<std::uint32_t> flat;
    for (const auto& [offset, value] : registers_) {
        flat.push_back(offset);
        flat.push_back(value);
    }
    image.put_bytes("ioc3.registers", std::as_bytes(std::span{flat}));
    std::vector<std::uint32_t> serial{sbbr_h_, sbbr_l_, sio_ir_, sio_enable_};
    for (const SerialDma& s : serial_) {
        serial.insert(serial.end(), {s.sscr, s.stpir, s.stcir, s.srpir, s.srcir, s.srtr, s.shadow});
    }
    image.put_bytes("ioc3.serial", std::as_bytes(std::span{serial}));
    ethernet_.save_state(image);
    image.put("ioc3.gpio_levels", gpio_levels_);
}

void Ioc3::load_state(const StateImage& image) {
    config_.load_state(image, "ioc3.config");
    uart_a_.load_state(image, "ioc3.uart_a");
    uart_b_.load_state(image, "ioc3.uart_b");
    if (const auto bytes = image.get_bytes("ioc3.registers")) {
        std::vector<std::uint32_t> flat(bytes->size() / sizeof(std::uint32_t));
        std::memcpy(flat.data(), bytes->data(), flat.size() * sizeof(std::uint32_t));
        registers_.clear();
        for (std::size_t i = 0; i + 1 < flat.size(); i += 2) {
            registers_[flat[i]] = flat[i + 1];
        }
    }
    if (const auto bytes = image.get_bytes("ioc3.serial");
        bytes && bytes->size() == std::size_t{18} * 4) {
        std::array<std::uint32_t, 18> v{};
        std::memcpy(v.data(), bytes->data(), bytes->size());
        sbbr_h_ = v[0];
        sbbr_l_ = v[1];
        sio_ir_ = v[2];
        sio_enable_ = v[3];
        for (std::size_t port = 0; port < 2; ++port) {
            const std::size_t b = 4 + 7 * port;
            serial_[port] = {v[b], v[b + 1], v[b + 2], v[b + 3], v[b + 4], v[b + 5], v[b + 6]};
        }
    }
    ethernet_.load_state(image);
    image.get("ioc3.gpio_levels", gpio_levels_);
    update_interrupt();
}

} // namespace ultraviolent::devices
