// Tier 1 (JIT.adoc): native code for decoded blocks, against the reference interpreter.

#include "arch/mips/assembler.hpp"
#include "arch/mips/differential.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/native_code.hpp>

#include <cstdint>
#include <format>
#include <random>
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
    // Every operation of the chain is in builder form: all emitted natively.
    t.check_equal(n.native_operations, std::uint64_t{4 * 6});
    t.check_equal(n.helper_operations, std::uint64_t{0});
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

// Every operation in builder form (operations.hpp), which tier 1 emits natively: random
// registers including 0 (a constant) and 31 (the link), values at the overflow and sign
// boundaries, every branch kind with short forward offsets, and JR/JALR/JAL through
// registers. The reference interpreter executes the same operations through the direct
// builder; any difference between the two builders shows here.
std::uint32_t builder_operation(std::mt19937_64& rng) {
    const auto reg = [&] {
        const unsigned r = static_cast<unsigned>(rng() % 12);
        return r == 0 ? 0u : r == 1 ? 31u : r + 1; // zero, ra, v0-t3
    };
    const auto imm = [&] {
        constexpr std::int32_t edges[] = {0, 1, -1, 0x7fff, -0x8000, 0x10};
        return rng() % 3 == 0 ? edges[rng() % 6]
                              : static_cast<std::int32_t>(rng() % 0x10000) - 0x8000;
    };
    const auto uimm = [&] { return static_cast<std::uint32_t>(rng() % 0x10000); };
    const auto sa = [&] { return static_cast<unsigned>(rng() % 32); };
    const auto skip = [&] { return static_cast<std::int32_t>(4 * (1 + rng() % 3)); };
    switch (rng() % 62) {
    case 0: return sll(reg(), reg(), sa());
    case 1: return srl(reg(), reg(), sa());
    case 2: return sra(reg(), reg(), sa());
    case 3: return sllv(reg(), reg(), reg());
    case 4: return srlv(reg(), reg(), reg());
    case 5: return srav(reg(), reg(), reg());
    case 6: return dsll(reg(), reg(), sa());
    case 7: return dsrl(reg(), reg(), sa());
    case 8: return dsra(reg(), reg(), sa());
    case 9: return dsll32(reg(), reg(), sa());
    case 10: return dsrl32(reg(), reg(), sa());
    case 11: return dsra32(reg(), reg(), sa());
    case 12: return dsllv(reg(), reg(), reg());
    case 13: return dsrlv(reg(), reg(), reg());
    case 14: return dsrav(reg(), reg(), reg());
    case 15: return mfhi(reg());
    case 16: return mthi(reg());
    case 17: return mflo(reg());
    case 18: return mtlo(reg());
    case 19: return add(reg(), reg(), reg());
    case 20: return addu(reg(), reg(), reg());
    case 21: return sub(reg(), reg(), reg());
    case 22: return subu(reg(), reg(), reg());
    case 23: return and_(reg(), reg(), reg());
    case 24: return or_(reg(), reg(), reg());
    case 25: return xor_(reg(), reg(), reg());
    case 26: return nor(reg(), reg(), reg());
    case 27: return slt(reg(), reg(), reg());
    case 28: return sltu(reg(), reg(), reg());
    case 29: return dadd(reg(), reg(), reg());
    case 30: return daddu(reg(), reg(), reg());
    case 31: return dsub(reg(), reg(), reg());
    case 32: return dsubu(reg(), reg(), reg());
    case 33: return addi(reg(), reg(), imm());
    case 34: return addiu(reg(), reg(), imm());
    case 35: return daddi(reg(), reg(), imm());
    case 36: return daddiu(reg(), reg(), imm());
    case 37: return slti(reg(), reg(), imm());
    case 38: return sltiu(reg(), reg(), imm());
    case 39: return andi(reg(), reg(), uimm());
    case 40: return ori(reg(), reg(), uimm());
    case 41: return xori(reg(), reg(), uimm());
    case 42: return lui(reg(), uimm());
    case 43: return beq(reg(), reg(), skip());
    case 44: return bne(reg(), reg(), skip());
    case 45: return blez(reg(), skip());
    case 46: return bgtz(reg(), skip());
    case 47: return bltz(reg(), skip());
    case 48: return bgez(reg(), skip());
    case 49: return beql(reg(), reg(), skip());
    case 50: return bnel(reg(), reg(), skip());
    case 51: return blezl(reg(), skip());
    case 52: return bgtzl(reg(), skip());
    case 53: return bltzl(reg(), skip());
    case 54: return bgezl(reg(), skip());
    case 55: return bltzal(reg(), skip());
    case 56: return bgezal(reg(), skip());
    case 57: return bltzall(reg(), skip());
    case 58: return bgezall(reg(), skip());
    case 59: return dsra32(reg(), reg(), sa());
    case 60: return dsrl32(reg(), reg(), sa());
    default: return nop();
    }
}

