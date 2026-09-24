#pragma once

#include <cstdint>

namespace ultraviolent {

// Receiver of interrupt levels, such as a CPU's interrupt pins or an interrupt controller.
// `input` distinguishes the sink's inputs; its meaning belongs to the sink.
class InterruptSink {
  public:
    // Called only when the level of a connected line changes.
    virtual void set_interrupt_level(std::uint32_t input, bool asserted) = 0;

  protected:
    ~InterruptSink() = default;
};

// A level-sensitive interrupt wire from one source to one sink input. The line remembers its
// level; the sink hears only about changes, so a source may drive the level from its state
// without tracking what it last reported.
class InterruptLine {
  public:
    InterruptLine() = default;
    InterruptLine(const InterruptLine&) = delete;
    InterruptLine& operator=(const InterruptLine&) = delete;

    // Wires the line to `input` of `sink`. If the line is already asserted, the sink sees the
    // level immediately. A line connects once, when the machine is assembled.
    void connect(InterruptSink& sink, std::uint32_t input);

    void set_level(bool asserted);
    void raise() {
        set_level(true);
    }
    void lower() {
        set_level(false);
    }

    [[nodiscard]] bool is_asserted() const {
        return asserted_;
    }

  private:
    InterruptSink* sink_{};
    std::uint32_t input_{};
    bool asserted_{};
};

} // namespace ultraviolent
