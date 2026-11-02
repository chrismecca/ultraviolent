#include <ultraviolent/arch/mips/block_interpreter.hpp>

#include <ultraviolent/arch/mips/decoded.hpp>
#include <ultraviolent/core/invariant.hpp>

#include "semantics.hpp"

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
        const NativeRun native = native_ != nullptr ? run_native(limit) : NativeRun::absent;
        if (native == NativeRun::ran) {
            continue;
        }
        const std::uint64_t hits = statistics_.hits;
        Entry* entry = lookup(cpu_.state().pc);
        if (entry == nullptr) {
            reference_.step();
            continue;
        }
        // A refused block runs decoded, exactly (it may stop anywhere inside, or start in a
        // delay slot).
        if (native == NativeRun::absent && native_ != nullptr && install_native(*entry)) {
            continue;
        }
        run_block(entry->block, limit, statistics_.hits != hits);
    }
}

// Native code runs only from here, between blocks; translations happen only here too, so a
// translation that discards the code region never removes code that is running (JIT.adoc
// "Native code: memory, lifetime, calls").
BlockInterpreter::NativeRun BlockInterpreter::run_native(const std::uint64_t& limit) {
    const IntegerState& s = cpu_.state();
    if (s.delay_slot) {
        // Native blocks start outside delay slots (a reference step can leave one).
        return NativeRun::refused;
    }
    DispatchEntry& entry = dispatch_[dispatch_index(s.pc)];
    if (entry.pc != s.pc || entry.context != cpu_.code_context()) {
        return NativeRun::absent;
    }
    cpu_.synchronize_code();
    if (entry.stamp != cpu_.code_changes()) {
        ++statistics_.dispatch_revalidations;
        if (!valid(dispatch_validity_[dispatch_index(s.pc)])) {
            ++statistics_.dispatch_invalid;
            entry = {};
            return NativeRun::absent;
        }
        entry.stamp = cpu_.code_changes();
    }
    const std::uint64_t cycles = cpu_.cycles();
    const NativeExit exit = native_->run(entry.code, limit, dispatch_.data());
    // Every block that starts runs at least one operation, and every operation takes a cycle.
    if (cpu_.cycles() == cycles && exit.reason == BlockExit::run_limit) {
        ++statistics_.native_refusals;
        return NativeRun::refused;
    }
    if (exit.reason == BlockExit::host_path) {
        ++statistics_.barriers[static_cast<std::size_t>(cpu_.last_bus_access())];
    }
    // Every operation native code ran took one cycle (an exception's too).
    const std::uint64_t operations = cpu_.cycles() - cycles;
    ++statistics_.native_runs;
    statistics_.native_operations += operations;
    ++statistics_.entries;
    statistics_.operations += operations;
    statistics_.cached_operations += operations;
    ++statistics_.exits[static_cast<std::size_t>(exit.reason)];
    return NativeRun::ran;
}

bool BlockInterpreter::install_native(Entry& entry) {
    if (entry.native == nullptr || entry.native_epoch != native_->epoch()) {
        if (entry.uses < native_threshold_) {
            return false;
        }
        RegionBlocks more{*this, cpu_, entry.block, region_storage_};
        region_entries_.clear();
        entry.native = native_->translate(entry.block, more, region_entries_);
        entry.native_epoch = native_->epoch();
        if (entry.native == nullptr) {
            return false;
        }
    }
    if (dispatch_epoch_ != native_->epoch()) {
        // The code region was discarded: no other entry's code exists any more.
        clear_dispatch();
    }
    // lookup has just validated the entry.
    cpu_.synchronize_code();
    const std::size_t index = dispatch_index(entry.pc);
    dispatch_[index] = {.pc = entry.pc,
                        .context = entry.context,
                        .stamp = cpu_.code_changes(),
                        .code = entry.native};
    const DispatchValidity validity{.epoch = entry.epoch,
                                    .frame_generation = entry.frame_generation,
                                    .frame_generation_value = entry.frame_generation_value,
                                    .tlb_generation = entry.tlb_generation,
                                    .tlb_index = entry.tlb_index};
    dispatch_validity_[index] = validity;
    ++statistics_.dispatch_installs;
    // The region's other blocks share the first's code page and context, so its validity.
    for (const RegionEntry& r : region_entries_) {
        const std::size_t other = dispatch_index(r.pc);
        dispatch_[other] = {.pc = r.pc,
                            .context = entry.context,
                            .stamp = cpu_.code_changes(),
                            .code = r.code};
        dispatch_validity_[other] = validity;
        ++statistics_.dispatch_installs;
    }
    region_entries_.clear();
    return true;
}

const Block* BlockInterpreter::RegionBlocks::block_at(std::uint64_t pc) {
    if ((pc >> 12) != (first_.start_pc >> 12)) {
        return nullptr;
    }
    // By default only blocks that have run: tier 0's cache holds the pc in this context and
    // has been hit. Joining every reachable block translated about twice the code (`fs` 98
    // against 49 MB, `boot` 790 against 479 MB) for nothing that ran.
    const Entry& cached = engine_.cache_[(pc >> 2) & engine_.mask_];
    if (engine_.region_joins_ == RegionJoins::ran &&
        (cached.pc != pc || cached.context != cpu_.code_context() || cached.uses < 1)) {
        return nullptr;
    }
    if (used_ == storage_.size()) {
        storage_.push_back(std::make_unique<Block>());
    }
    Block& block = *storage_[used_];
    if (build_block(cpu_, pc, block)) {
        return nullptr;
    }
    const Cpu::CodePage& a = block.code;
    const Cpu::CodePage& b = first_.code;
    if (a.bytes != b.bytes || a.block != b.block || a.frame != b.frame ||
        a.tlb_index != b.tlb_index) {
        return nullptr;
    }
    ++used_;
    return &block;
}

