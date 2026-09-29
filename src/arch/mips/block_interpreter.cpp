#include <ultraviolent/arch/mips/block_interpreter.hpp>

#include <ultraviolent/arch/mips/decoded.hpp>

namespace ultraviolent::mips {

void BlockInterpreter::run_until(const std::uint64_t& limit) {
    cpu_.synchronize_host_pages();
    while (cpu_.cycles() < limit) {
        if (cpu_.service_pending_exceptions()) {
            cpu_.end_cycle(false);
            ++statistics_.pending_at_entry;
            continue;
        }
        if (const auto none = build_block(cpu_, cpu_.state().pc, block_)) {
            ++statistics_.fallbacks[static_cast<std::size_t>(*none)];
            reference_.step();
            continue;
        }
        ++statistics_.built_lengths[block_.operations.size()];
        ++statistics_.built_ends[static_cast<std::size_t>(block_.end)];
        run_block(limit);
    }
}

// The first operation follows the checks run_until made; every later one gets the
// reference's boundary checks first (IR.adoc "Executor").
void BlockInterpreter::run_block(const std::uint64_t& limit) {
    const auto& operations = block_.operations;
    const IntegerState& s = cpu_.state();
    std::size_t next = 0;
    BlockExit exit = BlockExit::completed;
    for (;;) {
        cpu_.clear_left_host_path();
        const bool retired = complete_cycle(cpu_, operations[next]);
        ++next;
        if (!retired) {
            exit = BlockExit::exception;
            break;
        }
        if (cpu_.left_host_path()) {
            exit = BlockExit::host_path;
            break;
        }
        if (next == operations.size()) {
            break;
        }
        if (cpu_.cycles() >= limit) {
            exit = BlockExit::run_limit;
            break;
        }
        if (cpu_.service_pending_exceptions()) {
            cpu_.end_cycle(false);
            exit = BlockExit::pending_exception;
            break;
        }
        if (s.pc != block_.start_pc + 4 * next) {
            exit = BlockExit::left_block;
            break;
        }
        if (block_.word_now(next) != operations[next].instruction.word) {
            exit = BlockExit::code_changed;
            break;
        }
    }
    ++statistics_.entries;
    statistics_.operations += next;
    ++statistics_.run_lengths[next];
    ++statistics_.exits[static_cast<std::size_t>(exit)];
}

} // namespace ultraviolent::mips
