#include <ultraviolent/devices/uart16550.hpp>

#include <vector>

// Register semantics: National Semiconductor PC16550D datasheet.
namespace ultraviolent::devices {

namespace {

constexpr std::uint8_t lcr_dlab = 0x80;
constexpr std::uint8_t mcr_dtr = 0x01;
constexpr std::uint8_t mcr_rts = 0x02;
constexpr std::uint8_t mcr_out1 = 0x04;
constexpr std::uint8_t mcr_out2 = 0x08;
constexpr std::uint8_t mcr_loop = 0x10;
constexpr std::uint8_t lsr_dr = 0x01;
constexpr std::uint8_t lsr_oe = 0x02;
constexpr std::uint8_t lsr_thre = 0x20;
constexpr std::uint8_t lsr_temt = 0x40;
constexpr std::uint8_t iir_none = 0x01;
constexpr std::uint8_t fcr_enable = 0x01;
constexpr std::uint8_t fcr_rx_reset = 0x02;
constexpr std::size_t fifo_depth = 16;

} // namespace

void Uart16550::reset() {
    rx_.clear();
    ier_ = 0;
    fcr_ = 0;
    lcr_ = 0;
    mcr_ = 0;
    scr_ = 0;
    dll_ = 0;
    dlm_ = 0;
    overrun_ = false;
}

void Uart16550::receive(std::uint8_t byte) {
    const std::size_t capacity = (fcr_ & fcr_enable) != 0 ? fifo_depth : 1;
    if (rx_.size() >= capacity) {
        overrun_ = true;
        return;
    }
    rx_.push_back(byte);
}

void Uart16550::transmit(std::uint8_t byte) {
    if ((mcr_ & mcr_loop) != 0) {
        receive(byte);
    } else if (sink_ != nullptr) {
        sink_->serial_transmit(byte);
    }
}

std::optional<std::uint8_t> Uart16550::take_input() {
    if (!rx_.empty()) {
        const std::uint8_t byte = rx_.front();
        rx_.pop_front();
        return byte;
    }
    if (source_ != nullptr && (mcr_ & mcr_loop) == 0) {
        return source_->serial_receive();
    }
    return std::nullopt;
}

void Uart16550::poll_source() {
    if (source_ != nullptr && rx_.empty() && (mcr_ & mcr_loop) == 0) {
        if (const auto byte = source_->serial_receive()) {
            rx_.push_back(*byte);
        }
    }
}

std::uint8_t Uart16550::read(unsigned reg) {
    const bool dlab = (lcr_ & lcr_dlab) != 0;
    if ((reg & 7) == 5 || ((reg & 7) == 0 && !dlab)) {
        poll_source();
    }
    switch (reg & 7) {
    case 0:
        if (dlab) {
            return dll_;
        }
        if (rx_.empty()) {
            return 0;
        }
        {
            const std::uint8_t byte = rx_.front();
            rx_.pop_front();
            return byte;
        }
    case 1:
        return dlab ? dlm_ : ier_;
    case 2:
        // No interrupt pending; FIFOs enabled bits 7:6 when FCR enabled them.
        return static_cast<std::uint8_t>(iir_none | ((fcr_ & fcr_enable) != 0 ? 0xc0 : 0));
    case 3:
        return lcr_;
    case 4:
        return mcr_;
    case 5: {
        std::uint8_t lsr = lsr_thre | lsr_temt;
        if (!rx_.empty()) {
            lsr |= lsr_dr;
        }
        if (overrun_) {
            lsr |= lsr_oe;
            overrun_ = false; // cleared by reading LSR
        }
        return lsr;
    }
    case 6:
        if ((mcr_ & mcr_loop) != 0) {
            // Loopback: CTS = RTS, DSR = DTR, RI = OUT1, DCD = OUT2.
            return static_cast<std::uint8_t>(
                ((mcr_ & mcr_rts) != 0 ? 0x10 : 0) | ((mcr_ & mcr_dtr) != 0 ? 0x20 : 0) |
                ((mcr_ & mcr_out1) != 0 ? 0x40 : 0) | ((mcr_ & mcr_out2) != 0 ? 0x80 : 0));
        }
        return 0;
    default:
        return scr_;
    }
}

void Uart16550::write(unsigned reg, std::uint8_t value) {
    const bool dlab = (lcr_ & lcr_dlab) != 0;
    switch (reg & 7) {
    case 0:
        if (dlab) {
            dll_ = value;
        } else if ((mcr_ & mcr_loop) != 0) {
            receive(value); // loopback: the transmitter feeds the receiver
        } else if (sink_ != nullptr) {
            sink_->serial_transmit(value);
        }
        return;
    case 1:
        if (dlab) {
            dlm_ = value;
        } else {
            ier_ = value & 0x0f;
        }
        return;
    case 2:
        fcr_ = value;
        if ((value & fcr_rx_reset) != 0) {
            rx_.clear();
        }
        return;
    case 3:
        lcr_ = value;
        return;
    case 4:
        mcr_ = value & 0x1f;
        return;
    case 7:
        scr_ = value;
        return;
    default:
        return; // LSR and MSR are read-only
    }
}

void Uart16550::save_state(StateImage& image, const std::string& key) const {
    const std::uint8_t registers[] = {ier_, fcr_, lcr_, mcr_, scr_, dll_, dlm_};
    image.put(key + ".registers", registers);
    const std::vector<std::uint8_t> rx(rx_.begin(), rx_.end());
    image.put_bytes(key + ".rx", std::as_bytes(std::span{rx}));
}

void Uart16550::load_state(const StateImage& image, const std::string& key) {
    std::uint8_t registers[7]{};
    if (image.get(key + ".registers", registers)) {
        ier_ = registers[0];
        fcr_ = registers[1];
        lcr_ = registers[2];
        mcr_ = registers[3];
        scr_ = registers[4];
        dll_ = registers[5];
        dlm_ = registers[6];
    }
    if (const auto rx = image.get_bytes(key + ".rx")) {
        rx_.clear();
        for (const std::byte b : *rx) {
            rx_.push_back(std::to_integer<std::uint8_t>(b));
        }
    }
}

} // namespace ultraviolent::devices
