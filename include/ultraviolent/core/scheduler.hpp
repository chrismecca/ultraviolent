#pragma once

#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/core/virtual_time.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ultraviolent {

// Handle to an event registered with a Scheduler.
struct EventId {
    std::uint32_t index{};

    auto operator<=>(const EventId&) const = default;
};

// Deterministic virtual-time event scheduler.
//
// Components register named events once, then schedule and cancel them as their hardware
// state changes. Each event is either idle or pending at exactly one deadline; a component
// with several outstanding operations registers several events or keeps its own queue.
//
// Dispatch order is total and independent of host behavior: earlier deadline first, and for
// equal deadlines the event that was (re)scheduled first. Rescheduling a pending event
// replaces its deadline and its place in that order.
class Scheduler {
  public:
    using Callback = std::function<void()>;

    Scheduler(VirtualClock& clock, Tracer& tracer) : clock_{clock}, tracer_{tracer} {}
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    [[nodiscard]] VirtualTime now() const {
        return clock_.now();
    }

    // Registers an event. `name` identifies it in traces. The callback runs with the clock at
    // the event's deadline. Registration is permanent, so whatever the callback refers to must
    // outlive dispatch, as components owned alongside the scheduler do.
    EventId add_event(std::string name, Callback callback);

    void schedule_at(EventId event, VirtualTime deadline);
    void schedule_after(EventId event, VirtualDuration delay);
    void cancel(EventId event);

    [[nodiscard]] bool is_pending(EventId event) const;
    [[nodiscard]] std::optional<VirtualTime> deadline(EventId event) const;
    [[nodiscard]] std::optional<VirtualTime> next_deadline() const;

    // Fires, in dispatch order, every event whose deadline is at or before `limit`, including
    // events that callbacks schedule inside the window, then leaves the clock at `limit`.
    // Returns the number of events fired. Not reentrant: callbacks must not advance time.
    std::size_t advance_to(VirtualTime limit);
    std::size_t advance_by(VirtualDuration delay);

    // Snapshot support (StateImage): the current time and every pending event, keyed by event
    // name. Loading requires a scheduler at time zero with the same events registered; the
    // snapshot's pending events replace any already scheduled (for example by components
    // that start running at power-on).
    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    static constexpr std::size_t not_pending = std::numeric_limits<std::size_t>::max();

    struct Event {
        std::string name;
        Callback callback;
        VirtualTime deadline{};
        std::uint64_t sequence{};
        std::size_t heap_position{not_pending};
    };

    [[nodiscard]] const Event& event_at(EventId event) const;
    [[nodiscard]] Event& event_at(EventId event);
    [[nodiscard]] bool dispatches_before(std::uint32_t a, std::uint32_t b) const;
    void place(std::size_t position, std::uint32_t index);
    void sift_up(std::size_t position);
    void sift_down(std::size_t position);
    void remove_from_heap(std::uint32_t index);

    VirtualClock& clock_;
    Tracer& tracer_;
    // A deque keeps each Event at a stable address, so a callback may register new events
    // while its own Event is executing.
    std::deque<Event> events_;
    // Binary min-heap of indices into events_, ordered by (deadline, sequence).
    std::vector<std::uint32_t> heap_;
    std::uint64_t next_sequence_{};
    bool dispatching_{};
};

} // namespace ultraviolent
