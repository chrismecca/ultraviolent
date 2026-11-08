// Tier 1 (JIT.adoc): native code for decoded blocks, against the reference interpreter.

#include "arch/mips/assembler.hpp"
#include "arch/mips/differential.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/fpu.hpp>
#include <ultraviolent/arch/mips/native_code.hpp>

#include <atomic>
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

// Reads give a value from the offset and the number of reads so far, so a read made twice, or
// not made, changes every later value; the doubleword at 0xff8 answers with an error.
class CountingDevice final : public MmioTarget {
  public:
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth width) override {
        ++reads;
        if ((offset & 0xfff) == 0xff8) {
            return std::unexpected(AccessFault::unmapped);
        }
        const std::uint64_t value = 0xf0e1'd2c3'b4a5'9687 + offset * 0x0101 + reads * 0x1111;
        return width == AccessWidth::bits64
                   ? value
                   : value & ((std::uint64_t{1} << (8 * byte_count(width))) - 1);
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t /*offset*/, AccessWidth /*width*/,
                                                std::uint64_t /*value*/) override {
        return {};
    }
    std::uint64_t reads{};
};

// Loads of every width and sign from device pages, through tier 1's slow path: results, sign
// extension, r0 as destination, a bus error, a second page, and a load in a delay slot.
template <class Engine> void device_loads(test::Context& t) {
    constexpr std::uint64_t device = 0x20'0000;
    DifferentialWith<Engine> d;
    d.max_chunk = 500;
    CountingDevice reference_device;
    CountingDevice candidate_device;
    t.check(d.reference.bus.map_mmio({PhysicalAddress{device}, 0x2000}, reference_device)
                .has_value());
    t.check(d.candidate.bus.map_mmio({PhysicalAddress{device}, 0x2000}, candidate_device)
                .has_value());
    d.load_skip_handlers();
    d.load(System::code, kseg0(System::code),
           {lb(t0, 1, s0), lbu(t1, 0x13, s0), lh(t2, 0x22, s0), lhu(t3, 0x36, s0),
            lw(t4, 0x44, s0), lwu(t5, 0x5c, s0), ld(t6, 0x60, s0), ld(zero, 0x68, s0),
            daddu(t7, t7, t6), ld(t8, 0xff8, s0), lw(t9, 0x1004, s0), lw(v1, 0x48, s0),
            beq(zero, zero, -4 * 13), ld(v0, 0x70, s0)});
    d.both([&](System& s) {
        s.start_kernel();
        s.set(s0, kseg0(device));
    });
    t.check(d.run(t, 3000), "matches the reference");
    t.check_equal(candidate_device.reads, reference_device.reads);
}

// A device whose reads hold (MmioTarget::mmio_read_held) for windows of `period` cycles of
// the reading CPU, until it is written (its stamp). Its value is a function of the offset,
// the width, the window, and the writes so far, so a held read returns exactly what a read
// would, and one answered from the wrong window, offset, width, or stamp is wrong. With
// `hold` false it promises nothing.
class HoldingDevice final : public MmioTarget {
  public:
    HoldingDevice(const Cpu& cpu, std::uint64_t period, bool hold)
        : cpu_{cpu}, period_{period}, hold_{hold} {}
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth width) override {
        ++reads;
        const std::uint64_t value = 0xf0e1'd2c3'b4a5'9687 + offset * 0x0101 +
                                    cpu_.cycles() / period_ * 0x1111 +
                                    stamp_.load() * 0x10'0000'0000;
        return width == AccessWidth::bits64
                   ? value
                   : value & ((std::uint64_t{1} << (8 * byte_count(width))) - 1);
    }
    std::expected<HeldRead, AccessFault> mmio_read_held(std::uint64_t offset,
                                                        AccessWidth width) override {
        const auto value = mmio_read(offset, width);
        if (!hold_) {
            return HeldRead{.value = *value};
        }
        return HeldRead{.value = *value,
                        .until = (cpu_.cycles() / period_ + 1) * period_,
                        .stamp = &stamp_,
                        .stamp_value = stamp_.load()};
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t /*offset*/, AccessWidth /*width*/,
                                                std::uint64_t /*value*/) override {
        ++stamp_;
        return {};
    }
    std::uint64_t reads{};

  private:
    const Cpu& cpu_;
    std::uint64_t period_;
    bool hold_;
    std::atomic<std::uint64_t> stamp_{};
};

