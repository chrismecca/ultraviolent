#pragma once

#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/core/virtual_time.hpp>

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace ultraviolent {

// Diagnostic trace categories (ENGINEERING "Logging"). Tracing is observation only: no
// machine behavior may depend on whether a category is enabled.
enum class TraceCategory : std::uint8_t {
    cpu,
    exception,
    tlb,
    memory,
    hub,
    xtalk,
    xbow,
    bridge,
    pci,
    ioc3,
    scsi,
    ethernet,
    irq,
    scheduler,
    firmware,
    machine,
};

inline constexpr std::size_t trace_category_count = 16;

std::string_view trace_category_name(TraceCategory category);

// One trace message. `message` is only valid for the duration of TraceSink::write.
struct TraceRecord {
    VirtualTime time;
    TraceCategory category;
    std::string_view message;
};

// Receives enabled trace records. Where they go (terminal, file, flight recorder) is a host
// concern.
class TraceSink {
  public:
    virtual void write(const TraceRecord& record) = 0;

  protected:
    ~TraceSink() = default;
};

// Per-machine trace switchboard. Records are stamped with virtual time, never host time, so
// the trace of a deterministic run is itself deterministic.
class Tracer {
  public:
    explicit Tracer(const VirtualClock& clock) : clock_{clock} {}
    Tracer(const Tracer&) = delete;
    Tracer& operator=(const Tracer&) = delete;

    // The sink is not owned. A tracer without a sink reports every category disabled.
    void set_sink(TraceSink* sink) {
        sink_ = sink;
    }

    void enable(TraceCategory category) {
        enabled_mask_ |= bit(category);
    }
    void disable(TraceCategory category) {
        enabled_mask_ &= ~bit(category);
    }

    [[nodiscard]] bool enabled(TraceCategory category) const {
        return sink_ != nullptr && (enabled_mask_ & bit(category)) != 0;
    }

    // Formats only when the category is enabled, so disabled tracing costs one bit test.
    template <class... Args>
    void log(TraceCategory category, std::format_string<Args...> format, Args&&... args) {
        if (enabled(category)) {
            emit(category, std::format(format, std::forward<Args>(args)...));
        }
    }

    void emit(TraceCategory category, std::string_view message);

  private:
    static constexpr std::uint32_t bit(TraceCategory category) {
        return std::uint32_t{1} << std::to_underlying(category);
    }

    const VirtualClock& clock_;
    TraceSink* sink_{};
    std::uint32_t enabled_mask_{};
};

} // namespace ultraviolent
