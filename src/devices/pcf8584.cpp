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
constexpr std::uint8_t ack = 0x01;
// Status register S1 (read).
constexpr std::uint8_t ini = 0x40; // own address not yet written
constexpr std::uint8_t lrb = 0x08; // last received bit: 1 = not acknowledged
constexpr std::uint8_t bb = 0x01;  // bus busy, active low: 1 = bus free

// SCL rates selected by clock register bits 1:0 (LINUX I2C_PCF_TRNS*): 90, 45, 11, 1.5 kHz.
constexpr std::uint64_t scl_hertz[] = {90'000, 45'000, 11'000, 1'500};

} // namespace

Pcf8584::Pcf8584(Scheduler& scheduler, Tracer& tracer, I2cBus& bus)
    : scheduler_{scheduler}, tracer_{tracer}, bus_{bus},
      transfer_done_{scheduler.add_event("pcf8584.byte", [this] { finish_transfer(); })} {
    bus_.attach(static_cast<I2cMonitor&>(*this));
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
    receiving_ = false;
    start_pending_ = false;
    last_acknowledged_ = true;
    transfer_ = Transfer::none;
    // After reset: no transfer pending, not initialized, bus free.
    status_ = pin | ini | bb;
}

VirtualDuration Pcf8584::byte_time() const {
    // Eight data bits and an acknowledge bit.
    return VirtualDuration{9 * nanoseconds_per_second / scl_hertz[clock_ & 3]};
}

namespace {

struct Registers {
    std::uint8_t control;
    std::uint8_t status;
    std::uint8_t own_address;
    std::uint8_t clock;
    std::uint8_t vector;
    std::uint8_t data;
    bool master;
    bool receiving;
    bool start_pending;
    bool last_acknowledged;
    std::uint8_t transfer;
};

} // namespace

void Pcf8584::save_state(StateImage& image) const {
    image.put("pcf8584.registers",
              Registers{control_, status_, own_address_, clock_, vector_, data_, master_,
                        receiving_, start_pending_, last_acknowledged_,
                        static_cast<std::uint8_t>(transfer_)});
}

void Pcf8584::load_state(const StateImage& image) {
    Registers r{};
    if (image.get("pcf8584.registers", r)) {
        control_ = r.control;
        status_ = r.status;
        own_address_ = r.own_address;
        clock_ = r.clock;
        vector_ = r.vector;
        data_ = r.data;
        master_ = r.master;
        receiving_ = r.receiving;
        start_pending_ = r.start_pending;
        last_acknowledged_ = r.last_acknowledged;
        transfer_ = static_cast<Transfer>(r.transfer);
    }
}

bool Pcf8584::interrupt_asserted() const {
    return (control_ & eni) != 0 && (status_ & pin) == 0;
}

std::uint8_t Pcf8584::read(bool a0) {
    if (a0) {
        return status_;
    }
    if ((control_ & eso) != 0) {
        const std::uint8_t value = data_;
        // Master receiver: reading S0 returns the byte and starts receiving the next one.
        // The first read after the address is a dummy read. hypothesis: after a byte the
        // master did not acknowledge, reception stops (the slave has released SDA).
        if (master_ && receiving_ && transfer_ == Transfer::none && !start_pending_ &&
            last_acknowledged_) {
            begin(Transfer::receive);
        }
        return value;
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
            if (master_ && start_pending_) {
                // Repeated START with this address byte.
                start_pending_ = false;
                begin(Transfer::address);
            } else if (master_ && !receiving_) {
                begin(Transfer::transmit);
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
    if ((value & sta) != 0 && (value & sto) == 0) {
        if (!master_) {
            // START: take the bus and send S0, the address byte.
            master_ = true;
            status_ &= static_cast<std::uint8_t>(~bb);
            begin(Transfer::address);
        } else {
            // Repeated START: sent with the address written to S0 next. A reception in
            // progress is abandoned.
            scheduler_.cancel(transfer_done_);
            transfer_ = Transfer::none;
            start_pending_ = true;
        }
    } else if ((value & sto) != 0 && master_) {
        // STOP: release the bus.
        bus_.stop();
        end_mastership();
        status_ = static_cast<std::uint8_t>((status_ & ~lrb) | pin | bb);
    }
}

void Pcf8584::begin(Transfer transfer) {
    transfer_ = transfer;
    status_ |= pin;
    scheduler_.schedule_after(transfer_done_, byte_time());
}

void Pcf8584::finish_transfer() {
    bool acknowledged = true;
    switch (transfer_) {
    case Transfer::address:
        acknowledged = bus_.start(data_);
        receiving_ = (data_ & 1) != 0;
        last_acknowledged_ = true;
        tracer_.log(TraceCategory::machine, "pcf8584: start {:#04x} {}", data_,
                    acknowledged ? "ack" : "nak");
        break;
    case Transfer::transmit:
        acknowledged = bus_.write(data_);
        tracer_.log(TraceCategory::machine, "pcf8584: write {:#04x} {}", data_,
                    acknowledged ? "ack" : "nak");
        break;
    case Transfer::receive:
        data_ = bus_.read();
        // The master acknowledges according to S1.ACK when the byte completes.
        last_acknowledged_ = (control_ & ack) != 0;
        tracer_.log(TraceCategory::machine, "pcf8584: read {:#04x}", data_);
        break;
    case Transfer::none:
        return;
    }
    transfer_ = Transfer::none;
    status_ = static_cast<std::uint8_t>((status_ & ~(pin | lrb)) | (acknowledged ? 0 : lrb));
}

void Pcf8584::end_mastership() {
    scheduler_.cancel(transfer_done_);
    transfer_ = Transfer::none;
    master_ = false;
    receiving_ = false;
    start_pending_ = false;
}

void Pcf8584::i2c_observed_byte(std::uint8_t byte) {
    // The shift register takes every byte on the bus.
    if (!master_) {
        data_ = byte;
    }
}

void Pcf8584::i2c_observed_stop() {
    // A STOP from another agent ends any transfer this chip was running, and frees the bus.
    if (master_) {
        end_mastership();
    }
    status_ |= bb;
}

} // namespace ultraviolent::devices
