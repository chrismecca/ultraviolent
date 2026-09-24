#include <ultraviolent/core/trace.hpp>

namespace ultraviolent {

std::string_view trace_category_name(TraceCategory category) {
    switch (category) {
    case TraceCategory::cpu:
        return "cpu";
    case TraceCategory::exception:
        return "exception";
    case TraceCategory::tlb:
        return "tlb";
    case TraceCategory::memory:
        return "memory";
    case TraceCategory::hub:
        return "hub";
    case TraceCategory::xtalk:
        return "xtalk";
    case TraceCategory::xbow:
        return "xbow";
    case TraceCategory::bridge:
        return "bridge";
    case TraceCategory::pci:
        return "pci";
    case TraceCategory::ioc3:
        return "ioc3";
    case TraceCategory::scsi:
        return "scsi";
    case TraceCategory::ethernet:
        return "ethernet";
    case TraceCategory::irq:
        return "irq";
    case TraceCategory::scheduler:
        return "scheduler";
    case TraceCategory::firmware:
        return "firmware";
    case TraceCategory::machine:
        return "machine";
    }
    invariant_failed("unknown trace category");
}

void Tracer::emit(TraceCategory category, std::string_view message) {
    if (enabled(category)) {
        sink_->write({clock_.now(), category, message});
    }
}

} // namespace ultraviolent
