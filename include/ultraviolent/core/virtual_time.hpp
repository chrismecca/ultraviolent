#pragma once

#include <ultraviolent/core/invariant.hpp>

#include <compare>
#include <cstdint>
#include <limits>

namespace ultraviolent {

// Virtual time is the only guest-visible time base (ADR-005). It counts nanoseconds since
// machine power-on and moves only when the Scheduler advances it, never because host time
// passed. Unsigned 64-bit nanoseconds cover about 584 years (ADR-015).

inline constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;

namespace detail {

constexpr std::uint64_t checked_add(std::uint64_t a, std::uint64_t b) {
    invariant(a <= std::numeric_limits<std::uint64_t>::max() - b, "virtual time overflow");
    return a + b;
}

constexpr std::uint64_t checked_multiply(std::uint64_t a, std::uint64_t b) {
    invariant(b == 0 || a <= std::numeric_limits<std::uint64_t>::max() / b,
              "virtual time overflow");
    return a * b;
}

} // namespace detail

// A span of virtual time, in nanoseconds.
struct VirtualDuration {
    std::uint64_t nanoseconds{};

    static constexpr VirtualDuration from_microseconds(std::uint64_t count) {
        return {detail::checked_multiply(count, 1'000)};
    }
    static constexpr VirtualDuration from_milliseconds(std::uint64_t count) {
        return {detail::checked_multiply(count, 1'000'000)};
    }
    static constexpr VirtualDuration from_seconds(std::uint64_t count) {
        return {detail::checked_multiply(count, nanoseconds_per_second)};
    }

    auto operator<=>(const VirtualDuration&) const = default;
};

constexpr VirtualDuration operator+(VirtualDuration a, VirtualDuration b) {
    return {detail::checked_add(a.nanoseconds, b.nanoseconds)};
}

// A point in virtual time: nanoseconds since machine power-on.
struct VirtualTime {
    std::uint64_t nanoseconds{};

    auto operator<=>(const VirtualTime&) const = default;
};

constexpr VirtualTime operator+(VirtualTime time, VirtualDuration delay) {
    return {detail::checked_add(time.nanoseconds, delay.nanoseconds)};
}

constexpr VirtualDuration operator-(VirtualTime later, VirtualTime earlier) {
    invariant(later >= earlier, "virtual durations are never negative");
    return {later.nanoseconds - earlier.nanoseconds};
}

// The frequency of a guest-visible clock such as a CPU cycle counter or a real-time counter.
// It describes what the guest observes, not how fast the host executes (ADR-013).
struct Frequency {
    std::uint64_t hertz{};

    auto operator<=>(const Frequency&) const = default;
};

// Upper bound that keeps the conversions below exact in 64-bit arithmetic.
inline constexpr std::uint64_t maximum_frequency_hertz = 10'000'000'000;

// Whole cycles of `clock` completed at `time`, i.e. floor(time * hertz / 10^9).
//
// Guest counters derive their value from absolute virtual time through this function rather
// than accumulating per-step increments, so repeated conversion never drifts.
constexpr std::uint64_t cycles_at(VirtualTime time, Frequency clock) {
    invariant(clock.hertz != 0 && clock.hertz <= maximum_frequency_hertz,
              "clock frequency out of range");
    const std::uint64_t whole_seconds = time.nanoseconds / nanoseconds_per_second;
    const std::uint64_t remainder = time.nanoseconds % nanoseconds_per_second;
    // remainder < 10^9 and hertz <= 10^10, so the product fits in 64 bits.
    return detail::checked_add(detail::checked_multiply(whole_seconds, clock.hertz),
                               remainder * clock.hertz / nanoseconds_per_second);
}

// Earliest virtual time at which `cycles` whole cycles of `clock` have completed, i.e.
// ceil(cycles * 10^9 / hertz). This is the deadline to schedule for "counter reaches N".
constexpr VirtualTime time_at_cycles(std::uint64_t cycles, Frequency clock) {
    invariant(clock.hertz != 0 && clock.hertz <= maximum_frequency_hertz,
              "clock frequency out of range");
    const std::uint64_t whole_seconds = cycles / clock.hertz;
    const std::uint64_t remainder = cycles % clock.hertz;
    // remainder < hertz <= 10^10, so remainder * 10^9 + hertz fits in 64 bits.
    const std::uint64_t fraction =
        (remainder * nanoseconds_per_second + clock.hertz - 1) / clock.hertz;
    return {detail::checked_add(detail::checked_multiply(whole_seconds, nanoseconds_per_second),
                                fraction)};
}

} // namespace ultraviolent
