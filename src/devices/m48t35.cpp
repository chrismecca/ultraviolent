#include <ultraviolent/devices/m48t35.hpp>

#include <ultraviolent/core/invariant.hpp>

#include <algorithm>
#include <chrono>

namespace ultraviolent::devices {

namespace {

constexpr std::uint8_t control_write = 0x80;
constexpr std::uint8_t control_read = 0x40;
constexpr std::uint8_t seconds_stop = 0x80;
constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;

std::uint8_t to_bcd(unsigned value) {
    return static_cast<std::uint8_t>((value / 10) << 4 | (value % 10));
}

unsigned from_bcd(std::uint8_t value) {
    return (value >> 4) * 10u + (value & 0xfu);
}

} // namespace

M48t35::M48t35(Scheduler& scheduler, std::int64_t epoch, int year_base)
    : scheduler_{scheduler}, base_seconds_{epoch}, year_base_{year_base} {}

std::int64_t M48t35::elapsed_seconds() const {
    return static_cast<std::int64_t>(scheduler_.now().nanoseconds) / nanoseconds_per_second;
}

std::int64_t M48t35::now_seconds() const {
    return base_seconds_ + elapsed_seconds();
}

void M48t35::latch(std::int64_t seconds) {
    using namespace std::chrono;
    const sys_seconds time{std::chrono::seconds{seconds}};
    const auto day = floor<days>(time);
    const year_month_day date{day};
    const hh_mm_ss clock_time{time - day};
    const weekday week{day};
    memory_[clock + 1] =
        static_cast<std::uint8_t>((memory_[clock + 1] & seconds_stop) |
                                  to_bcd(static_cast<unsigned>(clock_time.seconds().count())));
    memory_[clock + 2] = to_bcd(static_cast<unsigned>(clock_time.minutes().count()));
    // hypothesis: bits 7:6 of hours (century enable and bit) stay as software wrote them.
    memory_[clock + 3] = static_cast<std::uint8_t>(
        (memory_[clock + 3] & 0xc0) | to_bcd(static_cast<unsigned>(clock_time.hours().count())));
    memory_[clock + 4] = static_cast<std::uint8_t>(week.iso_encoding()); // 1-7
    memory_[clock + 5] = to_bcd(static_cast<unsigned>(date.day()));
    memory_[clock + 6] = to_bcd(static_cast<unsigned>(date.month()));
    // Years outside the register's century wrap.
    memory_[clock + 7] = to_bcd(
        static_cast<unsigned>(((static_cast<int>(date.year()) - year_base_) % 100 + 100) % 100));
}

std::optional<std::int64_t> M48t35::registers_seconds() const {
    using namespace std::chrono;
    const int year = year_base_ + static_cast<int>(from_bcd(memory_[clock + 7]));
    const year_month_day date{std::chrono::year{year}, month{from_bcd(memory_[clock + 6])},
                              std::chrono::day{from_bcd(memory_[clock + 5])}};
    const unsigned hours = from_bcd(memory_[clock + 3] & 0x3f);
    const unsigned minutes = from_bcd(memory_[clock + 2] & 0x7f);
    const unsigned seconds = from_bcd(memory_[clock + 1] & 0x7f);
    if (!date.ok() || hours > 23 || minutes > 59 || seconds > 59) {
        return std::nullopt;
    }
    const sys_days day{date};
    return day.time_since_epoch().count() * std::int64_t{86'400} + std::int64_t{hours} * 3600 +
           std::int64_t{minutes} * 60 + std::int64_t{seconds};
}

std::span<const std::uint8_t> M48t35::contents() {
    if (!held_) {
        latch(now_seconds());
    }
    return memory_;
}

void M48t35::load_contents(std::span<const std::uint8_t> bytes, bool resume_clock) {
    invariant(bytes.size() == size, "M48T35 contents are 32 KB");
    std::ranges::copy(bytes, memory_.begin());
    held_ = false;
    memory_[clock] &= static_cast<std::uint8_t>(~(control_write | control_read));
    if (const auto seconds = registers_seconds(); resume_clock && seconds) {
        base_seconds_ = *seconds - elapsed_seconds();
    }
}

std::uint8_t M48t35::read(std::uint32_t offset) {
    offset %= size;
    if (offset > clock && !held_) {
        latch(now_seconds());
    }
    return memory_[offset];
}

void M48t35::write(std::uint32_t offset, std::uint8_t value) {
    offset %= size;
    if (offset != clock) {
        memory_[offset] = value;
        return;
    }
    const bool was_writing = (memory_[clock] & control_write) != 0;
    const bool hold = (value & (control_write | control_read)) != 0;
    if (hold && !held_) {
        latch(now_seconds());
    }
    if (was_writing && (value & control_write) == 0) {
        // The clock restarts from the values software loaded. hypothesis: an invalid time
        // leaves the clock running as it was.
        if (const auto seconds = registers_seconds()) {
            base_seconds_ = *seconds - elapsed_seconds();
        }
    }
    held_ = hold;
    memory_[clock] = value;
}

void M48t35::save_state(StateImage& image) const {
    image.put_bytes("m48t35.memory", std::as_bytes(std::span{memory_}));
    image.put("m48t35.base_seconds", base_seconds_);
    image.put("m48t35.held", held_);
}

void M48t35::load_state(const StateImage& image) {
    if (const auto bytes = image.get_bytes("m48t35.memory"); bytes && bytes->size() == size) {
        for (std::size_t i = 0; i < size; ++i) {
            memory_[i] = std::to_integer<std::uint8_t>((*bytes)[i]);
        }
    }
    image.get("m48t35.base_seconds", base_seconds_);
    image.get("m48t35.held", held_);
}

} // namespace ultraviolent::devices
