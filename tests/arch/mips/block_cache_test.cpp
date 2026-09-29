// Stage F of IR.adoc: the tier-0 block cache and its invalidation. Every test runs the
// reference interpreter alongside and compares the full state (differential.hpp); the
// statistics show which mechanism acted.

#include "arch/mips/assembler.hpp"
#include "arch/mips/differential.hpp"
#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/block_interpreter.hpp>
#include <ultraviolent/arch/mips/cp0.hpp>
#include <ultraviolent/core/byte_order.hpp>
#include <ultraviolent/core/state_image.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <string>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using namespace ultraviolent::mips::testing;

constexpr std::uint64_t code = kseg0(System::code);
constexpr std::uint64_t frame_a = 0x3000; // physical frames holding routines
constexpr std::uint64_t frame_b = 0x5000;
constexpr std::uint64_t mapped = 0x10000; // a TLB-mapped virtual page

// Runs are not cut into short chunks here, so the statistics count whole blocks.
template <std::size_t Entries> struct Pair : DifferentialWith<Tier0EngineSized<Entries>> {
    Pair() {
        this->max_chunk = 1'000'000;
    }
};

std::size_t index(auto e) {
    return static_cast<std::size_t>(e);
}

// The counters a failed expectation needs to be understood.
std::string describe(const BlockStatistics& s) {
    return std::format("lookups {} hits {} built {} misses cold {} tag {} epoch {} frame {} tlb {}"
                       " barriers fill {} device {} uncached {} code {} other {}",
                       s.lookups, s.hits, s.blocks_built, s.misses[0], s.misses[1], s.misses[2],
                       s.misses[3], s.misses[4], s.barriers[0], s.barriers[1], s.barriers[2],
                       s.barriers[3], s.barriers[4]);
}

// A routine that adds `amount` to v0 and returns.
void load_routine(auto& d, std::uint64_t physical, std::int32_t amount) {
    d.both([&](System& s) { s.load(physical, {addiu(v0, v0, amount), jr(ra), nop()}); });
}

// TLB entry `slot`: the even 4 KiB page at `va` to `pa`, with `asid` unless global. EntryHi
// is restored to `current_asid` afterwards.
void map(System& s, unsigned slot, std::uint64_t va, std::uint64_t pa, unsigned asid, bool global,
         unsigned current_asid) {
    const std::uint64_t flags =
        (3u << entry_lo::c_shift) | entry_lo::d | entry_lo::v | (global ? entry_lo::g : 0);
    s.cpu.dmtc0(cp0::entry_hi, va | asid);
    s.cpu.dmtc0(cp0::entry_lo0, ((pa >> 12) << entry_lo::pfn_shift) | flags);
    s.cpu.dmtc0(cp0::entry_lo1, global ? entry_lo::g : 0);
    s.cpu.mtc0(cp0::page_mask, 0);
    s.cpu.mtc0(cp0::index, slot);
    s.cpu.tlb_write_indexed();
    s.cpu.dmtc0(cp0::entry_hi, current_asid);
}

// Calls the routine at t9 forever, counting calls in v1.
void load_caller(auto& d) {
    d.load(System::code, code,
           {jalr(ra, t9), nop(), addiu(v1, v1, 1), beq(zero, zero, -16), nop()});
}

void start(auto& d, std::uint64_t routine) {
    d.both([&](System& s) {
        s.start_kernel(code);
        s.set(t9, routine);
    });
}

const test::Registration reuse{
    "mips.block_cache.tight_loop_built_once", [](test::Context& t) {
        Pair<4096> d;
        d.load(System::code, code,
               {addiu(v0, v0, 1), addiu(a0, a0, 3), bne(v0, t0, -8), nop(), beq(zero, zero, -4),
                nop()});
        d.both([](System& s) {
            s.start_kernel(code);
            s.set(t0, 1000);
        });
        d.run(t, 3000);
        const BlockStatistics& s = d.engine.statistics();
        t.check(s.blocks_built <= 2, "the loop body was built once: " + describe(s));
        t.check(s.hits >= 990, "and reused on every iteration: " + describe(s));
        t.check(s.cached_operations > s.operations - 10, "cached operations executed");
    }};

const test::Registration collisions{
    "mips.block_cache.direct_mapped_collisions", [](test::Context& t) {
        // With four entries, blocks 16 bytes apart share an entry: a caller at code and a
        // routine at code + 16 replace each other on every call.
        Pair<4> d;
        d.load(System::code, code,
               {jalr(ra, t9), nop(), beq(zero, zero, -12), nop(), addiu(v0, v0, 1), jr(ra), nop()});
        start(d, code + 16);
        d.run(t, 400);
        const BlockStatistics& s = d.engine.statistics();
        t.check(s.misses[index(CacheMiss::tag)] > 50,
                "collisions replaced entries: " + describe(s));
        t.check_equal(d.candidate.gpr(v0), d.reference.gpr(v0));
    }};

