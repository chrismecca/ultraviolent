#pragma once

#include <ultraviolent/arch/mips/cpu.hpp>

#include <cstdint>

namespace ultraviolent::mips {

// The reference interpreter (ADR-003): the permanent correctness oracle for MIPS execution.
// It keeps no architectural state of its own; everything lives in the Cpu.
class Interpreter {
  public:
    explicit Interpreter(Cpu& cpu) : cpu_{cpu} {}

    // One processor cycle: takes a pending interrupt or delayed watch exception, or else
    // fetches and executes the instruction at pc (taking its exception if it raises one).
    void step();

    void run(std::uint64_t steps) {
        for (std::uint64_t i = 0; i < steps; ++i) {
            step();
        }
    }

    // Steps while the CPU's cycle count is below `limit`, which the caller may lower while
    // this runs (for instance from a device access that schedules an event).
    void run_until(const std::uint64_t& limit);

  private:
    Cpu& cpu_;
};

} // namespace ultraviolent::mips
