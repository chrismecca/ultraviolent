#pragma once

// Differential testing of execution engines against the reference interpreter (IR.adoc
// "Correctness method"): two identical CPU + RAM systems, no MMIO, one stepped by the
// reference interpreter and one by the engine under test, compared after every cycle.

#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/block.hpp>
#include <ultraviolent/arch/mips/block_interpreter.hpp>
#include <ultraviolent/arch/mips/decoded.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <random>
#include <string>
#include <unordered_map>

namespace ultraviolent::mips::testing {

// The engine under test at stage C: instructions decoded once, ahead of execution, from the
// programs a test loads, and executed through complete_cycle. It uses a decoded instruction
// only where a fetch at pc would give the same word now; anything else (fetch faults, code it
// did not decode) runs as a reference step. Later stages substitute blocks.
class PredecodedEngine {
  public:
    static constexpr bool decodes_unsupported = true;

    explicit PredecodedEngine(Cpu& cpu) : cpu_{cpu}, fallback_{cpu} {}

    void add(std::uint64_t virtual_address, std::uint32_t word) {
        decoded_.insert_or_assign(virtual_address, decode(word));
    }

    void step() {
        cpu_.synchronize_host_pages();
        if (cpu_.service_pending_exceptions()) {
            cpu_.end_cycle(false);
            return;
        }
        const std::uint64_t pc = cpu_.state().pc;
        if (const auto it = decoded_.find(pc); it != decoded_.end()) {
            if (const auto word = cpu_.fetch(pc); word && *word == it->second.instruction.word) {
                complete_cycle(cpu_, it->second);
                ++decoded_steps;
                return;
            }
        }
        fallback_.step();
        ++fallback_steps;
    }

    std::uint64_t decoded_steps{};
    std::uint64_t fallback_steps{};

  private:
    Cpu& cpu_;
    Interpreter fallback_;
    std::unordered_map<std::uint64_t, DecodedInstruction> decoded_;
};

// The engine under test at stage D: blocks built at each block entry (build_block), their
// operations run one per cycle through complete_cycle, following the tier-0 executor rules
// (IR.adoc "Executor"): a block is left after an exception, after an access that left the
// host fast path, when pc is not the next operation's, or when an operation's word changed in
// memory. Where no block can be built, a reference step runs, counted by reason.
class BlockSteppingEngine {
  public:
    static constexpr bool decodes_unsupported = false;

    explicit BlockSteppingEngine(Cpu& cpu) : cpu_{cpu}, fallback_{cpu} {}

    void add(std::uint64_t /*virtual_address*/, std::uint32_t /*word*/) {}

    void step() {
        cpu_.synchronize_host_pages();
        if (cpu_.service_pending_exceptions()) {
            cpu_.end_cycle(false);
            in_block_ = false;
            return;
        }
        const std::uint64_t pc = cpu_.state().pc;
        if (!in_block_ || pc != block_.start_pc + 4 * next_ ||
            block_.word_now(next_) != block_.operations[next_].instruction.word) {
            if (const auto none = build_block(cpu_, pc, block_)) {
                fallback_.step();
                ++fallback_steps;
                ++fallback_reasons[static_cast<std::size_t>(*none)];
                in_block_ = false;
                return;
            }
            ++blocks_built;
            next_ = 0;
            in_block_ = true;
        }
        cpu_.clear_left_host_path();
        const bool retired = complete_cycle(cpu_, block_.operations[next_]);
        ++decoded_steps;
        ++next_;
        if (!retired || cpu_.left_host_path() || next_ == block_.operations.size()) {
            in_block_ = false;
        }
    }

    std::uint64_t decoded_steps{};
    std::uint64_t fallback_steps{};
    std::uint64_t blocks_built{};
    std::array<std::uint64_t, no_block_count> fallback_reasons{};

