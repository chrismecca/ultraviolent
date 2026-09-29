#include <ultraviolent/devices/elsc.hpp>

namespace ultraviolent::devices {

namespace {

// Tokens for slot codes 1-4 and CPU slices A/B: 0xde + 2 * slot + slice (see elsc.hpp).
constexpr unsigned token_count = 8;
constexpr std::uint8_t first_token = 0xde + 2;

// Command addresses the PROM uses (observed): 0x20 for commands, 0x08 for messages.
constexpr std::uint8_t command_address = 0x20;
constexpr std::uint8_t message_address = 0x08;

} // namespace

Elsc::Elsc(Scheduler& scheduler, Tracer& tracer, I2cBus& bus)
    : scheduler_{scheduler}, tracer_{tracer}, bus_{bus},
      token_event_{scheduler.add_event("elsc.token", [this] { next_token(); })},
      release_event_{scheduler.add_event("elsc.release", [this] { release_bus(); })} {
    bus_.attach(static_cast<I2cTarget&>(*this));
    reset();
}

void Elsc::reset() {
    token_index_ = 0;
    scheduler_.cancel(release_event_);
    scheduler_.schedule_after(token_event_, token_period);
}

void Elsc::next_token() {
    // Tokens are offered only while no master holds the bus.
    if (!bus_.busy()) {
        bus_.broadcast(static_cast<std::uint8_t>(first_token + token_index_));
        token_index_ = (token_index_ + 1) % token_count;
    }
    scheduler_.schedule_after(token_event_, token_period);
}

void Elsc::release_bus() {
    bus_.release();
}

bool Elsc::i2c_start(std::uint8_t address, bool read) {
    if (address == release_address && !read) {
        scheduler_.schedule_after(release_event_, release_delay);
        return true;
    }
    if (address == command_address || address == message_address) {
        tracer_.log(TraceCategory::machine, "elsc: {} address {:#04x} not modeled",
                    address == command_address ? "command" : "message", address);
    }
    return false;
}

bool Elsc::i2c_write(std::uint8_t /*byte*/) {
    return false;
}

std::uint8_t Elsc::i2c_read() {
    return 0xff;
}

void Elsc::i2c_stop() {}

void Elsc::save_state(StateImage& image) const {
    image.put("elsc.token_index", token_index_);
}

void Elsc::load_state(const StateImage& image) {
    image.get("elsc.token_index", token_index_);
    // A snapshot from before the controller was modeled has no token pending.
    if (!scheduler_.is_pending(token_event_)) {
        scheduler_.schedule_after(token_event_, token_period);
    }
}

} // namespace ultraviolent::devices
