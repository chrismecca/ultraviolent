#include <ultraviolent/devices/elsc.hpp>

#include <algorithm>
#include <charconv>

namespace ultraviolent::devices {

namespace {

// Tokens for slot codes 1-4 and CPU slices A/B: 0xde + 2 * slot + slice (see elsc.hpp).
constexpr unsigned token_count = 8;
constexpr std::uint8_t first_token = 0xde + 2;
constexpr std::uint8_t message_flag = 8;

// The address IRIX's elsc_ust_read and elsc_ust_write use (not modeled).
constexpr std::uint8_t ust_address = 0x20;

constexpr std::string_view keyswitch_off_message = "pd off";

} // namespace

Elsc::Elsc(Scheduler& scheduler, Tracer& tracer, I2cBus& bus)
    : scheduler_{scheduler}, tracer_{tracer}, bus_{bus},
      token_event_{scheduler.add_event("elsc.token", [this] { next_token(); })},
      release_event_{scheduler.add_event("elsc.release", [this] { release_bus(); })},
      power_event_{scheduler.add_event("elsc.power_off", [this] { remove_power(); })} {
    bus_.attach(static_cast<I2cTarget&>(*this));
    reset();
}

void Elsc::reset() {
    token_index_ = 0;
    scheduler_.cancel(release_event_);
    scheduler_.schedule_after(token_event_, token_period);
    command_length_ = 0;
    receiving_command_ = false;
    answer_length_ = 0;
    answer_position_ = 0;
}

void Elsc::turn_keyswitch_off() {
    if (powered_off_ || scheduler_.is_pending(power_event_)) {
        return;
    }
    tracer_.log(TraceCategory::machine, "elsc: keyswitch off");
    queue_message(keyswitch_off_message);
    scheduler_.schedule_after(power_event_, keyswitch_grace);
}

void Elsc::queue_message(std::string_view message) {
    const std::size_t length = std::min(message.size(), message_.size());
    std::copy_n(message.begin(), length, message_.begin());
    message_length_ = static_cast<std::uint8_t>(length);
    message_waiting_ = true;
    if (message_interrupt_) {
        message_interrupt_();
    }
}

void Elsc::remove_power() {
    tracer_.log(TraceCategory::machine, "elsc: power off");
    powered_off_ = true;
    scheduler_.cancel(token_event_);
    if (power_off_) {
        power_off_();
    }
}

void Elsc::next_token() {
    // Tokens are offered only while no master holds the bus.
    if (!bus_.busy()) {
        const auto flag = static_cast<std::uint8_t>(message_waiting_ ? message_flag : 0);
        bus_.broadcast(static_cast<std::uint8_t>(first_token + token_index_ + flag));
        token_index_ = (token_index_ + 1) % token_count;
    }
    scheduler_.schedule_after(token_event_, token_period);
}

void Elsc::release_bus() {
    bus_.release();
}

std::optional<std::string> Elsc::answer() {
    const std::span<const std::uint8_t> packet{command_.data(), command_length_};
    if (packet.empty() || packet.size() != std::size_t{packet[0]} + 2) {
        return std::nullopt;
    }
    std::uint8_t sum = 0;
    for (const std::uint8_t byte : packet) {
        sum = static_cast<std::uint8_t>(sum + byte);
    }
    if (sum != 0) {
        return "err csum";
    }
    const std::string_view text{reinterpret_cast<const char*>(packet.data() + 1), packet[0]};
    if (text == "get") {
        // IRIX treats an empty answer as "MSC interrupt without message".
        std::string message{message_.data(), message_waiting_ ? message_length_ : 0u};
        message_waiting_ = false;
        return message;
    }
    if (text.starts_with("pwr d ")) {
        // Remove power in N seconds (hexadecimal, IRIX's "pwr d %x"); IRIX's shutdown asks for
        // 8 every second and for 1 at the end (sysctlr.c msc_shutdown, sysctlr_power_off).
        const std::string_view digits = text.substr(6);
        unsigned seconds = 0;
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), seconds, 16);
        if (error != std::errc{} || end != digits.data() + digits.size()) {
            return "err arg";
        }
        scheduler_.schedule_after(power_event_,
                                  VirtualDuration{std::uint64_t{seconds} * 1'000'000'000});
        return "ok";
    }
    tracer_.log(TraceCategory::machine, "elsc: command \"{}\" not modeled", text);
    return std::nullopt;
}

bool Elsc::i2c_start(std::uint8_t address, bool read) {
    if (address == release_address && !read) {
        scheduler_.schedule_after(release_event_, release_delay);
        return true;
    }
    if (address == command_address && !read) {
        command_length_ = 0;
        receiving_command_ = true;
        return true;
    }
    if (address == command_address && read && receiving_command_) {
        receiving_command_ = false;
        const auto text = answer();
        if (!text) {
            return false;
        }
        const auto length = static_cast<std::uint8_t>(std::min(text->size(), packet_max));
        answer_[0] = length;
        std::uint8_t sum = length;
        for (std::size_t i = 0; i < length; ++i) {
            answer_[i + 1] = static_cast<std::uint8_t>((*text)[i]);
            sum = static_cast<std::uint8_t>(sum + answer_[i + 1]);
        }
        answer_[length + 1] = static_cast<std::uint8_t>(-sum);
        answer_length_ = static_cast<std::uint8_t>(length + 2);
        answer_position_ = 0;
        return true;
    }
    if (address == ust_address) {
        tracer_.log(TraceCategory::machine, "elsc: address {:#04x} not modeled", address);
    }
    return false;
}

bool Elsc::i2c_write(std::uint8_t byte) {
    if (!receiving_command_ || command_length_ == command_.size()) {
        return false;
    }
    command_[command_length_++] = byte;
    return true;
}

std::uint8_t Elsc::i2c_read() {
    return answer_position_ < answer_length_ ? answer_[answer_position_++] : std::uint8_t{0xff};
}

void Elsc::i2c_stop() {
    receiving_command_ = false;
    answer_length_ = 0;
    answer_position_ = 0;
}

void Elsc::save_state(StateImage& image) const {
    image.put("elsc.token_index", token_index_);
    image.put("elsc.command", command_);
    image.put("elsc.command_length", command_length_);
    image.put("elsc.receiving_command", receiving_command_);
    image.put("elsc.answer", answer_);
    image.put("elsc.answer_length", answer_length_);
    image.put("elsc.answer_position", answer_position_);
    image.put("elsc.message", message_);
    image.put("elsc.message_length", message_length_);
    image.put("elsc.message_waiting", message_waiting_);
    image.put("elsc.powered_off", powered_off_);
}

void Elsc::load_state(const StateImage& image) {
    image.get("elsc.token_index", token_index_);
    image.get("elsc.command", command_);
    image.get("elsc.command_length", command_length_);
    image.get("elsc.receiving_command", receiving_command_);
    image.get("elsc.answer", answer_);
    image.get("elsc.answer_length", answer_length_);
    image.get("elsc.answer_position", answer_position_);
    image.get("elsc.message", message_);
    image.get("elsc.message_length", message_length_);
    image.get("elsc.message_waiting", message_waiting_);
    image.get("elsc.powered_off", powered_off_);
    // A snapshot from before the controller was modeled has no token pending.
    if (!powered_off_ && !scheduler_.is_pending(token_event_)) {
        scheduler_.schedule_after(token_event_, token_period);
    }
}

} // namespace ultraviolent::devices
