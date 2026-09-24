#pragma once

#include <ultraviolent/core/invariant.hpp>
#include <ultraviolent/core/virtual_time.hpp>

namespace ultraviolent {

class Scheduler;

// The machine's single source of current virtual time. Any component may read it; only the
// Scheduler advances it, so time moves only as events and execution progress are accounted.
class VirtualClock {
  public:
    VirtualClock() = default;
    VirtualClock(const VirtualClock&) = delete;
    VirtualClock& operator=(const VirtualClock&) = delete;

    [[nodiscard]] VirtualTime now() const {
        return now_;
    }

  private:
    friend class Scheduler;

    void advance_to(VirtualTime time) {
        invariant(time >= now_, "virtual time must not move backwards");
        now_ = time;
    }

    VirtualTime now_{};
};

} // namespace ultraviolent