const test::Registration aliases{
    "mips.block_cache.virtual_aliases_of_one_frame", [](test::Context& t) {
        // One routine reached through kseg0 and through a TLB-mapped page (two blocks, one
        // frame); a store through the mapped alias rewrites it, and both blocks run the new
        // code. (kseg0 and kseg1 aliases differ only in high bits and share a direct-mapped
        // entry, so they replace each other instead.)
        Pair<4096> d;
        load_routine(d, frame_a, 1);
        d.load(System::code, code,
               {jalr(ra, s2), nop(), jalr(ra, s3), nop(), sw(t8, 0, s3), jalr(ra, s2), nop(),
                jalr(ra, s3), nop(), beq(zero, zero, -4), nop()});
        d.both([](System& s) {
            s.start_kernel(code);
            map(s, 1, mapped, frame_a, 0, true, 0);
            s.set(s2, kseg0(frame_a));
            s.set(s3, mapped);
            s.set(t8, addiu(v0, v0, 0x100));
        });
        d.run(t, 60);
        t.check_equal(d.candidate.gpr(v0), std::uint64_t{0x202});
        const BlockStatistics& s = d.engine.statistics();
        t.check(s.misses[index(CacheMiss::frame)] >= 2, "both aliases went stale: " + describe(s));
        t.check(s.barriers[index(Cpu::BusAccess::code_store)] >= 1,
                "the store was a barrier: " + describe(s));
    }};

const test::Registration remap{
    "mips.block_cache.tlb_remap_and_unrelated_writes", [](test::Context& t) {
        // The same virtual page remapped by TLB entry 1 to other code: the block goes stale.
        // A write to an unrelated entry does not stale it.
        Pair<4096> d;
        load_routine(d, frame_a, 1);
        load_routine(d, frame_b, 0x10);
        load_caller(d);
        start(d, mapped);
        d.both([](System& s) { map(s, 1, mapped, frame_a, 0, true, 0); });
        d.run(t, 100);
        const std::uint64_t hits = d.engine.statistics().hits;
        d.both([](System& s) { map(s, 5, 0x40000, frame_b, 0, true, 0); });
        d.run(t, 100);
        t.check_equal(d.engine.statistics().misses[index(CacheMiss::tlb)], std::uint64_t{0});
        t.check(d.engine.statistics().hits > hits, "still hitting after an unrelated write");
        d.both([](System& s) { map(s, 1, mapped, frame_b, 0, true, 0); });
        const std::uint64_t before = d.candidate.gpr(v0);
        d.run(t, 100);
        t.check(d.engine.statistics().misses[index(CacheMiss::tlb)] >= 1, "remap staled it");
        t.check((d.candidate.gpr(v0) - before) % 0x10 == 0, "the new routine ran");
    }};

const test::Registration asids{"mips.block_cache.translation_contexts", [](test::Context& t) {
                                   // The same virtual page under two ASIDs maps different code;
                                   // switching the ASID selects the other block (the context is
                                   // part of the key).
                                   Pair<4096> d;
                                   load_routine(d, frame_a, 1);
                                   load_routine(d, frame_b, 0x10);
                                   load_caller(d);
                                   start(d, mapped);
                                   d.both([](System& s) {
                                       map(s, 1, mapped, frame_a, 1, false, 1);
                                       map(s, 2, mapped, frame_b, 2, false, 1);
                                   });
                                   for (unsigned round = 0; round < 6; ++round) {
                                       const unsigned asid = 1 + round % 2;
                                       d.both([&](System& s) { s.cpu.dmtc0(cp0::entry_hi, asid); });
                                       const std::uint64_t before = d.candidate.gpr(v0);
                                       d.run(t, 60);
                                       const std::uint64_t added = d.candidate.gpr(v0) - before;
                                       t.check(asid == 1 ? added % 0x10 != 0 || added == 0
                                                         : added % 0x10 == 0,
                                               "the routine of the current ASID ran");
                                   }
                               }};

const test::Registration self_modifying{
    "mips.block_cache.self_modifying_block", [](test::Context& t) {
        // A store rewrites a later instruction of the executing block: the store retires, the
        // block is left (a code-store barrier), and the stale operation never runs.
        Pair<4096> d;
        d.load(System::code, code,
               {sw(t8, 12, s1), addiu(v0, v0, 1), addiu(v0, v0, 2), addiu(v0, v0, 4),
                beq(zero, zero, -20), nop()});
        d.both([](System& s) {
            s.start_kernel(code);
            s.set(s1, code);
            s.set(t8, addiu(v0, v0, 0x40));
        });
        d.run(t, 60);
        t.check_equal(d.candidate.gpr(v0) % 0x43, std::uint64_t{0});
        const BlockStatistics& s = d.engine.statistics();
        t.check(s.barriers[index(Cpu::BusAccess::code_store)] >= 1,
                "a code-store barrier: " + describe(s));
        t.check(s.misses[index(CacheMiss::frame)] >= 1, "the frame went stale: " + describe(s));
    }};