template <class Engine> void builder_operations(test::Context& t) {
    std::mt19937_64 rng{20261010};
    constexpr std::uint64_t edges[] = {0x7fff'ffff,          0x8000'0000,        0xffff'ffff'ffff'ffff,
                                       0xffff'ffff,          0x8000'0000'0000'0000, 0x7fff'ffff'ffff'ffff,
                                       0x0000'0001'0000'0000, 0xffff'ffff'8000'0000, 5,
                                       0xffff'ffff'7fff'ffff};
    for (int program = 0; program < 80; ++program) {
        DifferentialWith<Engine> d;
        d.load_skip_handlers();
        std::vector<std::uint32_t> words;
        for (int i = 0; i < 100; ++i) {
            words.push_back(builder_operation(rng));
        }
        // JALR, JR, and JAL, each over a skipped word, through t9 (not a random register):
        //   A: lui/ori t9 = A+20; A+8: jalr ra, t9 (slot A+12; A+16 skipped)
        //   A+20: daddiu t9, t9, 12; A+24: jr t9 (slot A+28) -> A+32
        //   A+32: jal A+44 (slot A+36; A+40 skipped) -> A+44
        const std::uint64_t base = kseg0(System::code) + 4 * words.size();
        for (const std::uint32_t word :
             {lui(25, static_cast<std::uint32_t>((base + 20) >> 16) & 0xffff),
              ori(25, 25, static_cast<std::uint32_t>(base + 20) & 0xffff), jalr(31, 25), nop(),
              nop(), daddiu(25, 25, 12), jr(25), nop(), jal(base + 44), nop(), nop()}) {
            words.push_back(word);
        }
        // Loop back to the start; forward branches near the end land in the NOPs and the
        // self-loop after them.
        words.push_back(beq(zero, zero, -4 * static_cast<std::int32_t>(words.size() + 1)));
        for (const std::uint32_t word : {nop(), nop(), nop(), nop(), beq(zero, zero, -4), nop()}) {
            words.push_back(word);
        }
        for (std::size_t i = 0; i < words.size(); ++i) {
            d.load(System::code + 4 * i, kseg0(System::code) + 4 * i, {words[i]});
        }
        d.both([&](System& s) {
            s.start_kernel();
            for (unsigned r = 2; r < 16; ++r) {
                s.set(r, edges[(r + static_cast<unsigned>(program)) % 10]);
            }
            s.set(31, edges[static_cast<unsigned>(program) % 10]);
        });
        if (!d.run(t, 900)) {
            t.check(false, std::format("program {}", program));
            return;
        }
    }
}

const test::Registration builder_tier0{"mips.tier1.builder_operations.tier0",
                                       builder_operations<Tier0Engine>};
const test::Registration builder_tier1{"mips.tier1.builder_operations.tier1",
                                       builder_operations<Tier1Engine>};
const test::Registration builder_tier1_tiny{"mips.tier1.builder_operations.tier1_tiny_cache",
                                            builder_operations<Tier1TinyCacheEngine>};

} // namespace
