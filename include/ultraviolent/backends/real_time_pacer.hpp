#pragma once

#include <ultraviolent/core/virtual_time.hpp>

#include <chrono>

namespace ultraviolent::backends {

// Paces a run to the host's clock for interactive use (ARCHITECTURE.adoc: wall clock may pace
// a guest, never define its correctness). Virtual time still moves only by execution; the
// pacer only makes the host wait while virtual time is ahead of the host time elapsed since
// the start, so an idle guest neither outruns a person nor keeps a host core busy. Waiting
// changes nothing the guest sees: a paced run without input is the same run as an unpaced one.
//
// When the host falls behind (a slow stretch such as a boot, or a stopped process), the
// guest runs unpaced until it catches up, but by at most `max_lag`: older lag is forgotten,
// so a long stall is not followed by a long burst. The guest's clock then trails the host's
// by what was forgotten.
class RealTimePacer {
  public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::nanoseconds default_max_lag = std::chrono::seconds{1};

    RealTimePacer(VirtualTime start, Clock::time_point host_start,
                  std::chrono::nanoseconds max_lag = default_max_lag)
        : start_{start}, host_origin_{host_start}, host_start_{host_start}, max_lag_{max_lag} {}

    // How long the host should wait at virtual time `now`, host time `host_now`.
    [[nodiscard]] std::chrono::nanoseconds wait(VirtualTime now, Clock::time_point host_now) {
        const Clock::time_point due =
            host_start_ + std::chrono::nanoseconds{(now - start_).nanoseconds};
        if (host_now <= due) {
            return due - host_now;
        }
        if (const auto lag = host_now - due; lag > max_lag_) {
            host_start_ += lag - max_lag_;
        }
        return {};
    }

    // Lag forgotten so far: how far the guest's clock has fallen behind the host's.
    [[nodiscard]] std::chrono::nanoseconds forgotten() const {
        return host_start_ - host_origin_;
    }

  private:
    VirtualTime start_;
    Clock::time_point host_origin_;
    Clock::time_point host_start_;
    std::chrono::nanoseconds max_lag_;
};

} // namespace ultraviolent::backends
