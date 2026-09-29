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
// software agrees on, `year_base` (register 00 is that year).
//
// Time is virtual: the clock reads `epoch` seconds (Unix time) at virtual time zero, plus
// elapsed virtual time. READ freezes the registers for reading; WRITE stops updates while
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
    // from the time in its registers, if they hold a valid one: no virtual time passes while
    // the machine is off.
    void load_contents(std::span<const std::uint8_t> bytes, bool resume_clock);

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    static constexpr std::uint32_t clock = 0x7ff8;
    // Whole seconds of virtual time since power-on.
    [[nodiscard]] std::int64_t elapsed_seconds() const;
    // The running time in seconds (Unix time).
    [[nodiscard]] std::int64_t now_seconds() const;
    // Loads the clock registers from `seconds`.
    void latch(std::int64_t seconds);
    // The time the clock registers hold, or none when they do not hold a valid time.
    [[nodiscard]] std::optional<std::int64_t> registers_seconds() const;

    Scheduler& scheduler_;
    std::vector<std::uint8_t> memory_ = std::vector<std::uint8_t>(size);
    // Unix time at virtual time zero; the clock runs from it.
    std::int64_t base_seconds_;
    int year_base_;
    // Whether READ or WRITE holds the registers.
    bool held_{};
};

} // namespace ultraviolent::devices
