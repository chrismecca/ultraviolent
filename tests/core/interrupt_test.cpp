#include "support/test.hpp"

#include <ultraviolent/core/interrupt.hpp>

#include <cstdint>
#include <vector>

namespace {

using namespace ultraviolent;

class RecordingSink final : public InterruptSink {
  public:
    struct Change {
        std::uint32_t input;
        bool asserted;

        bool operator==(const Change&) const = default;
    };

    void set_interrupt_level(std::uint32_t input, bool asserted) override {
        changes.push_back({input, asserted});
    }

    std::vector<Change> changes;
};

const test::Registration sink_sees_changes_only{
    "interrupt.sink_sees_changes_only", [](test::Context& t) {
        RecordingSink sink;
        InterruptLine line;
        line.connect(sink, 3);
        line.raise();
        line.raise();
        line.set_level(true);
        line.lower();
        line.lower();
        t.check(sink.changes == std::vector<RecordingSink::Change>{{3, true}, {3, false}});
        t.check(!line.is_asserted());
    }};

const test::Registration connect_propagates_level{
    "interrupt.connect_propagates_asserted_level", [](test::Context& t) {
        RecordingSink sink;
        InterruptLine asserted;
        InterruptLine quiet;
        asserted.raise();
        asserted.connect(sink, 1);
        quiet.connect(sink, 2);
        t.check(sink.changes == std::vector<RecordingSink::Change>{{1, true}},
                "an asserted line is visible on connection; an idle one is silent");
    }};

const test::Registration unconnected_line_holds_level{"interrupt.unconnected_line_holds_level",
                                                      [](test::Context& t) {
                                                          InterruptLine line;
                                                          line.raise();
                                                          t.check(line.is_asserted());
                                                          line.lower();
                                                          t.check(!line.is_asserted());
                                                      }};

} // namespace
