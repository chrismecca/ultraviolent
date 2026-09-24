#include "support/test.hpp"

#include <ultraviolent/core/virtual_time.hpp>

#include <cstdint>

namespace {

using namespace ultraviolent;

const test::Registration duration_units{
    "virtual_time.duration_units", [](test::Context& t) {
        t.check_equal(VirtualDuration::from_microseconds(3).nanoseconds, std::uint64_t{3'000});
        t.check_equal(VirtualDuration::from_milliseconds(3).nanoseconds, std::uint64_t{3'000'000});
        t.check_equal(VirtualDuration::from_seconds(3).nanoseconds, std::uint64_t{3'000'000'000});
        t.check_equal((VirtualDuration{5} + VirtualDuration{7}).nanoseconds, std::uint64_t{12});
    }};

const test::Registration time_arithmetic{
    "virtual_time.arithmetic", [](test::Context& t) {
        constexpr VirtualTime start{100};
        constexpr VirtualTime later = start + VirtualDuration{50};
        static_assert(later.nanoseconds == 150);
        static_assert((later - start).nanoseconds == 50);
        t.check(start < later);
        t.check(VirtualTime{} == VirtualTime{0}, "time starts at zero");
    }};

// 195 MHz does not divide 10^9, so rounding in both directions is exercised.
constexpr Frequency cpu_clock{195'000'000};

const test::Registration cycles_at_whole_seconds{
    "virtual_time.cycles_at_counts_whole_cycles", [](test::Context& t) {
        t.check_equal(cycles_at(VirtualTime{0}, cpu_clock), std::uint64_t{0});
        t.check_equal(cycles_at(VirtualTime{nanoseconds_per_second}, cpu_clock),
                      std::uint64_t{195'000'000});
        // One cycle is 5.128... ns: 5 ns is not yet a cycle, 6 ns is.
        t.check_equal(cycles_at(VirtualTime{5}, cpu_clock), std::uint64_t{0});
        t.check_equal(cycles_at(VirtualTime{6}, cpu_clock), std::uint64_t{1});
    }};

const test::Registration time_at_cycles_is_earliest{
    "virtual_time.time_at_cycles_is_earliest_time", [](test::Context& t) {
        for (const Frequency clock :
             {cpu_clock, Frequency{1}, Frequency{3}, Frequency{1'250'000},
              Frequency{nanoseconds_per_second}, Frequency{maximum_frequency_hertz}}) {
            for (std::uint64_t cycles = 1; cycles < 2'000; ++cycles) {
                const VirtualTime when = time_at_cycles(cycles, clock);
                if (!t.check(cycles_at(when, clock) >= cycles, "counter reached at deadline") ||
                    !t.check(cycles_at(VirtualTime{when.nanoseconds - 1}, clock) < cycles,
                             "counter not reached one nanosecond earlier")) {
                    return;
                }
            }
        }
    }};

const test::Registration conversions_do_not_drift{
    "virtual_time.conversions_do_not_drift", [](test::Context& t) {
        // After a simulated year the cycle count is still exact.
        constexpr std::uint64_t seconds_per_year = 365ull * 24 * 60 * 60;
        const VirtualTime year{seconds_per_year * nanoseconds_per_second};
        t.check_equal(cycles_at(year, cpu_clock), seconds_per_year * 195'000'000);
        t.check_equal(time_at_cycles(seconds_per_year * 195'000'000, cpu_clock), year);
    }};

} // namespace