  private:
    Cpu& cpu_;
    Interpreter fallback_;
    Block block_;
    std::size_t next_{};
    bool in_block_{};
};

// The engine under test from stage E: the library's tier-0 executor with a block cache of
// `Entries`, run for chunks of cycles (DifferentialWith::run compares after each chunk).
template <std::size_t Entries = BlockInterpreter::default_cache_entries> class Tier0EngineSized {
  public:
    static constexpr bool decodes_unsupported = false;

    explicit Tier0EngineSized(Cpu& cpu) : interpreter_{cpu, Entries} {}

    void add(std::uint64_t /*virtual_address*/, std::uint32_t /*word*/) {}

    void run_until(const std::uint64_t& limit) {
        interpreter_.run_until(limit);
        const BlockStatistics& s = interpreter_.statistics();
        decoded_steps = s.operations;
        fallback_reasons = s.fallbacks;
        fallback_steps = 0;
        for (const std::uint64_t n : s.fallbacks) {
            fallback_steps += n;
        }
    }

    [[nodiscard]] const BlockStatistics& statistics() const {
        return interpreter_.statistics();
    }

    std::uint64_t decoded_steps{};
    std::uint64_t fallback_steps{};
    std::array<std::uint64_t, no_block_count> fallback_reasons{};

  private:
    BlockInterpreter interpreter_;
};

// The first difference between two systems' architectural state and memory, or empty.
inline std::string difference(const System& a, const System& b) {
    const Cpu::State x = a.cpu.capture();
    const Cpu::State y = b.cpu.capture();
    if (x.integer != y.integer) {
        for (unsigned r = 0; r < 32; ++r) {
            if (x.integer.gpr[r] != y.integer.gpr[r]) {
                return std::format("gpr {}: {:#x} vs {:#x}", r, x.integer.gpr[r], y.integer.gpr[r]);
            }
        }
        return std::format("integer state: pc {:#x}/{:#x} next {:#x}/{:#x}", x.integer.pc,
                           y.integer.pc, x.integer.next_pc, y.integer.next_pc);
    }
    if (x.fpu != y.fpu) {
        return "fpu state";
    }
    if (x.cp0 != y.cp0) {
        return "cp0 registers";
    }
    if (x.tlb != y.tlb) {
        return "tlb";
    }
    if (x.cycles != y.cycles || x.retired != y.retired) {
        return std::format("cycles {}/{} retired {}/{}", x.cycles, y.cycles, x.retired, y.retired);
    }
    if (x != y) {
        return "interrupt or watch state";
    }
    if (!(a.cpu.caches() == b.cpu.caches())) {
        return "cache arrays";
    }
    for (const auto& [left, right, name] : {std::tuple{a.ram.bytes(), b.ram.bytes(), "ram"},
                                            std::tuple{a.boot.bytes(), b.boot.bytes(), "boot"}}) {
        if (std::memcmp(left.data(), right.data(), left.size()) != 0) {
            std::size_t at = 0;
            while (left[at] == right[at]) {
                ++at;
            }
            return std::format("{} byte {:#x}", name, at);
        }
    }
    return {};
}

// A reference system and a candidate system kept identical: every setup action is applied
// to both, and the candidate's engine is told about every program loaded.
template <class Engine> struct DifferentialWith {
    System reference;
    System candidate;
    Engine engine{candidate.cpu};
    // For engines that run many cycles per call: the longest chunk between comparisons.
    // Short random chunks stop blocks at arbitrary points; tests of cache statistics use
    // long ones.
    std::uint64_t max_chunk{37};

    // Applies `action` to both systems.
    void both(const std::function<void(System&)>& action) {
        action(reference);
        action(candidate);
    }

    // Loads `words` at `physical` in both systems; the candidate decodes them at
    // `virtual_address` (where the program will run).
    void load(std::uint64_t physical, std::uint64_t virtual_address,
              std::initializer_list<std::uint32_t> words) {
        both([&](System& s) { s.load(physical, words); });
        for (const std::uint32_t word : words) {
            engine.add(virtual_address, word);
            virtual_address += 4;
        }
    }

    // A handler at every exception vector (BEV clear) that resumes after the faulting
    // instruction.
    void load_skip_handlers() {
        for (const std::uint64_t vector : {0x000u, 0x080u, 0x180u}) {
            load(vector, kseg0(vector),
                 {assembler::dmfc0(assembler::k0, cp0::epc),
                  assembler::daddiu(assembler::k0, assembler::k0, 4),
                  assembler::dmtc0(assembler::k0, cp0::epc), assembler::eret()});
        }
    }

    // Steps both systems `steps` cycles, calling `before` (on both) ahead of each cycle, and
    // compares them after every cycle. Returns false at the first difference.
    bool run(test::Context& t, std::uint64_t steps,
             const std::function<void(std::uint64_t, System&)>& before = {}) {
        if constexpr (requires(Engine& e, const std::uint64_t& limit) { e.run_until(limit); }) {
            // Engines that run many cycles per call: chunks of 1 to 37 cycles (a fixed
            // sequence), both systems through run_until, compared after each chunk. `before`
            // runs for every cycle of a chunk at its start, on both systems alike.
            std::mt19937_64 rng{steps};
            for (std::uint64_t n = 0; n < steps;) {
                const std::uint64_t random_chunk = rng() % 4 == 0 ? 1 : 1 + rng() % 37;
                const std::uint64_t chunk =
                    std::min(steps - n, max_chunk <= 37 ? random_chunk : max_chunk);
                if (before) {
                    for (std::uint64_t m = n; m < n + chunk; ++m) {
                        before(m, reference);
                        before(m, candidate);
                    }
                }
                const std::uint64_t reference_limit = reference.cpu.cycles() + chunk;
                reference.interpreter.run_until(reference_limit);
                const std::uint64_t candidate_limit = candidate.cpu.cycles() + chunk;
                engine.run_until(candidate_limit);
                n += chunk;
                if (const std::string d = difference(reference, candidate); !d.empty()) {
                    t.check(false,
                            std::format("after cycle {} pc {:#x}: {}", n, reference.pc(), d));
                    return false;
                }
            }
            return true;
        } else {
            for (std::uint64_t n = 0; n < steps; ++n) {
                if (before) {
                    before(n, reference);
                    before(n, candidate);
                }
                reference.interpreter.step();
                engine.step();
                if (const std::string d = difference(reference, candidate); !d.empty()) {
                    t.check(false, std::format("cycle {} pc {:#x}: {}", n, reference.pc(), d));
                    return false;
                }
            }
            return true;
        }
    }
};

using Differential = DifferentialWith<PredecodedEngine>;
using Tier0Engine = Tier0EngineSized<>;
// Two entries: nearly every block change replaces an entry.
using Tier0TinyCacheEngine = Tier0EngineSized<2>;

} // namespace ultraviolent::mips::testing