// Held device reads (JIT.adoc "Device time"), which tier 1 checks in generated code before
// the slow path: loads of every width and sign, repeated so that some are held, a store that
// changes the stamp, and a load in a delay slot, against the reference; the same number of
// reads reach the device, and fewer than without holding.
template <class Engine> std::uint64_t held_device_reads(test::Context& t, bool hold) {
    constexpr std::uint64_t device = 0x20'0000;
    DifferentialWith<Engine> d;
    d.max_chunk = 500;
    HoldingDevice reference_device{d.reference.cpu, 5, hold};
    HoldingDevice candidate_device{d.candidate.cpu, 5, hold};
    t.check(
        d.reference.bus.map_mmio({PhysicalAddress{device}, 0x1000}, reference_device).has_value());
    t.check(
        d.candidate.bus.map_mmio({PhysicalAddress{device}, 0x1000}, candidate_device).has_value());
    d.load_skip_handlers();
    d.load(System::code, kseg0(System::code),
           {lb(t0, 1, s0),     lb(t1, 1, s0),     lbu(t2, 0x13, s0),
            lbu(t3, 0x13, s0), lh(t4, 0x22, s0),  lh(t5, 0x22, s0),
            lhu(t6, 0x36, s0), lhu(t6, 0x36, s0), lw(t7, 0x44, s0),
            lw(t8, 0x44, s0),  lwu(t9, 0x5c, s0), lwu(t9, 0x5c, s0),
            ld(v0, 0x60, s0),  ld(v1, 0x60, s0),  daddu(a1, a1, v1),
            sd(t0, 0x80, s0),  ld(a0, 0x60, s0),  daddu(a1, a1, a0),
            lw(a2, 0x44, s0),  daddu(a1, a1, a2), beq(zero, zero, -4 * 21),
            ld(a3, 0x60, s0)});
    d.both([&](System& s) {
        s.start_kernel();
        s.set(s0, kseg0(device));
    });
    t.check(d.run(t, 4000), "matches the reference");
    t.check_equal(candidate_device.reads, reference_device.reads);
    return candidate_device.reads;
}
// The same under register pressure: twelve guest values computed (dirty in host registers)
// and live across held loads, so allocating a load's result evicts one; a held read must not
// skip its write-back (measured 2026-11-08: IRIX lost guest values this way).
template <class Engine> void held_reads_under_pressure(test::Context& t) {
    constexpr std::uint64_t device = 0x20'0000;
    DifferentialWith<Engine> d;
    d.max_chunk = 2000;
    HoldingDevice reference_device{d.reference.cpu, 64, true};
    HoldingDevice candidate_device{d.candidate.cpu, 64, true};
    t.check(
        d.reference.bus.map_mmio({PhysicalAddress{device}, 0x1000}, reference_device).has_value());
    t.check(
        d.candidate.bus.map_mmio({PhysicalAddress{device}, 0x1000}, candidate_device).has_value());
    d.load_skip_handlers();
    d.load(System::code, kseg0(System::code), {daddiu(t0, t0, 1),        daddiu(t1, t1, 2),
                                               daddiu(t2, t2, 3),        daddiu(t3, t3, 4),
                                               daddiu(t4, t4, 5),        daddiu(t5, t5, 6),
                                               daddiu(t6, t6, 7),        daddiu(t7, t7, 8),
                                               daddiu(t8, t8, 9),        daddiu(t9, t9, 10),
                                               daddiu(a0, a0, 11),       daddiu(a1, a1, 12),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               ld(v0, 0x60, s0),         daddu(v1, v1, v0),
                                               daddu(a2, a2, t0),        daddu(a2, a2, t1),
                                               daddu(a2, a2, t2),        daddu(a2, a2, t3),
                                               daddu(a2, a2, t4),        daddu(a2, a2, t5),
                                               daddu(a2, a2, t6),        daddu(a2, a2, t7),
                                               daddu(a2, a2, t8),        daddu(a2, a2, t9),
                                               daddu(a2, a2, a0),        daddu(a2, a2, a1),
                                               beq(zero, zero, -4 * 49), nop()});
    d.both([&](System& s) {
        s.start_kernel();
        s.set(s0, kseg0(device));
    });
    t.check(d.run(t, 3000), "matches the reference");
}

