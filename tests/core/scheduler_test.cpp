#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace ultraviolent;

// A clock, tracer, and scheduler wired the way a machine owns them.
struct Harness {
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
};

struct Fired {
    std::string name;
    std::uint64_t time;

    bool operator==(const Fired&) const = default;
};

const test::Registration fires_in_deadline_order{
    "scheduler.fires_in_deadline_order", [](test::Context& t) {
        Harness h;
        std::vector<Fired> fired;
        auto record = [&](const std::string& name) {
            return h.scheduler.add_event(name, [&fired, &h, name] {
                fired.push_back({name, h.scheduler.now().nanoseconds});
            });
        };
        const EventId late = record("late");
        const EventId early = record("early");
        const EventId middle = record("middle");
        h.scheduler.schedule_at(late, VirtualTime{300});
        h.scheduler.schedule_at(early, VirtualTime{100});
        h.scheduler.schedule_at(middle, VirtualTime{200});

        t.check_equal(h.scheduler.advance_to(VirtualTime{1'000}), std::size_t{3});
        t.check(fired == std::vector<Fired>{{"early", 100}, {"middle", 200}, {"late", 300}},
                "each callback runs at its own deadline, in order");
        t.check_equal(h.scheduler.now().nanoseconds, std::uint64_t{1'000});
    }};

const test::Registration equal_deadlines_fire_in_schedule_order{
    "scheduler.equal_deadlines_fire_in_schedule_order", [](test::Context& t) {
        Harness h;
        std::vector<int> fired;
        std::vector<EventId> events;
        events.reserve(8);
        for (int i = 0; i < 8; ++i) {
            events.push_back(h.scheduler.add_event("e", [&fired, i] { fired.push_back(i); }));
        }
        // Scheduled in an order unrelated to registration order.
        for (int i : {5, 2, 7, 0, 3, 6, 1, 4}) {
            h.scheduler.schedule_at(events[static_cast<std::size_t>(i)], VirtualTime{10});
        }
        h.scheduler.advance_to(VirtualTime{10});
        t.check(fired == std::vector<int>{5, 2, 7, 0, 3, 6, 1, 4});
    }};

const test::Registration reschedule_replaces_deadline{
    "scheduler.reschedule_replaces_deadline_and_order", [](test::Context& t) {
        Harness h;
        std::vector<std::string> fired;
        const EventId a = h.scheduler.add_event("a", [&] { fired.push_back("a"); });
        const EventId b = h.scheduler.add_event("b", [&] { fired.push_back("b"); });
        h.scheduler.schedule_at(a, VirtualTime{10});
        h.scheduler.schedule_at(b, VirtualTime{10});
        // Rescheduling `a` to the same deadline moves it behind `b`.
        h.scheduler.schedule_at(a, VirtualTime{10});
        t.check_equal(h.scheduler.deadline(a), std::optional{VirtualTime{10}});
        h.scheduler.advance_to(VirtualTime{10});
        t.check(fired == std::vector<std::string>{"b", "a"});

        fired.clear();
        h.scheduler.schedule_at(a, VirtualTime{50});
        h.scheduler.schedule_at(a, VirtualTime{20});
        h.scheduler.advance_to(VirtualTime{30});
        t.check(fired == std::vector<std::string>{"a"}, "only the replacement deadline fires");
        t.check_equal(h.scheduler.advance_to(VirtualTime{100}), std::size_t{0});
    }};

const test::Registration cancel_removes_pending_event{
    "scheduler.cancel_removes_pending_event", [](test::Context& t) {
        Harness h;
        int fired = 0;
        const EventId a = h.scheduler.add_event("a", [&] { ++fired; });
        const EventId b = h.scheduler.add_event("b", [&] { ++fired; });
        h.scheduler.schedule_at(a, VirtualTime{10});
        h.scheduler.schedule_at(b, VirtualTime{20});
        t.check(h.scheduler.is_pending(a));
        h.scheduler.cancel(a);
        h.scheduler.cancel(a); // cancelling an idle event is harmless
        t.check(!h.scheduler.is_pending(a));
        t.check_equal(h.scheduler.deadline(a), std::optional<VirtualTime>{});
        t.check_equal(h.scheduler.next_deadline(), std::optional{VirtualTime{20}});
        h.scheduler.advance_to(VirtualTime{100});
        t.check_equal(fired, 1);
        t.check_equal(h.scheduler.next_deadline(), std::optional<VirtualTime>{});
    }};

const test::Registration callbacks_may_schedule_within_window{
    "scheduler.callbacks_may_schedule_within_window", [](test::Context& t) {
        Harness h;
        std::vector<Fired> fired;
        EventId follow_up{};
        const EventId first = h.scheduler.add_event("first", [&] {
            fired.push_back({"first", h.scheduler.now().nanoseconds});
            h.scheduler.schedule_after(follow_up, VirtualDuration{0});
        });
        follow_up = h.scheduler.add_event(
            "follow_up", [&] { fired.push_back({"follow_up", h.scheduler.now().nanoseconds}); });
        h.scheduler.schedule_at(first, VirtualTime{40});
        t.check_equal(h.scheduler.advance_to(VirtualTime{40}), std::size_t{2});
        t.check(fired == std::vector<Fired>{{"first", 40}, {"follow_up", 40}});
    }};

const test::Registration periodic_event{
    "scheduler.periodic_event", [](test::Context& t) {
        Harness h;
        std::vector<std::uint64_t> ticks;
        EventId tick{};
        tick = h.scheduler.add_event("tick", [&] {
            ticks.push_back(h.scheduler.now().nanoseconds);
            h.scheduler.schedule_after(tick, VirtualDuration{25});
        });
        h.scheduler.schedule_at(tick, VirtualTime{25});
        t.check_equal(h.scheduler.advance_by(VirtualDuration{100}), std::size_t{4});
        t.check(ticks == std::vector<std::uint64_t>{25, 50, 75, 100});
        t.check_equal(h.scheduler.deadline(tick), std::optional{VirtualTime{125}});
    }};

const test::Registration events_beyond_limit_wait{
    "scheduler.events_beyond_limit_wait", [](test::Context& t) {
        Harness h;
        int fired = 0;
        const EventId a = h.scheduler.add_event("a", [&] { ++fired; });
        h.scheduler.schedule_at(a, VirtualTime{101});
        t.check_equal(h.scheduler.advance_to(VirtualTime{100}), std::size_t{0});
        t.check_equal(h.scheduler.now().nanoseconds, std::uint64_t{100});
        t.check_equal(h.scheduler.advance_to(VirtualTime{101}), std::size_t{1});
        t.check_equal(fired, 1);
    }};

const test::Registration many_events_stay_ordered{
    "scheduler.many_events_stay_ordered", [](test::Context& t) {
        Harness h;
        std::vector<std::uint64_t> times;
        std::vector<EventId> events;
        events.reserve(200);
        for (int i = 0; i < 200; ++i) {
            events.push_back(h.scheduler.add_event(
                "e", [&] { times.push_back(h.scheduler.now().nanoseconds); }));
        }
        // A fixed pseudo-random pattern of schedules, reschedules, and cancels.
        std::uint64_t state = 12345;
        auto next = [&state] {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return state >> 33;
        };
        std::size_t cancelled = 0;
        for (std::size_t i = 0; i < events.size(); ++i) {
            h.scheduler.schedule_at(events[i], VirtualTime{next() % 1'000});
            h.scheduler.schedule_at(events[next() % events.size()], VirtualTime{next() % 1'000});
        }
        for (std::size_t i = 0; i < events.size(); i += 7) {
            cancelled += h.scheduler.is_pending(events[i]) ? 1u : 0u;
            h.scheduler.cancel(events[i]);
        }
        const std::size_t fired = h.scheduler.advance_to(VirtualTime{1'000});
        t.check_equal(fired, events.size() - cancelled);
        t.check(std::ranges::is_sorted(times), "dispatch times never decrease");
    }};

class RecordingSink final : public TraceSink {
  public:
    void write(const TraceRecord& record) override {
        lines.push_back(std::to_string(record.time.nanoseconds) + " " +
                        std::string{trace_category_name(record.category)} + " " +
                        std::string{record.message});
    }
    std::vector<std::string> lines;
};

std::vector<std::string> traced_run() {
    Harness h;
    RecordingSink sink;
    h.tracer.set_sink(&sink);
    h.tracer.enable(TraceCategory::scheduler);
    EventId ping{};
    EventId pong{};
    ping = h.scheduler.add_event("ping", [&] { h.scheduler.schedule_after(pong, {7}); });
    pong = h.scheduler.add_event("pong", [&] { h.scheduler.schedule_after(ping, {3}); });
    h.scheduler.schedule_at(ping, VirtualTime{1});
    h.scheduler.advance_to(VirtualTime{40});
    return sink.lines;
}

const test::Registration dispatch_trace_is_deterministic{
    "scheduler.dispatch_trace_is_deterministic", [](test::Context& t) {
        const std::vector<std::string> first = traced_run();
        t.check(first == traced_run(), "identical runs produce identical traces");
        t.check(first ==
                std::vector<std::string>{"1 scheduler fire ping", "8 scheduler fire pong",
                                         "11 scheduler fire ping", "18 scheduler fire pong",
                                         "21 scheduler fire ping", "28 scheduler fire pong",
                                         "31 scheduler fire ping", "38 scheduler fire pong"});
    }};

} // namespace
