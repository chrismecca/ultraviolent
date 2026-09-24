#include <ultraviolent/core/scheduler.hpp>

#include <utility>

namespace ultraviolent {

EventId Scheduler::add_event(std::string name, Callback callback) {
    invariant(static_cast<bool>(callback), "an event needs a callback");
    invariant(events_.size() < std::numeric_limits<std::uint32_t>::max(), "too many events");
    const auto index = static_cast<std::uint32_t>(events_.size());
    events_.push_back({.name = std::move(name), .callback = std::move(callback)});
    return EventId{index};
}

void Scheduler::schedule_at(EventId event, VirtualTime deadline) {
    invariant(deadline >= now(), "an event cannot be scheduled in the past");
    Event& target = event_at(event);
    if (target.heap_position != not_pending) {
        remove_from_heap(event.index);
    }
    target.deadline = deadline;
    target.sequence = next_sequence_++;
    heap_.push_back(event.index);
    target.heap_position = heap_.size() - 1;
    sift_up(target.heap_position);
}

void Scheduler::schedule_after(EventId event, VirtualDuration delay) {
    schedule_at(event, now() + delay);
}

void Scheduler::cancel(EventId event) {
    if (event_at(event).heap_position != not_pending) {
        remove_from_heap(event.index);
    }
}

bool Scheduler::is_pending(EventId event) const {
    return event_at(event).heap_position != not_pending;
}

std::optional<VirtualTime> Scheduler::deadline(EventId event) const {
    const Event& target = event_at(event);
    if (target.heap_position == not_pending) {
        return std::nullopt;
    }
    return target.deadline;
}

std::optional<VirtualTime> Scheduler::next_deadline() const {
    if (heap_.empty()) {
        return std::nullopt;
    }
    return events_[heap_.front()].deadline;
}

std::size_t Scheduler::advance_to(VirtualTime limit) {
    invariant(!dispatching_, "Scheduler::advance_to is not reentrant");
    invariant(limit >= now(), "virtual time must not move backwards");

    struct DispatchGuard {
        bool& flag;
        explicit DispatchGuard(bool& f) : flag{f} {
            flag = true;
        }
        DispatchGuard(const DispatchGuard&) = delete;
        DispatchGuard& operator=(const DispatchGuard&) = delete;
        ~DispatchGuard() {
            flag = false;
        }
    } guard{dispatching_};

    std::size_t fired = 0;
    while (!heap_.empty() && events_[heap_.front()].deadline <= limit) {
        const std::uint32_t index = heap_.front();
        Event& event = events_[index];
        // Idle before the callback runs, so the callback may reschedule its own event.
        remove_from_heap(index);
        clock_.advance_to(event.deadline);
        tracer_.log(TraceCategory::scheduler, "fire {}", event.name);
        event.callback();
        ++fired;
    }
    clock_.advance_to(limit);
    return fired;
}

std::size_t Scheduler::advance_by(VirtualDuration delay) {
    return advance_to(now() + delay);
}

const Scheduler::Event& Scheduler::event_at(EventId event) const {
    invariant(event.index < events_.size(), "unknown event");
    return events_[event.index];
}

Scheduler::Event& Scheduler::event_at(EventId event) {
    invariant(event.index < events_.size(), "unknown event");
    return events_[event.index];
}

bool Scheduler::dispatches_before(std::uint32_t a, std::uint32_t b) const {
    const Event& first = events_[a];
    const Event& second = events_[b];
    if (first.deadline != second.deadline) {
        return first.deadline < second.deadline;
    }
    return first.sequence < second.sequence;
}

void Scheduler::place(std::size_t position, std::uint32_t index) {
    heap_[position] = index;
    events_[index].heap_position = position;
}

void Scheduler::sift_up(std::size_t position) {
    const std::uint32_t index = heap_[position];
    while (position > 0) {
        const std::size_t parent = (position - 1) / 2;
        if (!dispatches_before(index, heap_[parent])) {
            break;
        }
        place(position, heap_[parent]);
        position = parent;
    }
    place(position, index);
}

void Scheduler::sift_down(std::size_t position) {
    const std::uint32_t index = heap_[position];
    const std::size_t count = heap_.size();
    while (true) {
        std::size_t child = 2 * position + 1;
        if (child >= count) {
            break;
        }
        if (child + 1 < count && dispatches_before(heap_[child + 1], heap_[child])) {
            ++child;
        }
        if (!dispatches_before(heap_[child], index)) {
            break;
        }
        place(position, heap_[child]);
        position = child;
    }
    place(position, index);
}

void Scheduler::remove_from_heap(std::uint32_t index) {
    const std::size_t position = events_[index].heap_position;
    const std::uint32_t last = heap_.back();
    heap_.pop_back();
    events_[index].heap_position = not_pending;
    if (position == heap_.size()) {
        return;
    }
    place(position, last);
    sift_up(position);
    sift_down(events_[last].heap_position);
}

} // namespace ultraviolent