template <class Engine> void held_device_reads(test::Context& t) {
    const std::uint64_t held = held_device_reads<Engine>(t, true);
    const std::uint64_t plain = held_device_reads<Engine>(t, false);
    t.check(held < plain * 3 / 4, std::format("held reads: {} of {}", held, plain));
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

// Code under ASIDs `first` and `second`: a loop in kseg0 calls a routine at one virtual page
// and switches between the ASIDs with mtc0 (a helper inside native code, then dispatch and
// region joins). With `global`, one global TLB entry maps the page under both; otherwise
// each ASID's own entry maps a routine of its own, which must be the one that runs. Returns
// the blocks translated.
template <class Engine>
std::uint64_t code_under_asids(test::Context& t, bool global, std::uint64_t first,
                               std::uint64_t second) {
    DifferentialWith<Engine> d;
    d.max_chunk = 2000;
    constexpr std::uint64_t page = 0x0002'0000;
    constexpr std::uint64_t frame = System::data + 0x4000;
    d.load(frame, kseg0(frame), {addiu(v0, v0, 1), jr(ra), nop()});
    d.load(frame + 0x1000, kseg0(frame + 0x1000), {addiu(v1, v1, 1), jr(ra), nop()});
    d.load(System::code, kseg0(System::code),
           {jalr(ra, s1), nop(), mtc0(t1, cp0::entry_hi), nop(), jalr(ra, s1), nop(),
            mtc0(t0, cp0::entry_hi), nop(), beq(zero, zero, -4 * 9), nop()});
    const auto map = [&](System& s, std::uint32_t index, std::uint64_t asid, std::uint64_t pa) {
        const std::uint64_t g = global ? entry_lo::g : 0;
        const std::uint64_t flags = (3u << entry_lo::c_shift) | entry_lo::d | entry_lo::v | g;
        s.cpu.dmtc0(cp0::entry_hi, page | asid);
        s.cpu.dmtc0(cp0::entry_lo0, (pa >> 12) << entry_lo::pfn_shift | flags);
        s.cpu.dmtc0(cp0::entry_lo1, g);
        s.cpu.mtc0(cp0::page_mask, 0);
        s.cpu.mtc0(cp0::index, index);
        s.cpu.tlb_write_indexed();
    };
    d.both([&](System& s) {
        map(s, 3, first, frame);
        if (!global) {
            map(s, 4, second, frame + 0x1000);
        }
        s.start_kernel();
        s.cpu.dmtc0(cp0::entry_hi, first);
        s.set(s1, page);
        s.set(t0, first);
        s.set(t1, second);
    });
    t.check(d.run(t, 4000), "matches the reference");
    t.check(d.candidate.gpr(v0) > 100, "the first ASID's routine ran");
    t.check(global == (d.candidate.gpr(v1) == 0), "the second ASID's own routine ran");
#ifdef ULTRAVIOLENT_TIER1
    return d.engine.native()->statistics().blocks_translated;
#else
    return 0;
#endif
}

// Code that no ASID selects (kseg0, a global TLB entry) has one translation for every ASID
// (Cpu::code_context). ASID 0's context has the bits of that shared form but its marker.
template <class Engine> void code_no_asid_selects(test::Context& t) {
    const std::uint64_t one = code_under_asids<Engine>(t, true, 1, 1);
    const std::uint64_t two = code_under_asids<Engine>(t, true, 1, 2);
    t.check_equal(two, one);
    code_under_asids<Engine>(t, false, 0, 2);
}

// TLB entry 3 maps `page` globally; t0 and t1 hold EntryLo0 values that map it to frames A and
// B, and CP0 is left so that `mtc0 tN, EntryLo0; tlbwi` switches the page to that frame.
constexpr std::uint64_t remapped_page = 0x0003'0000;
constexpr std::uint64_t remap_frame_a = System::data + 0x4000;
constexpr std::uint64_t remap_frame_b = System::data + 0x5000;
void prepare_remap(System& s) {
    const auto lo = [](std::uint64_t frame) {
        return (frame >> 12) << entry_lo::pfn_shift | (3u << entry_lo::c_shift) | entry_lo::d |
               entry_lo::v | entry_lo::g;
    };
    s.cpu.dmtc0(cp0::entry_hi, remapped_page);
    s.cpu.dmtc0(cp0::entry_lo0, lo(remap_frame_a));
    s.cpu.dmtc0(cp0::entry_lo1, entry_lo::g);
    s.cpu.mtc0(cp0::page_mask, 0);
    s.cpu.mtc0(cp0::index, 3);
    s.cpu.tlb_write_indexed();
    s.set(t0, lo(remap_frame_a));
    s.set(t1, lo(remap_frame_b));
}

// A block on a mapped page rewrites the TLB entry that maps it (tlbwi, a helper inside native
// code): what follows comes from the new frame, which holds other code at the same offsets.
template <class Engine> void remap_own_page(test::Context& t) {
    DifferentialWith<Engine> d;
    d.max_chunk = 2000;
    d.load(remap_frame_a, kseg0(remap_frame_a),
           {addiu(v0, v0, 1), mtc0(t1, cp0::entry_lo0), nop(), tlbwi(), nop(), addiu(v1, v1, 1),
            beq(zero, zero, -4 * 7), nop()});
    d.load(remap_frame_b, kseg0(remap_frame_b),
           {addiu(v0, v0, 0x100), mtc0(t0, cp0::entry_lo0), nop(), tlbwi(), nop(),
            addiu(v1, v1, 0x100), beq(zero, zero, -4 * 7), nop()});
    d.both([](System& s) {
        prepare_remap(s);
        s.start_kernel(remapped_page);
    });
    t.check(d.run(t, 3000), "matches the reference");
    const std::uint64_t v = d.candidate.gpr(v0);
    t.check(v % 0x100 > 10 && v / 0x100 > 10, "both frames' code ran");
}

// Code in kseg0 remaps a page (tlbwi) and then calls the routine there through dispatch: the
// dispatch entry from before the remap must not run.
template <class Engine> void remap_called_page(test::Context& t) {
    DifferentialWith<Engine> d;
    d.max_chunk = 2000;
    d.load(remap_frame_a, kseg0(remap_frame_a), {addiu(v0, v0, 1), jr(ra), nop()});
    d.load(remap_frame_b, kseg0(remap_frame_b), {addiu(v1, v1, 1), jr(ra), nop()});
    d.load(System::code, kseg0(System::code),
           {jalr(ra, s1), nop(), mtc0(t1, cp0::entry_lo0), nop(), tlbwi(), nop(), jalr(ra, s1),
            nop(), mtc0(t0, cp0::entry_lo0), nop(), tlbwi(), nop(), beq(zero, zero, -4 * 13),
            nop()});
    d.both([](System& s) {
        prepare_remap(s);
        s.start_kernel();
        s.set(s1, remapped_page);
    });
    t.check(d.run(t, 3000), "matches the reference");
    t.check(d.candidate.gpr(v0) > 10 && d.candidate.gpr(v1) > 10, "both frames' routines ran");
}

// Rewriting a TLB entry that translated code invalidates only that entry's blocks: the others
// keep their dispatch entries without leaving native code to revalidate them.
template <class Engine> void unrelated_tlb_writes(test::Context& t) {
    DifferentialWith<Engine> d;
    d.max_chunk = 2000;
    constexpr std::uint64_t other_page = 0x0004'0000;
    constexpr std::uint64_t other_frame = System::data + 0x6000;
    d.load(remap_frame_a, kseg0(remap_frame_a), {addiu(v0, v0, 1), jr(ra), nop()});
    d.load(other_frame, kseg0(other_frame), {addiu(v1, v1, 1), jr(ra), nop()});
    d.load(
        System::code, kseg0(System::code),
        {jalr(ra, s1), nop(), jalr(ra, s2), nop(), tlbwi(), nop(), beq(zero, zero, -4 * 7), nop()});
    d.both([&](System& s) {
        prepare_remap(s);
        // Entry 5 maps the other page; tlbwi writes it again, unchanged, on every pass.
        s.cpu.dmtc0(cp0::entry_hi, other_page);
        s.cpu.dmtc0(cp0::entry_lo0, (other_frame >> 12) << entry_lo::pfn_shift |
                                        (3u << entry_lo::c_shift) | entry_lo::v | entry_lo::g);
        s.cpu.mtc0(cp0::index, 5);
        s.cpu.tlb_write_indexed();
        s.start_kernel();
        s.set(s1, remapped_page);
        s.set(s2, other_page);
    });
    t.check(d.run(t, 3000), "matches the reference");
    const std::uint64_t passes = d.candidate.gpr(v0);
    t.check(passes > 100 && d.candidate.gpr(v1) + 1 >= passes, "both routines ran on every pass");
    const std::uint64_t revalidations = d.engine.statistics().dispatch_revalidations;
    t.check(revalidations < passes / 4,
            std::format("{} revalidations in {} passes", revalidations, passes));
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

// The FPU's arithmetic, compares, CVT.D.W, and branches, which tier 1 emits with fast paths
// that leave every unusual case to call_semantics: operands at the IEEE edges in both formats
// (signaling and quiet NaNs in MIPS's encoding, infinities, zeros, denormals, values whose
// products overflow or underflow, values whose quotients are inexact, values whose differences
// are exactly denormal from normal operands), FCSR rewritten inside
// the program (every rounding mode, the flush bit, and now and then enabled exceptions, which
// trap), every compare condition on condition codes 0 and 1, all four BC1 forms over short
// skips, and Status switching CU1 off and on. FPU state is compared with the reference.
std::uint32_t fpu_arithmetic_operation(std::mt19937_64& rng) {
    const auto f = [&] { return static_cast<unsigned>(rng() % 20); }; // f0-f19: the edges
    const Format fmt = rng() % 2 == 0 ? fmt_d : fmt_s;
    const auto skip = [&] { return static_cast<std::int32_t>(4 * (1 + rng() % 3)); };
    switch (rng() % 14) {
    case 0: return add_fmt(fmt, f(), f(), f());
    case 1: return sub_fmt(fmt, f(), f(), f());
    case 2: return mul_fmt(fmt, f(), f(), f());
    case 3: return div_fmt(fmt, f(), f(), f());
    case 4:
    case 5: return c_cond(fmt, static_cast<unsigned>(rng() % 16), rng() % 6 == 0 ? 1u : 0u, f(), f());
    case 6: return cvt_d(fmt_w, f(), f());
    case 7: return bc1(rng() % 6 == 0 ? 1u : 0u, rng() % 2 == 0, rng() % 2 == 0, skip());
    case 8: return rng() % 3 == 0 ? ctc1(16 + static_cast<unsigned>(rng() % 6), 31) : nop();
    case 9: return rng() % 8 == 0 ? mtc0(22 + static_cast<unsigned>(rng() % 2), cp0::status) : nop();
    case 10: return mov_fmt(fmt, f(), f());
    case 11: return cfc1(2 + static_cast<unsigned>(rng() % 10), 31);
    default: return add_fmt(fmt_d, f(), f(), f());
    }
}

template <class Engine> void fpu_arithmetic(test::Context& t) {
    std::mt19937_64 rng{20261103};
    // Doubles in the even registers' 64 bits, singles in the odd ones' low halves (FR=1).
    constexpr std::uint64_t doubles[] = {
        0x3ff0'0000'0000'0000, // 1
        0x4008'0000'0000'0000, // 3
        0x7fe0'0000'0000'0000, // 2^1023: products overflow
        0x0010'0000'0000'0000, // the smallest normal: products underflow
        0x0000'0000'0000'0001, // the smallest denormal
        0x7ff0'0000'0000'0000, // infinity
        0x7ff4'0000'0000'0000, // a quiet NaN (MIPS)
        0x7ff8'0000'0000'0000, // a signaling NaN (MIPS)
        0x0018'0000'0000'0000, // 1.5 times the smallest normal: less it, a denormal, exactly
        0x3ff8'0000'0000'0000, // 1.5
    };
    constexpr std::uint32_t singles[] = {
        0x3f80'0000, 0x4040'0000, 0x7f00'0000, 0x0080'0000, 0x0000'0001,
        0x7f80'0000, 0x7fa0'0000, 0x7fc0'0000, 0x00c0'0000, 0x3fc0'0000,
    };
    // FCSR values ctc1 writes: each rounding mode, flush, and enables (inexact, overflow).
    constexpr std::uint64_t fcsrs[] = {0, 1, 2, 3, fcsr::flush, 0x80 | 0x200};
    for (int program = 0; program < 60; ++program) {
        DifferentialWith<Engine> d;
        d.load_skip_handlers();
        std::vector<std::uint32_t> words;
        // First, directed: 1.5 times the smallest normal less the smallest normal, exactly a
        // denormal (Unimplemented Operation with FS=0, zero with FS=1), in both formats, then
        // again with FS set.
        const auto holding = [&](unsigned k, bool single) {
            return 2 * ((k + 10 - static_cast<unsigned>(program) % 10) % 10) + (single ? 1 : 0);
        };
        const unsigned flush = 16 + (4 + 6 - static_cast<unsigned>(program) % 6) % 6;
        for (const std::uint32_t word :
             {sub_fmt(fmt_d, 20, holding(8, false), holding(3, false)),
              sub_fmt(fmt_s, 21, holding(8, true), holding(3, true)), ctc1(flush, 31),
              sub_fmt(fmt_d, 22, holding(8, false), holding(3, false)),
              sub_fmt(fmt_s, 23, holding(8, true), holding(3, true))}) {
            words.push_back(word);
        }
        for (int i = 0; i < 120; ++i) {
            const std::uint32_t word = fpu_arithmetic_operation(rng);
            words.push_back(word);
            if ((word >> 26) == 16 || (word >> 26) == 17) {
                // After MTC0 or a COP1 branch, a word that is no branch.
                words.push_back(i % 2 == 0 ? nop() : add_fmt(fmt_d, 1, 1, 0));
            }
        }
        words.push_back(beq(zero, zero, -4 * static_cast<std::int32_t>(words.size() + 1)));
        for (const std::uint32_t word : {nop(), nop(), nop(), nop(), beq(zero, zero, -4), nop()}) {
            words.push_back(word);
        }
        for (std::size_t i = 0; i < words.size(); ++i) {
            d.load(System::code + 4 * i, kseg0(System::code) + 4 * i, {words[i]});
        }
        d.both([&](System& s) {
            s.start_kernel(kseg0(System::code), status::kx | status::cu1 | status::fr);
            for (unsigned r = 16; r < 22; ++r) {
                s.set(r, fcsrs[(r + static_cast<unsigned>(program)) % 6]);
            }
            s.set(22, status::kx | status::fr);               // coprocessor 1 unusable
            s.set(23, status::kx | status::cu1 | status::fr); // back
            for (unsigned n = 0; n < 20; ++n) {
                const unsigned k = (n / 2 + static_cast<unsigned>(program)) % 10;
                const std::uint64_t sign = (n + static_cast<unsigned>(program)) % 3 == 0
                                               ? 0x8000'0000'0000'0000
                                               : 0;
                s.cpu.fpu().fpr[n] = n % 2 == 0 ? doubles[k] | sign
                                                : singles[k] | (sign != 0 ? 0x8000'0000u : 0u);
            }
        });
        if (!d.run(t, 1500)) {
            t.check(false, std::format("program {}", program));
            return;
        }
    }
}

const test::Registration fpu_arithmetic_tier0{"mips.tier1.fpu_arithmetic.tier0",
                                              fpu_arithmetic<Tier0Engine>};
const test::Registration fpu_arithmetic_tier1{"mips.tier1.fpu_arithmetic.tier1",
                                              fpu_arithmetic<Tier1Engine>};
const test::Registration fpu_arithmetic_regions{"mips.tier1.fpu_arithmetic.regions",
                                                fpu_arithmetic<Tier1RegionEngine>};

const test::Registration fpu_tier0{"mips.tier1.fpu_moves.tier0", fpu_moves<Tier0Engine>};
const test::Registration fpu_tier1{"mips.tier1.fpu_moves.tier1", fpu_moves<Tier1Engine>};
const test::Registration fpu_tier1_tiny{"mips.tier1.fpu_moves.tier1_tiny_cache",
                                        fpu_moves<Tier1TinyCacheEngine>};

const test::Registration data_pages_tier0{"mips.tier1.data_pages_by_context.tier0",
                                          data_pages_by_context<Tier0Engine>};
const test::Registration data_pages_tier1{"mips.tier1.data_pages_by_context.tier1",
                                          data_pages_by_context<Tier1Engine>};
const test::Registration code_no_asid_tier1{"mips.tier1.code_no_asid_selects.tier1",
                                            code_no_asid_selects<Tier1Engine>};
const test::Registration code_no_asid_regions{"mips.tier1.code_no_asid_selects.regions",
                                              code_no_asid_selects<Tier1RegionEngine>};
const test::Registration remap_own_tier0{"mips.tier1.remap_own_page.tier0",
                                         remap_own_page<Tier0Engine>};
const test::Registration remap_own_tier1{"mips.tier1.remap_own_page.tier1",
                                         remap_own_page<Tier1Engine>};
const test::Registration remap_own_regions{"mips.tier1.remap_own_page.regions",
                                           remap_own_page<Tier1RegionEngine>};
const test::Registration remap_called_tier1{"mips.tier1.remap_called_page.tier1",
                                            remap_called_page<Tier1Engine>};
const test::Registration remap_called_regions{"mips.tier1.remap_called_page.regions",
                                              remap_called_page<Tier1RegionEngine>};
const test::Registration unrelated_tier1{"mips.tier1.unrelated_tlb_writes.tier1",
                                         unrelated_tlb_writes<Tier1Engine>};
const test::Registration unrelated_regions{"mips.tier1.unrelated_tlb_writes.regions",
                                           unrelated_tlb_writes<Tier1RegionEngine>};

const test::Registration device_loads_tier0{"mips.tier1.device_loads.tier0",
                                            device_loads<Tier0Engine>};
const test::Registration device_loads_tier1{"mips.tier1.device_loads.tier1",
                                            device_loads<Tier1Engine>};
const test::Registration device_loads_regions{"mips.tier1.device_loads.regions",
                                              device_loads<Tier1RegionEngine>};

const test::Registration held_reads_tier0{"mips.tier1.held_device_reads.tier0",
                                          held_device_reads<Tier0Engine>};
const test::Registration held_reads_tier1{"mips.tier1.held_device_reads.tier1",
                                          held_device_reads<Tier1Engine>};
const test::Registration held_reads_regions{"mips.tier1.held_device_reads.regions",
                                            held_device_reads<Tier1RegionEngine>};

const test::Registration held_pressure_tier1{"mips.tier1.held_reads_under_pressure.tier1",
                                             held_reads_under_pressure<Tier1Engine>};
const test::Registration held_pressure_regions{"mips.tier1.held_reads_under_pressure.regions",
                                               held_reads_under_pressure<Tier1RegionEngine>};

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
