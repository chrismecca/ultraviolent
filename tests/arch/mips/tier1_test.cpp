// Tier 1 (JIT.adoc): native code for decoded blocks, against the reference interpreter.

#include "arch/mips/assembler.hpp"
#include "arch/mips/differential.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/native_code.hpp>

#include <cstdint>
#include <vector>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using namespace ultraviolent::mips::testing;

// A loop through `blocks` blocks of a few operations each, each ending in a jump to the next;
// the last jumps back to the first. Placed from System::code.
template <class Engine> void load_block_chain(DifferentialWith<Engine>& d, unsigned blocks) {
    for (unsigned b = 0; b < blocks; ++b) {
        const std::uint64_t at = System::code + 0x20 * b;
        const std::uint64_t next = b + 1 == blocks ? System::code : at + 0x20;
        d.load(at, kseg0(at),
               {addiu(t0, t0, 1), addu(t1, t1, t0), sll(t2, t1, 3), addiu(t3, t3, -1),
                j(kseg0(next)), nop()});
    }
    d.both([](System& s) { s.start_kernel(); });
}

const test::Registration native_runs{"mips.tier1.native_code_runs", [](test::Context& t) {
    DifferentialWith<Tier1Engine> d;
    // One long chunk: shorter ones stop blocks midway, and re-entering mid-block makes another
    // block.
    d.max_chunk = 2000;
    load_block_chain(d, 4);
    t.check(d.run(t, 2000), "matches the reference");
#ifdef ULTRAVIOLENT_TIER1
    const NativeStatistics& n = d.engine.native()->statistics();
    t.check_equal(n.blocks_translated, std::uint64_t{4});
    t.check(d.engine.statistics().operations > d.engine.statistics().entries,
            "native blocks of more than one operation ran");
    t.check_equal(n.discards, std::uint64_t{0});
#endif
}};

// A code region that holds only a few blocks: translating more discards everything and starts
// again, between blocks, and the guest cannot tell (JIT.adoc "Native code: memory, lifetime,
// calls").
using Tier1SmallRegionEngine = Tier1EngineSized<BlockInterpreter::default_cache_entries, 4096>;

const test::Registration discard{"mips.tier1.discard_when_the_region_is_full",
                                 [](test::Context& t) {
    DifferentialWith<Tier1SmallRegionEngine> d;
    load_block_chain(d, 24);
    t.check(d.run(t, 6000), "matches the reference");
#ifdef ULTRAVIOLENT_TIER1
    const NativeStatistics& n = d.engine.native()->statistics();
    t.check(n.discards > 0, "the region filled and was discarded");
    t.check(n.blocks_translated > 24, "blocks translated again after a discard");
#endif
}};

} // namespace
