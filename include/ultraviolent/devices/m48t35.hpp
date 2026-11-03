#pragma once

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ultraviolent::devices {

// ST M48T35 TIMEKEEPER: 32 KB of battery-backed SRAM whose top eight bytes are a BCD clock
// (IP27.adoc "IOC3"; Linux drivers/rtc/rtc-m48t35.c): 0x7ff8 control (bit 7 WRITE, bit 6
// READ, calibration 5:0), 0x7ff9 seconds (bit 7 ST, stop), minutes, hours, day of week,
// date, month, year (00-99).
//
// The chip keeps no century: the year register counts years from a base that the platform's
// software agrees on, `year_base` (register 00 is that year). Nor does it know the base: its
// leap years are those whose register is a multiple of 4 (ST M48T35 datasheet, "leap year";
// from a 1970 base those are 1970, 1974, ..., 2026, not the real ones), and it counts days
// by that calendar. Software that counts years from such a base corrects for it (IP27.adoc
// "IOC3").
//
// Time is virtual: the clock reads `epoch` (Unix time, set into the registers as its date
// in the real calendar) at virtual time zero, plus elapsed virtual time, counted on the
// chip's calendar. READ freezes the registers for reading; WRITE stops updates while
// software loads new values, which take effect when WRITE clears.
class M48t35 {
  public:
    static constexpr std::size_t size = 0x8000;

    M48t35(Scheduler& scheduler, std::int64_t epoch, int year_base);

    std::uint8_t read(std::uint32_t offset);
    void write(std::uint32_t offset, std::uint8_t value);

    // Battery-backed contents, for host persistence (IP27.adoc "IOC3"). `contents` latches the
    // running time into the clock registers first, as the chip holds it at power-off.
    [[nodiscard]] std::span<const std::uint8_t> contents();
    // Replaces the memory with `bytes` (size bytes). With `resume_clock`, the clock continues
    // from the time in its registers, if they hold a valid one, plus `seconds_off`: the time
    // it ran on its battery while the machine was off (0 unless the host says otherwise; no
    // virtual time passes then).
    void load_contents(std::span<const std::uint8_t> bytes, bool resume_clock,
                       std::int64_t seconds_off = 0);

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    static constexpr std::uint32_t clock = 0x7ff8;
    // Whole seconds of virtual time since power-on.
    [[nodiscard]] std::int64_t elapsed_seconds() const;
    // The running time: seconds on the chip's calendar since register year 0 began.
    [[nodiscard]] std::int64_t now_seconds() const;
    // Loads the clock registers from `seconds` (chip calendar).
    void latch(std::int64_t seconds);
    // The time the clock registers hold (chip calendar), or none when they do not hold a
    // valid time.
    [[nodiscard]] std::optional<std::int64_t> registers_seconds() const;
    // Sets the day-of-week counter to `weekday` (1-7) at time `seconds`.
    void set_weekday(unsigned weekday, std::int64_t seconds);

    Scheduler& scheduler_;
    std::vector<std::uint8_t> memory_ = std::vector<std::uint8_t>(size);
    // The time at virtual time zero (as now_seconds); the clock runs from it.
    std::int64_t base_seconds_;
    int year_base_;
    // The day-of-week register is (day number + weekday_offset_) % 7 + 1.
    std::int64_t weekday_offset_{};
    // Whether READ or WRITE holds the registers.
    bool held_{};
};

} // namespace ultraviolent::devices
