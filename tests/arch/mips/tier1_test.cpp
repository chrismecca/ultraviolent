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
// boundaries (and so zero divisors, -1, and the most negative words and doublewords for the
// divisions), every branch kind with short forward offsets, and JR/JALR/JAL through
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
    switch (rng() % 70) {
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
    case 61: return mult(reg(), reg());
    case 62: return multu(reg(), reg());
    case 63: return div(reg(), reg());
    case 64: return divu(reg(), reg());
    case 65: return dmult(reg(), reg());
    case 66: return dmultu(reg(), reg());
    case 67: return ddiv(reg(), reg());
    case 68: return ddivu(reg(), reg());
    default: return nop();
    }
}

// The divisions' edges, which random programs rarely reach: the most negative doubleword
// and word divided by -1 (x86 faults on the first), zero divisors (HI and LO unchanged), and
// unsigned divisions of values with bit 63 or bit 31 set; each result moved out of HI and LO.
template <class Engine> void division_edges(test::Context& t) {
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    std::vector<std::uint32_t> words;
    for (const auto& [x, y] : {std::pair{t0, t1}, std::pair{t2, t1}, std::pair{t0, zero},
                               std::pair{t3, t2}, std::pair{t1, t3}, std::pair{t2, t0}}) {
        for (const std::uint32_t op : {ddiv(x, y), div(x, y), ddivu(x, y), divu(x, y),
                                       dmult(x, y), dmultu(x, y), mult(x, y), multu(x, y)}) {
            words.push_back(mthi(a0));
            words.push_back(mtlo(a1));
            words.push_back(op);
            words.push_back(mfhi(v0));
            words.push_back(mflo(v1));
            words.push_back(daddu(a2, a2, v0));
            words.push_back(daddu(a3, a3, v1));
        }
    }
    words.push_back(beq(zero, zero, -4 * static_cast<std::int32_t>(words.size() + 1)));
    words.push_back(nop());
    for (std::size_t i = 0; i < words.size(); ++i) {
        d.load(System::code + 4 * i, kseg0(System::code) + 4 * i, {words[i]});
    }
    d.both([&](System& s) {
        s.start_kernel();
        s.set(t0, 0x8000'0000'0000'0000);
        s.set(t1, 0xffff'ffff'ffff'ffff);
        s.set(t2, 0xffff'ffff'8000'0000);
        s.set(t3, 0x0000'0000'8000'0001);
        s.set(a0, 0x1234'5678'9abc'def0);
        s.set(a1, 0x0fed'cba9'8765'4321);
    });
    t.check(d.run(t, 2 * words.size()), "matches the reference");
}

const test::Registration division_edges_tier0{"mips.tier1.division_edges.tier0",
                                              division_edges<Tier0Engine>};
const test::Registration division_edges_tier1{"mips.tier1.division_edges.tier1",
                                              division_edges<Tier1Engine>};

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

// Loads and stores of every width in builder form, which tier 1 emits with an inline host
// page fast path and a slow path through call_semantics: bases in two data pages (hits once
// filled), in the code frame (stores reach the bus and invalidate blocks), uncached (kseg1:
// always the bus), and unmapped user space (TLB refill, skipped); offsets that misalign
// (address errors, skipped); interleaved with the other builder operations, so guest values
// are cached in host registers across the paths. Memory is compared with the reference too.
std::uint32_t memory_operation(std::mt19937_64& rng) {
    if (rng() % 2 == 0) {
        return builder_operation(rng);
    }
    const unsigned base = 16 + static_cast<unsigned>(rng() % 5); // s0-s4
    const auto offset = static_cast<std::int32_t>(rng() % 64) - 16;
    const unsigned r = static_cast<unsigned>(rng() % 12);
    const unsigned rt = r == 0 ? 0u : r == 1 ? 31u : r + 1;
    switch (rng() % 11) {
    case 0: return lb(rt, offset, base);
    case 1: return lbu(rt, offset, base);
    case 2: return lh(rt, offset & ~1, base);
    case 3: return lhu(rt, offset, base);
    case 4: return lw(rt, offset & ~3, base);
    case 5: return lwu(rt, offset, base);
    case 6: return ld(rt, offset & ~7, base);
    case 7: return sb(rt, offset, base);
    case 8: return sh(rt, offset & ~1, base);
    case 9: return sw(rt, offset & ~3, base);
    default: return sd(rt, rng() % 4 == 0 ? offset : offset & ~7, base);
    }
}

template <class Engine> void memory_operations(test::Context& t) {
    std::mt19937_64 rng{20261011};
    constexpr std::uint64_t edges[] = {0x7fff'ffff,          0x8000'0000,        0xffff'ffff'ffff'ffff,
                                       0x8080'8080'8080'8080, 0x0123'4567'89ab'cdef, 0x80,
                                       0x8000,                0xffff'ffff'8000'0000, 5,
                                       0xffff'ffff'7fff'ffff};
    for (int program = 0; program < 60; ++program) {
        DifferentialWith<Engine> d;
        d.load_skip_handlers();
        std::vector<std::uint32_t> words;
        for (int i = 0; i < 120; ++i) {
            words.push_back(memory_operation(rng));
        }
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
            s.set(16, kseg0(System::data) + 0x20);
            s.set(17, kseg0(System::data) + 0x1000);
            s.set(18, kseg0(System::code) + 0x800);
            s.set(19, kseg1(System::data) + 0x2000);
            s.set(20, 0x1000'0000);
        });
        if (!d.run(t, 1500)) {
            t.check(false, std::format("program {}", program));
            return;
        }
    }
}

// The inline host page path tests the entry's translation context: one user virtual page,
// mapped by two non-global TLB entries (ASIDs 1 and 2) to two frames, loaded and stored in
// turn under each ASID. Without the test, a host page filled under one ASID would serve the
// other.
template <class Engine> void data_pages_by_context(test::Context& t) {
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    constexpr std::uint64_t page = 0x0001'0000;
    d.load(System::code, kseg0(System::code),
           {lw(v0, 0, s0),            // ASID 1: frame A
            sw(v0, 8, s0),
            mtc0(t1, cp0::entry_hi),  // ASID 2
            nop(),
            lw(v1, 0, s0),            // frame B
            daddiu(v1, v1, 1),
            sw(v1, 0, s0),
            mtc0(t0, cp0::entry_hi),  // ASID 1
            nop(),
            lw(a0, 0, s0),
            daddu(v0, v0, a0),
            sd(v0, 16, s0),
            beq(zero, zero, -4 * 13),
            nop()});
    d.both([&](System& s) {
        const std::uint64_t flags = (3u << entry_lo::c_shift) | entry_lo::d | entry_lo::v;
        for (const std::uint64_t asid : {1u, 2u}) {
            const std::uint64_t frame = System::data + 0x1000 * asid;
            s.cpu.dmtc0(cp0::entry_hi, page | asid);
            s.cpu.dmtc0(cp0::entry_lo0, (frame >> 12) << entry_lo::pfn_shift | flags);
            s.cpu.dmtc0(cp0::entry_lo1, 0);
            s.cpu.mtc0(cp0::page_mask, 0);
            s.cpu.mtc0(cp0::index, static_cast<std::uint32_t>(asid));
            s.cpu.tlb_write_indexed();
            s.poke(frame, AccessWidth::bits32, 0x1111'0000 * asid);
        }
        s.start_kernel();
        s.cpu.dmtc0(cp0::entry_hi, 1);
        s.set(s0, page);
        s.set(t0, 1);
        s.set(t1, 2);
    });
    t.check(d.run(t, 600), "matches the reference");
    t.check(d.engine.statistics().operations > 0, "blocks ran");
}

// The FPU's moves, loads, and stores, which tier 1 emits with a guard on Status (CU1, FR) and
// the memory fast path: random registers (odd and even, which differ with FR=0), bases and
// offsets as above (misaligned ones raise address errors), interleaved with integer memory
// operations and FPU arithmetic (still helpers), and Status changed between FR=1, FR=0, and
// coprocessor 1 unusable inside the program, so code translated under one Status runs under
// another. FPU state is compared with the reference after every chunk.
std::uint32_t fpu_operation(std::mt19937_64& rng) {
    const auto f = [&] { return static_cast<unsigned>(rng() % 32); };
    const unsigned base = 16 + static_cast<unsigned>(rng() % 5); // s0-s4
    const auto offset = static_cast<std::int32_t>(rng() % 64) - 16;
    const unsigned r = static_cast<unsigned>(rng() % 12);
    const unsigned rt = r == 0 ? 0u : r == 1 ? 31u : r + 1;
    switch (rng() % 16) {
    case 0: return ldc1(f(), rng() % 4 == 0 ? offset : offset & ~7, base);
    case 1: return sdc1(f(), rng() % 4 == 0 ? offset : offset & ~7, base);
    case 2: return lwc1(f(), rng() % 4 == 0 ? offset : offset & ~3, base);
    case 3: return swc1(f(), rng() % 4 == 0 ? offset : offset & ~3, base);
    case 4: return mfc1(rt, f());
    case 5: return dmfc1(rt, f());
    case 6: return mtc1(rt, f());
    case 7: return dmtc1(rt, f());
    case 8: return mov_fmt(fmt_s, f(), f());
    case 9: return mov_fmt(fmt_d, f(), f());
    case 10: return add_fmt(fmt_d, f(), f(), f());
    case 11: return mul_fmt(fmt_s, f(), f(), f());
    case 12: return rng() % 8 == 0 ? mtc0(21 + static_cast<unsigned>(rng() % 3), cp0::status)
                                   : memory_operation(rng);
    default: return memory_operation(rng);
    }
}

template <class Engine> void fpu_moves(test::Context& t) {
    std::mt19937_64 rng{20261102};
    constexpr std::uint64_t edges[] = {0x7fff'ffff,          0x8000'0000,        0xffff'ffff'ffff'ffff,
                                       0x8080'8080'8080'8080, 0x0123'4567'89ab'cdef, 0x80,
                                       0x8000,                0xffff'ffff'8000'0000, 5,
                                       0x3ff0'0000'0000'0000};
    for (int program = 0; program < 60; ++program) {
        DifferentialWith<Engine> d;
        d.load_skip_handlers();
        std::vector<std::uint32_t> words;
        for (int i = 0; i < 120; ++i) {
            const std::uint32_t word = fpu_operation(rng);
            words.push_back(word);
            if ((word >> 26) == 16) {
                words.push_back(nop()); // after an MTC0 to Status
            }
        }
        words.push_back(beq(zero, zero, -4 * static_cast<std::int32_t>(words.size() + 1)));
        for (const std::uint32_t word : {nop(), nop(), nop(), nop(), beq(zero, zero, -4), nop()}) {
            words.push_back(word);
        }
        for (std::size_t i = 0; i < words.size(); ++i) {
            d.load(System::code + 4 * i, kseg0(System::code) + 4 * i, {words[i]});
        }
        const std::uint32_t fr = program % 2 == 0 ? status::fr : 0;
        d.both([&](System& s) {
            s.start_kernel(kseg0(System::code), status::kx | status::cu1 | fr);
            for (unsigned r = 2; r < 16; ++r) {
                s.set(r, edges[(r + static_cast<unsigned>(program)) % 10]);
            }
            s.set(31, edges[static_cast<unsigned>(program) % 10]);
            s.set(16, kseg0(System::data) + 0x20);
            s.set(17, kseg0(System::data) + 0x1000);
            s.set(18, kseg0(System::code) + 0x800);
            s.set(19, kseg1(System::data) + 0x2000);
            s.set(20, 0x1000'0000);
            s.set(21, status::kx | status::cu1 | (fr ^ status::fr)); // the other FR
            s.set(22, status::kx | fr);                              // coprocessor 1 unusable
            s.set(23, status::kx | status::cu1 | fr);                // back
            for (unsigned n = 0; n < 32; ++n) {
                s.cpu.fpu().fpr[n] = edges[(n + static_cast<unsigned>(program)) % 10] ^ (n * 0x1111);
            }
        });
        if (!d.run(t, 1500)) {
            t.check(false, std::format("program {}", program));
            return;
        }
    }
}

const test::Registration fpu_tier0{"mips.tier1.fpu_moves.tier0", fpu_moves<Tier0Engine>};
const test::Registration fpu_tier1{"mips.tier1.fpu_moves.tier1", fpu_moves<Tier1Engine>};
const test::Registration fpu_tier1_tiny{"mips.tier1.fpu_moves.tier1_tiny_cache",
                                        fpu_moves<Tier1TinyCacheEngine>};

const test::Registration data_pages_tier0{"mips.tier1.data_pages_by_context.tier0",
                                          data_pages_by_context<Tier0Engine>};
const test::Registration data_pages_tier1{"mips.tier1.data_pages_by_context.tier1",
                                          data_pages_by_context<Tier1Engine>};

const test::Registration memory_tier0{"mips.tier1.memory_operations.tier0",
                                      memory_operations<Tier0Engine>};
const test::Registration memory_tier1{"mips.tier1.memory_operations.tier1",
                                      memory_operations<Tier1Engine>};
const test::Registration memory_tier1_tiny{"mips.tier1.memory_operations.tier1_tiny_cache",
                                           memory_operations<Tier1TinyCacheEngine>};

const test::Registration builder_tier0{"mips.tier1.builder_operations.tier0",
                                       builder_operations<Tier0Engine>};
const test::Registration builder_tier1{"mips.tier1.builder_operations.tier1",
                                       builder_operations<Tier1Engine>};
const test::Registration builder_tier1_tiny{"mips.tier1.builder_operations.tier1_tiny_cache",
                                            builder_operations<Tier1TinyCacheEngine>};

const test::Registration builder_regions{"mips.tier1.builder_operations.regions",
                                         builder_operations<Tier1RegionEngine>};
const test::Registration memory_regions{"mips.tier1.memory_operations.regions",
                                        memory_operations<Tier1RegionEngine>};
const test::Registration fpu_regions{"mips.tier1.fpu_moves.regions", fpu_moves<Tier1RegionEngine>};
const test::Registration division_edges_regions{"mips.tier1.division_edges.regions",
                                                division_edges<Tier1RegionEngine>};
const test::Registration native_runs_regions{
    "mips.tier1.native_code_runs.regions", [](test::Context& t) {
        DifferentialWith<Tier1RegionEngine> d;
        d.max_chunk = 2000;
        load_block_chain(d, 4);
        t.check(d.run(t, 2000), "matches the reference");
#ifdef ULTRAVIOLENT_TIER1
        const NativeStatistics& n = d.engine.native()->statistics();
        t.check_equal(n.regions, std::uint64_t{1});
        t.check_equal(n.region_blocks, std::uint64_t{3});
#endif
    }};

} // namespace
