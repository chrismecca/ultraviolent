#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ultraviolent;

class RecordingSink final : public TraceSink {
  public:
    struct Line {
        std::uint64_t time;
        TraceCategory category;
        std::string message;

        bool operator==(const Line&) const = default;
    };

    void write(const TraceRecord& record) override {
        lines.push_back({record.time.nanoseconds, record.category, std::string{record.message}});
    }

    std::vector<Line> lines;
};

const test::Registration categories_are_gated{
    "trace.categories_are_gated", [](test::Context& t) {
        VirtualClock clock;
        Tracer tracer{clock};
        RecordingSink sink;

        t.check(!tracer.enabled(TraceCategory::memory), "a tracer without a sink traces nothing");
        tracer.enable(TraceCategory::memory);
        t.check(!tracer.enabled(TraceCategory::memory), "a tracer without a sink traces nothing");

        tracer.set_sink(&sink);
        t.check(tracer.enabled(TraceCategory::memory));
        t.check(!tracer.enabled(TraceCategory::irq));
        tracer.log(TraceCategory::memory, "read {:#x}", 0x40);
        tracer.log(TraceCategory::irq, "hidden");
        tracer.disable(TraceCategory::memory);
        tracer.log(TraceCategory::memory, "hidden");
        t.check(sink.lines ==
                std::vector<RecordingSink::Line>{{0, TraceCategory::memory, "read 0x40"}});
    }};

const test::Registration records_carry_virtual_time{
    "trace.records_carry_virtual_time", [](test::Context& t) {
        VirtualClock clock;
        Tracer tracer{clock};
        Scheduler scheduler{clock, tracer};
        RecordingSink sink;
        tracer.set_sink(&sink);
        tracer.enable(TraceCategory::machine);

        const EventId event = scheduler.add_event(
            "mark", [&] { tracer.emit(TraceCategory::machine, "at deadline"); });
        scheduler.schedule_at(event, VirtualTime{1'234});
        scheduler.advance_to(VirtualTime{5'000});
        tracer.emit(TraceCategory::machine, "after advance");
        t.check(sink.lines ==
                std::vector<RecordingSink::Line>{{1'234, TraceCategory::machine, "at deadline"},
                                                 {5'000, TraceCategory::machine, "after advance"}});
    }};

const test::Registration category_names_are_unique{
    "trace.category_names_are_unique", [](test::Context& t) {
        std::set<std::string_view> names;
        for (std::size_t i = 0; i < trace_category_count; ++i) {
            names.insert(trace_category_name(static_cast<TraceCategory>(i)));
        }
        t.check_equal(names.size(), trace_category_count);
        t.check_equal(trace_category_name(TraceCategory::scheduler), std::string_view{"scheduler"});
    }};

} // namespace