const test::Registration dma_between_runs{
    "mips.block_cache.dma_between_engine_calls", [](test::Context& t) {
        // A device writes code in memory between runs (as DMA does, through a writable span):
        // the next lookup finds the frame stale.
        Pair<4096> d;
        load_routine(d, frame_a, 1);
        load_caller(d);
        start(d, kseg0(frame_a));
        d.run(t, 100);
        d.both([](System& s) {
            const auto bytes = s.bus.writable_memory_bytes(PhysicalAddress{frame_a}, 4);
            store_unsigned(bytes, addiu(v0, v0, 0x10), ByteOrder::big);
        });
        const std::uint64_t before = d.candidate.gpr(v0);
        d.run(t, 100);
        t.check(d.engine.statistics().misses[index(CacheMiss::frame)] >= 1, "stale frame");
        t.check((d.candidate.gpr(v0) - before) % 0x10 == 0, "the new code ran");
    }};

// A device window whose access writes code into memory (a synchronized access that runs
// DMA, as an IP27 device access can run events).
struct Writer final : MmioTarget {
    std::function<void()> on_access;
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t, AccessWidth) override {
        on_access();
        return 0;
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t, AccessWidth,
                                                std::uint64_t) override {
        on_access();
        return {};
    }
};

template <std::size_t Entries> void device_writes(test::Context& t) {
    // An access inside the block makes a device write a later instruction of that block: the
    // access is a barrier, and the rewritten instruction runs next, not the stale one.
    Pair<Entries> d;
    Writer reference_device;
    Writer candidate_device;
    constexpr std::uint64_t device_base = 0x0800'0000;
    for (auto [system, device] :
         {std::pair{&d.reference, &reference_device}, std::pair{&d.candidate, &candidate_device}}) {
        System& s = *system;
        device->on_access = [&s] {
            const auto bytes = s.bus.writable_memory_bytes(PhysicalAddress{System::code + 8}, 4);
            store_unsigned(bytes, addiu(v0, v0, 0x40), ByteOrder::big);
        };
        invariant(s.bus.map_mmio({PhysicalAddress{device_base}, 0x1000}, *device).has_value(),
                  "test device");
    }
    d.load(System::code, code,
           {lw(t1, 0, t0), addiu(v0, v0, 1), addiu(v0, v0, 2), beq(zero, zero, -16), nop()});
    d.both([](System& s) {
        s.start_kernel(code);
        s.set(t0, kseg0(device_base));
    });
    d.run(t, 60);
    t.check_equal(d.candidate.gpr(v0) % 0x41, std::uint64_t{0});
    const BlockStatistics& s = d.engine.statistics();
    t.check(s.barriers[index(Cpu::BusAccess::device)] >= 1, "a device barrier: " + describe(s));
    if constexpr (Entries > 1) {
        t.check(s.misses[index(CacheMiss::frame)] >= 1, "the frame went stale: " + describe(s));
    }
}

const test::Registration device_writes_code{"mips.block_cache.device_access_writes_code",
                                            device_writes<4096>};
// With one entry every lookup replaces it, including the entry of the block whose access
// invalidated it; under the sanitizer preset this shows no executing block is freed.
const test::Registration device_writes_one_entry{
    "mips.block_cache.invalidation_never_frees_the_executing_block", device_writes<1>};

const test::Registration global_epoch{
    "mips.block_cache.reset_snapshot_and_remapping", [](test::Context& t) {
        Pair<4096> d;
        load_routine(d, frame_a, 1);
        load_caller(d);
        start(d, kseg0(frame_a));
        d.run(t, 60);
        const auto epoch_misses = [&] {
            return d.engine.statistics().misses[index(CacheMiss::epoch)];
        };
        // A snapshot load.
        d.both([](System& s) {
            StateImage image;
            s.cpu.save_state(image);
            s.cpu.load_state(image);
        });
        std::uint64_t seen = epoch_misses();
        d.run(t, 60);
        t.check(epoch_misses() > seen, "a snapshot load staled the blocks");
        // A bus mapping replaced (the same memory, so the program continues).
        d.both([](System& s) {
            invariant(s.bus
                          .remap_memory({PhysicalAddress{System::boot_base}, s.boot.size()}, s.boot,
                                        0, MemoryAccess::read_write)
                          .has_value(),
                      "remap");
        });
        seen = epoch_misses();
        d.run(t, 60);
        t.check(epoch_misses() > seen, "remapping staled the blocks");
        // A reset, then the same code again from the reset vector.
        d.load(System::boot_base, 0xffff'ffff'bfc0'0000,
               {addiu(a0, a0, 1), beq(zero, zero, -8), nop()});
        d.both([](System& s) { s.cpu.reset(ResetKind::power_on); });
        d.run(t, 30);
        d.both([](System& s) { s.cpu.reset(ResetKind::power_on); });
        seen = epoch_misses();
        d.run(t, 30);
        t.check(epoch_misses() > seen, "a reset staled the blocks");
    }};

} // namespace
