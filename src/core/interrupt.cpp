#include <ultraviolent/core/interrupt.hpp>

#include <ultraviolent/core/invariant.hpp>

namespace ultraviolent {

void InterruptLine::connect(InterruptSink& sink, std::uint32_t input) {
    invariant(sink_ == nullptr, "interrupt line is already connected");
    sink_ = &sink;
    input_ = input;
    if (asserted_) {
        sink_->set_interrupt_level(input_, true);
    }
}

void InterruptLine::set_level(bool asserted) {
    if (asserted == asserted_) {
        return;
    }
    asserted_ = asserted;
    if (sink_ != nullptr) {
        sink_->set_interrupt_level(input_, asserted_);
    }
}

} // namespace ultraviolent
