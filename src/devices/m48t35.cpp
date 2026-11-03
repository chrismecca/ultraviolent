#include <ultraviolent/devices/m48t35.hpp>

#include <ultraviolent/core/invariant.hpp>

#include <algorithm>
#include <array>
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

// The chip's calendar: a year whose register is a multiple of 4 is a leap year, so every four
// years from register year 0 are 366 + 365 + 365 + 365 days.
constexpr std::int64_t seconds_per_day = 86'400;
constexpr std::int64_t days_per_four_years = 4 * 365 + 1;
constexpr std::array<unsigned, 12> month_days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

std::int64_t floor_divide(std::int64_t a, std::int64_t b) {
    return a / b - (a % b != 0 && (a < 0) != (b < 0) ? 1 : 0);
}

struct ChipDate {
    std::int64_t year; // years since register year 0
    unsigned month;    // 1-12
    unsigned day;      // 1-31
    std::int64_t seconds_of_day;
};

// Days of the chip's calendar before `year` begins.
std::int64_t days_before(std::int64_t year) {
    // Leap years before `year`: the multiples of 4 in [0, year).
    return year * 365 + floor_divide(year + 3, 4);
}

// Seconds since register year 0 began. A day past the end of its month runs on into the
// next, as the chip's counters would from it.
std::int64_t chip_seconds(const ChipDate& date) {
    const bool leap = date.year % 4 == 0;
    std::int64_t days = days_before(date.year) + date.day - 1;
    for (unsigned m = 1; m < date.month; ++m) {
        days += month_days[m - 1] + (m == 2 && leap ? 1 : 0);
    }
    return days * seconds_per_day + date.seconds_of_day;
}

ChipDate chip_date(std::int64_t seconds) {
    std::int64_t days = floor_divide(seconds, seconds_per_day);
    ChipDate date{};
    date.seconds_of_day = seconds - days * seconds_per_day;
    const std::int64_t cycles = floor_divide(days, days_per_four_years);
    days -= cycles * days_per_four_years;
    // The cycle's first year is the leap year.
    std::int64_t year = 0;
    if (days >= 366) {
        year = 1 + (days - 366) / 365;
        days = (days - 366) % 365;
    }
    date.year = cycles * 4 + year;
    const bool leap = year == 0;
    date.month = 1;
    for (;;) {
        const unsigned length = month_days[date.month - 1] + (date.month == 2 && leap ? 1 : 0);
        if (days < length) {
            break;
        }
        days -= length;
        ++date.month;
    }
    date.day = static_cast<unsigned>(days) + 1;
    return date;
}

} // namespace

M48t35::M48t35(Scheduler& scheduler, std::int64_t epoch, int year_base)
    : scheduler_{scheduler}, year_base_{year_base} {
    // The epoch's date in the real calendar goes into the registers, as when the clock is set.
    using namespace std::chrono;
    const sys_seconds time{std::chrono::seconds{epoch}};
    const auto day = floor<days>(time);
    const year_month_day date{day};
    base_seconds_ = chip_seconds({static_cast<int>(date.year()) - year_base,
                                  static_cast<unsigned>(date.month()),
                                  static_cast<unsigned>(date.day()), (time - day).count()});
    // The day of week as ISO numbers it (Monday 1).
    set_weekday(weekday{day}.iso_encoding(), base_seconds_);
}

void M48t35::set_weekday(unsigned weekday, std::int64_t seconds) {
    weekday_offset_ =
        (static_cast<std::int64_t>(weekday) - 1 - floor_divide(seconds, seconds_per_day) % 7 + 14) %
        7;
}

std::int64_t M48t35::elapsed_seconds() const {
    return static_cast<std::int64_t>(scheduler_.now().nanoseconds) / nanoseconds_per_second;
}

std::int64_t M48t35::now_seconds() const {
    return base_seconds_ + elapsed_seconds();
}

