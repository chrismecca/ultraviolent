#include "support/test.hpp"

#include <ultraviolent/backends/real_time_pacer.hpp>

#include <chrono>

namespace {

using namespace ultraviolent;
using namespace std::chrono_literals;
using Pacer = backends::RealTimePacer;

// Host times as offsets from an arbitrary origin; virtual times in milliseconds.
const Pacer::Clock::time_point origin{1000s};

Pacer::Clock::time_point host(std::chrono::nanoseconds offset) {
    return origin + offset;
}

VirtualTime at(std::uint64_t milliseconds) {
    return VirtualTime{} + VirtualDuration::from_milliseconds(milliseconds);
}

const test::Registration ahead{"real_time_pacer.waits_while_ahead", [](test::Context& t) {
    // Started at virtual 5 s: only the time since the start counts.
    Pacer pacer{at(5000), host(0s)};
    t.check_equal(pacer.wait(at(5000), host(0s)), 0ns);
    t.check_equal(pacer.wait(at(5010), host(0s)), 10ms);
    t.check_equal(pacer.wait(at(5010), host(4ms)), 6ms);
    t.check_equal(pacer.wait(at(5010), host(10ms)), 0ns);
    t.check_equal(pacer.forgotten(), 0ns);
}};

const test::Registration behind{"real_time_pacer.catches_up_within_the_lag",
                                [](test::Context& t) {
    Pacer pacer{at(0), host(0s), 1s};
    // 600 ms behind: no wait, and nothing forgotten.
    t.check_equal(pacer.wait(at(400), host(1000ms)), 0ns);
    t.check_equal(pacer.forgotten(), 0ns);
    // Exactly the lag: still nothing forgotten.
    t.check_equal(pacer.wait(at(500), host(1500ms)), 0ns);
    t.check_equal(pacer.forgotten(), 0ns);
    // The guest catches up unpaced, then waits again against the original start.
    t.check_equal(pacer.wait(at(1500), host(1500ms)), 0ns);
    t.check_equal(pacer.wait(at(1520), host(1510ms)), 10ms);
}};

const test::Registration forgets{"real_time_pacer.forgets_lag_beyond_the_bound",
                                 [](test::Context& t) {
    Pacer pacer{at(0), host(0s), 1s};
    // 9 s behind (a stopped process): 8 s is forgotten, 1 s remains to catch up.
    t.check_equal(pacer.wait(at(1000), host(10s)), 0ns);
    t.check_equal(pacer.forgotten(), 8s);
    t.check_equal(pacer.wait(at(1999), host(10s)), 0ns);
    t.check_equal(pacer.wait(at(2000), host(10s)), 0ns);
    t.check_equal(pacer.wait(at(2050), host(10s)), 50ms);
    // Forgetting accumulates.
    t.check_equal(pacer.wait(at(2050), host(12050ms)), 0ns);
    t.check_equal(pacer.forgotten(), 9s);
}};

} // namespace
