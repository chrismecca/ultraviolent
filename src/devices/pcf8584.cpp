#include <ultraviolent/devices/pcf8584.hpp>

namespace ultraviolent::devices {

namespace {

// Control register S1 (write), LINUX i2c-algo-pcf.h.
constexpr std::uint8_t pin = 0x80;
constexpr std::uint8_t eso = 0x40;
constexpr std::uint8_t es1 = 0x20;
constexpr std::uint8_t es2 = 0x10;
constexpr std::uint8_t eni = 0x08;
constexpr std::uint8_t sta = 0x04;
constexpr std::uint8_t sto = 0x02;
// Status register S1 (read).
constexpr std::uint8_t ini = 0x40; // own address not yet written
constexpr std::uint8_t lrb = 0x08; // last received bit: 1 = not acknowledged
constexpr std::uint8_t bb = 0x01;  // bus busy, active low: 1 = bus free

// SCL rates selected by clock register bits 1:0 (LINUX I2C_PCF_TRNS*): 90, 45, 11, 1.5 kHz.
constexpr std::uint64_t scl_hertz[] = {90'000, 45'000, 11'000, 1'500};

} // namespace

Pcf8584::Pcf8584(Scheduler& scheduler, Tracer& tracer)
    : scheduler_{scheduler}, tracer_{tracer},
      transfer_done_{scheduler.add_event("pcf8584.byte", [this] { finish_transfer(); })} {
    reset();
}

void Pcf8584::reset() {
    scheduler_.cancel(transfer_done_);
    control_ = 0;
    own_address_ = 0;
    clock_ = 0;
    vector_ = 0;
    data_ = 0;
    master_ = false;
    // After reset: no transfer pending, not initialized, bus free.
    status_ = pin | ini | bb;
}

VirtualDuration Pcf8584::byte_time() const {
    // Eight data bits and an acknowledge bit.
    return VirtualDuration{9 * nanoseconds_per_second / scl_hertz[clock_ & 3]};
}

bool Pcf8584::interrupt_asserted() const {
    return (control_ & eni) != 0 && (status_ & pin) == 0;
}

std::uint8_t Pcf8584::read(bool a0) {
    if (a0) {
        return status_;
    }
    if ((control_ & eso) != 0) {
        return data_;
    }
    if ((control_ & es1) != 0) {
        return clock_;
    }
    if ((control_ & es2) != 0) {
        return vector_;
    }
    return own_address_;
}

void Pcf8584::write(bool a0, std::uint8_t value) {
    if (!a0) {
        if ((control_ & eso) != 0) {
            data_ = value;
            // In a transfer, writing S0 sends the next byte.
            if (master_) {
                start_transfer();
            }
        } else if ((control_ & es1) != 0) {
            clock_ = value;
        } else if ((control_ & es2) != 0) {
            vector_ = value;
        } else {
            own_address_ = value;
            status_ &= static_cast<std::uint8_t>(~ini);
        }
        return;
    }

    control_ = value;
    if ((value & eni) != 0) {
        tracer_.log(TraceCategory::machine, "pcf8584: interrupt output not modeled");
    }
    if ((value & eso) == 0) {
        return;
    }
    if ((value & sta) != 0) {
        // START (or repeated START): take the bus and send S0, the address byte.
        master_ = true;
        status_ &= static_cast<std::uint8_t>(~bb);
        start_transfer();
    } else if ((value & sto) != 0) {
        // STOP: release the bus.
        master_ = false;
        scheduler_.cancel(transfer_done_);
        status_ = static_cast<std::uint8_t>((status_ & ~lrb) | pin | bb);
    }
}

void Pcf8584::start_transfer() {
    status_ |= pin;
    scheduler_.schedule_after(transfer_done_, byte_time());
    tracer_.log(TraceCategory::machine, "pcf8584: byte {:#04x}", data_);
}

void Pcf8584::finish_transfer() {
    // Nothing on the bus acknowledges.
    status_ = static_cast<std::uint8_t>((status_ & ~pin) | lrb);
}

} // namespace ultraviolent::devices