bool BlockInterpreter::valid(const DispatchValidity& v) const {
    return v.epoch == cpu_.code_epoch() &&
           v.frame_generation->load(std::memory_order_relaxed) == v.frame_generation_value &&
           (v.tlb_index < 0 ||
            cpu_.tlb_generation(static_cast<std::size_t>(v.tlb_index)) == v.tlb_generation);
}

void BlockInterpreter::clear_dispatch() {
    dispatch_.assign(native_ != nullptr ? dispatch_entries : 0, DispatchEntry{});
    dispatch_validity_.assign(dispatch_.size(), DispatchValidity{});
    dispatch_epoch_ = native_ != nullptr ? native_->epoch() : 0;
}

void BlockInterpreter::count_exit(BlockExit exit, std::size_t operations, bool cached) {
    ++statistics_.entries;
    statistics_.operations += operations;
    if (cached) {
        statistics_.cached_operations += operations;
    }
    ++statistics_.run_lengths[operations];
    ++statistics_.exits[static_cast<std::size_t>(exit)];
}

// Lookups happen only between blocks, so replacing an entry never touches the block being
// executed, and invalidation never frees anything: a stale entry simply stops matching
// (IR.adoc "Keeping blocks valid").
BlockInterpreter::Entry* BlockInterpreter::lookup(std::uint64_t pc) {
    ++statistics_.lookups;
    Entry& entry = cache_[(pc >> 2) & mask_];
    const std::uint64_t context = cpu_.code_context();
    CacheMiss miss{};
    if (entry.pc != pc || entry.context != context) {
        miss = entry.pc == empty_pc ? CacheMiss::cold : CacheMiss::tag;
    } else if (entry.epoch != cpu_.code_epoch()) {
        miss = CacheMiss::epoch;
    } else if (entry.frame_generation->load(std::memory_order_relaxed) !=
               entry.frame_generation_value) {
        miss = CacheMiss::frame;
    } else if (entry.tlb_index >= 0 && cpu_.tlb_generation(static_cast<std::size_t>(
                                           entry.tlb_index)) != entry.tlb_generation) {
        miss = CacheMiss::tlb;
    } else {
        ++statistics_.hits;
        ++entry.uses;
        return &entry;
    }
    ++statistics_.misses[static_cast<std::size_t>(miss)];
    if (entry.pc != empty_pc) {
        statistics_.max_reuse = std::max(statistics_.max_reuse, entry.uses);
    }
    entry.pc = empty_pc;
    entry.uses = 0;
    entry.native = nullptr;
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
    entry.frame_generation_value = entry.frame_generation->load(std::memory_order_relaxed);
    entry.tlb_index = code.tlb_index;
    entry.tlb_generation =
        code.tlb_index >= 0 ? cpu_.tlb_generation(static_cast<std::size_t>(code.tlb_index)) : 0;
    ++statistics_.blocks_built;
    ++statistics_.built_lengths[entry.block.operations.size()];
    ++statistics_.built_ends[static_cast<std::size_t>(entry.block.end)];
    return &entry;
}

// The first operation follows the checks run_until made; every later one gets the
// reference's boundary checks first (IR.adoc "Executor"). No operation's word is fetched
// again: a store into the block's frame reaches the bus (the frame is claimed), which is a
// barrier, and advances the frame's generation, so the next lookup rebuilds.
void BlockInterpreter::run_block(const Block& block, const std::uint64_t& limit, bool cached) {
    // Locals, not members read through `this` and `block`: stores into the CPU's state would
    // otherwise make the compiler reload them for every operation (IR.adoc "Stage H"). The run
    // limit is still read through its reference at every boundary (a device access can lower
    // it).
    Cpu& cpu = cpu_;
    const DecodedInstruction* const operations = block.operations.data();
    const std::size_t count = block.operations.size();
    const std::uint64_t start_pc = block.start_pc;
    const IntegerState& s = cpu.state();
    std::size_t next = 0;
    BlockExit exit = BlockExit::completed;
    for (;;) {
        cpu.clear_left_host_path();
        const bool retired = complete_cycle<&execute_inline>(cpu, operations[next]);
        ++next;
        if (!retired) {
            exit = BlockExit::exception;
            break;
        }
        if (cpu.left_host_path()) {
            exit = BlockExit::host_path;
            ++statistics_.barriers[static_cast<std::size_t>(cpu.last_bus_access())];
            break;
        }
        if (next == count) {
            break;
        }
        if (cpu.cycles() >= limit) {
            exit = BlockExit::run_limit;
            break;
        }
        if (cpu.service_pending_exceptions()) {
            cpu.end_cycle(false);
            exit = BlockExit::pending_exception;
            break;
        }
        if (s.pc != start_pc + 4 * next) {
            exit = BlockExit::left_block;
            break;
        }
    }
    count_exit(exit, next, cached);
}

#ifndef ULTRAVIOLENT_TIER1
std::unique_ptr<NativeCode> make_native_code(Cpu& /*cpu*/, std::size_t /*code_bytes*/) {
    return nullptr;
}
#endif

} // namespace ultraviolent::mips
