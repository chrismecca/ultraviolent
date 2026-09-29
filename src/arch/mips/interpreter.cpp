#include <ultraviolent/arch/mips/interpreter.hpp>

#include <ultraviolent/arch/mips/decoded.hpp>

namespace ultraviolent::mips {

namespace {

// One processor cycle; Interpreter::step and Interpreter::run_until both inline it. The
// reference sequence: pending exceptions, fetch, decode, execute (semantics.cpp), and the
// cycle accounting every engine reproduces (IR.adoc "Execution rules every engine keeps").
[[gnu::always_inline]] inline void execute_cycle(Cpu& cpu) {
    if (cpu.service_pending_exceptions()) {
        cpu.end_cycle(false);
        return;
    }
    IntegerState& s = cpu.state();
    auto word = cpu.fetch(s.pc);
    if (!word) {
        cpu.take_exception(word.error());
        cpu.end_cycle(false);
        return;
    }
    complete_cycle(cpu, decode(*word));
}

} // namespace

void Interpreter::step() {
    cpu_.synchronize_host_pages();
    execute_cycle(cpu_);
}

void Interpreter::run_until(const std::uint64_t& limit) {
    cpu_.synchronize_host_pages();
    while (cpu_.cycles() < limit) {
        execute_cycle(cpu_);
    }
}

} // namespace ultraviolent::mips