void M48t35::latch(std::int64_t seconds) {
    const ChipDate date = chip_date(seconds);
    const auto time = static_cast<unsigned>(date.seconds_of_day);
    memory_[clock + 1] = static_cast<std::uint8_t>((memory_[clock + 1] & seconds_stop) |
                                                   to_bcd(time % 60));
    memory_[clock + 2] = to_bcd(time / 60 % 60);
    // hypothesis: bits 7:6 of hours (century enable and bit) stay as software wrote them.
    memory_[clock + 3] =
        static_cast<std::uint8_t>((memory_[clock + 3] & 0xc0) | to_bcd(time / 3600));
    // The day of week is a counter of its own, 1-7, that steps with the days from the value
    // it was set to.
    memory_[clock + 4] = static_cast<std::uint8_t>(
        (floor_divide(seconds, seconds_per_day) + weekday_offset_) % 7 + 1);
    memory_[clock + 5] = to_bcd(date.day);
    memory_[clock + 6] = to_bcd(date.month);
    // Years outside the register's century wrap.
    memory_[clock + 7] = to_bcd(static_cast<unsigned>((date.year % 100 + 100) % 100));
}

std::optional<std::int64_t> M48t35::registers_seconds() const {
    const unsigned year = from_bcd(memory_[clock + 7]);
    const unsigned month = from_bcd(memory_[clock + 6]);
    const unsigned day = from_bcd(memory_[clock + 5]);
    const unsigned hours = from_bcd(memory_[clock + 3] & 0x3f);
    const unsigned minutes = from_bcd(memory_[clock + 2] & 0x7f);
    const unsigned seconds = from_bcd(memory_[clock + 1] & 0x7f);
    if (year > 99 || month < 1 || month > 12 || day < 1 || day > 31 || hours > 23 ||
        minutes > 59 || seconds > 59) {
        return std::nullopt;
    }
    return chip_seconds({year, month, day, std::int64_t{hours} * 3600 + minutes * 60 + seconds});
}

std::span<const std::uint8_t> M48t35::contents() {
    if (!held_) {
        latch(now_seconds());
    }
    return memory_;
}

void M48t35::load_contents(std::span<const std::uint8_t> bytes, bool resume_clock,
                           std::int64_t seconds_off) {
    invariant(bytes.size() == size, "M48T35 contents are 32 KB");
    std::ranges::copy(bytes, memory_.begin());
    held_ = false;
    memory_[clock] &= static_cast<std::uint8_t>(~(control_write | control_read));
    if (const auto seconds = registers_seconds(); resume_clock && seconds) {
        base_seconds_ = *seconds + seconds_off - elapsed_seconds();
        set_weekday(memory_[clock + 4] & 7u, *seconds);
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
            set_weekday(memory_[clock + 4] & 7u, *seconds);
        }
    }
    held_ = hold;
    memory_[clock] = value;
}

void M48t35::save_state(StateImage& image) const {
    image.put_bytes("m48t35.memory", std::as_bytes(std::span{memory_}));
    image.put("m48t35.base_seconds", base_seconds_);
    image.put("m48t35.held", held_);
    image.put("m48t35.weekday_offset", weekday_offset_);
}

void M48t35::load_state(const StateImage& image) {
    if (const auto bytes = image.get_bytes("m48t35.memory"); bytes && bytes->size() == size) {
        for (std::size_t i = 0; i < size; ++i) {
            memory_[i] = std::to_integer<std::uint8_t>((*bytes)[i]);
        }
    }
    image.get("m48t35.base_seconds", base_seconds_);
    image.get("m48t35.held", held_);
    // Snapshots from before the counter: the weekday as the real calendar has it while the
    // two calendars agree (1970-01-01, day 0, was a Thursday).
    weekday_offset_ = 3;
    image.get("m48t35.weekday_offset", weekday_offset_);
}

} // namespace ultraviolent::devices
