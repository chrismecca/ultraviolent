#include <ultraviolent/arch/mips/block_interpreter.hpp>

#include <ultraviolent/arch/mips/decoded.hpp>
#include <ultraviolent/core/invariant.hpp>

#include <algorithm>
#include <bit>

namespace ultraviolent::mips {

BlockInterpreter::BlockInterpreter(Cpu& cpu, std::size_t cache_entries)
    : cpu_{cpu}, reference_{cpu} {
    set_cache_entries(cache_entries);
}

void BlockInterpreter::set_cache_entries(std::size_t entries) {
    invariant(std::has_single_bit(entries), "the block cache size is a power of two");
    cache_ = std::vector<Entry>(entries);
    mask_ = entries - 1;
}

void BlockInterpreter::run_until(const std::uint64_t& limit) {
    cpu_.synchronize_host_pages();
    while (cpu_.cycles() < limit) {
        if (cpu_.service_pending_exceptions()) {
            cpu_.end_cycle(false);
            ++statistics_.pending_at_entry;
            continue;
        }
        const std::uint64_t hits = statistics_.hits;
        const Block* block = lookup(cpu_.state().pc);
        if (block == nullptr) {
            reference_.step();
            continue;
        }
        run_block(*block, limit, statistics_.hits != hits);
    }
}

// Lookups happen only between blocks, so replacing an entry never touches the block being
// executed, and invalidation never frees anything: a stale entry simply stops matching
// (IR.adoc "Keeping blocks valid").
const Block* BlockInterpreter::lookup(std::uint64_t pc) {
    ++statistics_.lookups;
    Entry& entry = cache_[(pc >> 2) & mask_];
    const std::uint64_t context = cpu_.code_context();
    CacheMiss miss{};
    if (entry.pc != pc || entry.context != context) {
        miss = entry.pc == empty_pc ? CacheMiss::cold : CacheMiss::tag;
    } else if (entry.epoch != cpu_.code_epoch()) {
        miss = CacheMiss::epoch;
    } else if (*entry.frame_generation != entry.frame_generation_value) {
        miss = CacheMiss::frame;
    } else if (entry.tlb_index >= 0 && cpu_.tlb_generation(static_cast<std::size_t>(
                                           entry.tlb_index)) != entry.tlb_generation) {
        miss = CacheMiss::tlb;
    } else {
        ++statistics_.hits;
        ++entry.uses;
        return &entry.block;
    }
    ++statistics_.misses[static_cast<std::size_t>(miss)];
    if (entry.pc != empty_pc) {
        statistics_.max_reuse = std::max(statistics_.max_reuse, entry.uses);
    }
    entry.pc = empty_pc;
    entry.uses = 0;
    if (const auto none = build_block(cpu_, pc, entry.block)) {
        ++statistics_.fallbacks[static_cast<std::size_t>(*none)];
        return nullptr;
    }
    const Cpu::CodePage& code = entry.block.code;
    cpu_.claim_code_frame(code);
    entry.pc = pc;
    entry.context = context;
    entry.epoch = cpu_.code_epoch();
    entry.frame_generation = code.block->code_generation(code.frame);
    entry.frame_generation_value = *entry.frame_generation;
    entry.tlb_index = code.tlb_index;
    entry.tlb_generation =
        code.tlb_index >= 0 ? cpu_.tlb_generation(static_cast<std::size_t>(code.tlb_index)) : 0;
    ++statistics_.blocks_built;
    ++statistics_.built_lengths[entry.block.operations.size()];
    ++statistics_.built_ends[static_cast<std::size_t>(entry.block.end)];
    return &entry.block;
}

// The first operation follows the checks run_until made; every later one gets the
// reference's boundary checks first (IR.adoc "Executor"). No operation's word is fetched
// again: a store into the block's frame reaches the bus (the frame is claimed), which is a
// barrier, and advances the frame's generation, so the next lookup rebuilds.
void BlockInterpreter::run_block(const Block& block, const std::uint64_t& limit, bool cached) {
    const auto& operations = block.operations;
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
            ++statistics_.barriers[static_cast<std::size_t>(cpu_.last_bus_access())];
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
        if (s.pc != block.start_pc + 4 * next) {
            exit = BlockExit::left_block;
            break;
        }
    }
    ++statistics_.entries;
    statistics_.operations += next;
    if (cached) {
        statistics_.cached_operations += next;
    }
    ++statistics_.run_lengths[next];
    ++statistics_.exits[static_cast<std::size_t>(exit)];
}

} // namespace ultraviolent::mips
