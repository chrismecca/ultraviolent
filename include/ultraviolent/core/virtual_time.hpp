#pragma once

#include <ultraviolent/core/invariant.hpp>

#include <bit>
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

namespace detail {

// Exact unsigned 64-bit division by a divisor fixed when the object is made, by a multiply
// and shifts (Granlund and Montgomery, "Division by Invariant Integers using
// Multiplication", PLDI 1994, figure 4.1): divide(n) == n / d for every n. A hardware
// division costs tens of host cycles; time conversions run on every synchronized device
// access.
class InvariantDivisor {
  public:
    constexpr explicit InvariantDivisor(std::uint64_t d) : divisor_{d} {
        invariant(d != 0 && d <= std::uint64_t{1} << 62, "divisor out of range");
        if (std::has_single_bit(d)) {
            shift_ = static_cast<unsigned>(std::countr_zero(d));
            return;
        }
        // l = ceil(log2 d), 2 <= l <= 62; m = floor(2^(64+l) / d) - 2^64 + 1 < 2^64.
        const auto l = static_cast<unsigned>(64 - std::countl_zero(d - 1));
        __extension__ using u128 = unsigned __int128;
        multiplier_ = static_cast<std::uint64_t>((u128{1} << (64 + l)) / d - (u128{1} << 64) + 1);
        shift_ = l - 1;
    }

    [[nodiscard]] constexpr std::uint64_t divide(std::uint64_t n) const {
        if (multiplier_ == 0) {
            return n >> shift_;
        }
        __extension__ using u128 = unsigned __int128;
        const auto t = static_cast<std::uint64_t>((u128{multiplier_} * n) >> 64);
        return (t + ((n - t) >> 1)) >> shift_;
    }
    [[nodiscard]] constexpr std::uint64_t divisor() const {
        return divisor_;
    }

  private:
    std::uint64_t divisor_;
    std::uint64_t multiplier_{};
    unsigned shift_{};
};

} // namespace detail

// time_at_cycles for one clock, without a hardware division: the same results, for the paths
// that convert on every device access.
class CycleTime {
  public:
    constexpr explicit CycleTime(Frequency clock) : hertz_{check(clock)} {}

    [[nodiscard]] constexpr VirtualTime time_at(std::uint64_t cycles) const {
        const std::uint64_t hertz = hertz_.divisor();
        const std::uint64_t whole_seconds = hertz_.divide(cycles);
        const std::uint64_t remainder = cycles - whole_seconds * hertz;
        const std::uint64_t fraction =
            hertz_.divide(remainder * nanoseconds_per_second + hertz - 1);
        return {detail::checked_add(detail::checked_multiply(whole_seconds, nanoseconds_per_second),
                                    fraction)};
    }

  private:
    static constexpr std::uint64_t check(Frequency clock) {
        invariant(clock.hertz != 0 && clock.hertz <= maximum_frequency_hertz,
                  "clock frequency out of range");
        return clock.hertz;
    }
    detail::InvariantDivisor hertz_;
};

} // namespace ultraviolent
