// Stage C of IR.adoc: decoded instructions executed through the shared semantics against the
// reference interpreter, compared after every cycle.

#include "arch/mips/assembler.hpp"
#include "arch/mips/differential.hpp"
#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/cp0.hpp>

#include <cstdint>
#include <random>
#include <vector>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using namespace ultraviolent::mips::testing;

constexpr std::uint64_t code = kseg0(System::code);

// Kernel mode, 64-bit addressing, CU1 and FR on: the setting of most programs below.
template <class Engine> void start(DifferentialWith<Engine>& d, std::uint32_t extra_status = 0) {
    d.both([&](System& s) {
        s.start_kernel(code, status::kx | status::cu0 << 1 | status::fr | extra_status);
        s.set(s0, kseg0(System::data));
        for (unsigned r = 8; r < 16; ++r) { // t0-t7: distinct, some negative
            s.set(r, (std::uint64_t{r} * 0x9e37'79b9'7f4a'7c15) ^ (r % 3 == 0 ? ~0ull : 0));
        }
    });
}

// Every instruction of the program ran decoded, except where a reference step is expected.
template <class Engine>
void check_decoded(test::Context& t, const DifferentialWith<Engine>& d,
                   std::uint64_t expected_fallbacks = 0) {
    t.check(d.engine.decoded_steps > 0, "decoded instructions executed");
    t.check_equal(d.engine.fallback_steps, expected_fallbacks);
}

template <class Engine> void arithmetic(test::Context& t) {
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    d.load(System::code, code,
           {addu(v0, t0, t1), subu(v1, t2, t3), and_(a0, t0, t4), or_(a1, t1, t5), xor_(a2, t2, t6),
            nor(a3, t3, t7), slt(t8, t0, t1), sltu(t9, t2, t3), sll(v0, t4, 7), srl(v1, t5, 3),
            sra(a0, t6, 31), sllv(a1, t7, t0), srlv(a2, t0, t1), srav(a3, t1, t2),
            daddu(v0, t0, t1), dsubu(v1, t2, t3), dsll(a0, t4, 5), dsrl32(a1, t5, 1),
            dsra32(a2, t6, 3), dsllv(a3, t7, t0), addiu(v0, t0, -5), slti(v1, t1, 100),
            sltiu(a0, t2, -1), andi(a1, t3, 0xff00), ori(a2, t4, 0x1234), xori(a3, t5, 0xffff),
            lui(t8, 0x8001), daddiu(t9, t6, -1), mult(t0, t1), mfhi(v0), mflo(v1), multu(t2, t3),
            dmult(t4, t5), dmultu(t6, t7), div(t0, t1), divu(t2, t3), ddiv(t4, t5), ddivu(t6, t7),
            div(t0, zero), mthi(t0), mtlo(t1), movz(v0, t2, zero), movn(v1, t3, t4),
            // Overflow traps (the handler skips them).
            lui(t8, 0x7fff), ori(t8, t8, 0xffff), addi(t9, t8, 1), add(t9, t8, t8),
            sub(t9, zero, t8), dadd(t9, t8, t8), nop(), beq(zero, zero, -4), nop()});
    start(d);
    d.run(t, 70);
    check_decoded(t, d);
}

const test::Registration arithmetic_predecoded{"mips.differential.predecoded.arithmetic",
                                               arithmetic<PredecodedEngine>};
const test::Registration arithmetic_blocks{"mips.differential.blocks.arithmetic",
                                           arithmetic<BlockSteppingEngine>};

template <class Engine> void sixty_four_bit(test::Context& t) {
    // In User mode MIPS III operations are reserved unless UX is set (UM 17).
    for (const bool ux : {false, true}) {
        DifferentialWith<Engine> d;
        d.load_skip_handlers();
        d.load(System::code, System::code,
               {daddu(v0, t0, t1), dsll(v1, t2, 4), ld(a0, 0, s1), daddiu(a1, t3, 9), dmult(t4, t5),
                addu(a2, t6, t7), nop(), beq(zero, zero, -4), nop()});
        d.both([&](System& s) {
            s.map_user_low();
            s.cpu.mtc0(cp0::status, (2u << status::ksu_shift) | (ux ? status::ux : 0));
            s.jump(System::code);
            s.set(s1, System::data);
            s.set(t0, 5);
        });
        d.run(t, 30);
        check_decoded(t, d);
    }
}

const test::Registration sixty_four_bit_predecoded{
    "mips.differential.predecoded.sixty_four_bit_gating", sixty_four_bit<PredecodedEngine>};
const test::Registration sixty_four_bit_blocks{"mips.differential.blocks.sixty_four_bit_gating",
                                               sixty_four_bit<BlockSteppingEngine>};

template <class Engine> void branches(test::Context& t) {
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    d.load(System::code, code,
           {beq(t0, t0, 12), addiu(v0, v0, 1), addiu(v0, v0, 2), addiu(v0, v0, 4), bne(t0, t0, 12),
            addiu(v1, v1, 1), addiu(v1, v1, 2), blez(t0, 8), addiu(a0, a0, 1), bgtz(t1, 8),
            addiu(a0, a0, 2), addiu(a0, a0, 4), bltzal(t3, 8), addiu(a1, a1, 1), addiu(a1, a1, 2),
            bgezal(zero, 8), addiu(a1, a1, 4), addiu(a1, a1, 8),
            jal(System::code + std::uint64_t{4} * 22), addiu(a2, a2, 1), addiu(a2, a2, 2),
            addiu(a2, a2, 4),
            // 22: return through ra, link in a3 with JALR
            daddiu(t8, ra, 8), jalr(a3, t8), addiu(a2, a2, 8), nop(), nop(), nop(),
            beq(zero, zero, -4), nop()});
    start(d);
    d.run(t, 40);
    check_decoded(t, d);
}

const test::Registration branches_predecoded{
    "mips.differential.predecoded.branches_and_delay_slots", branches<PredecodedEngine>};
const test::Registration branches_blocks{"mips.differential.blocks.branches_and_delay_slots",
                                         branches<BlockSteppingEngine>};

template <class Engine> void likely(test::Context& t) {
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    d.load(System::code, code,
           {beql(t0, t0, 8),
            addiu(v0, v0, 1),
            addiu(v0, v0, 2), // taken: slot runs
            bnel(t0, t0, 8),
            addiu(v1, v1, 1),
            addiu(v1, v1, 2), // not taken: nullified
            blezl(t1, 8),
            addiu(a0, a0, 1),
            addiu(a0, a0, 2),
            bgtzl(t1, 8),
            addiu(a1, a1, 1),
            addiu(a1, a1, 2),
            bltzl(t3, 8),
            addiu(a2, a2, 1),
            addiu(a2, a2, 2),
            bgezall(t3, 8),
            addiu(a3, a3, 1),
            addiu(a3, a3, 2),
            bltzall(zero, 8),
            addiu(t8, t8, 1),
            addiu(t8, t8, 2),
            nop(),
            nop(),
            beq(zero, zero, -4),
            nop()});
    start(d);
    d.run(t, 30);
    check_decoded(t, d);
}

const test::Registration likely_predecoded{"mips.differential.predecoded.branch_likely",
                                           likely<PredecodedEngine>};
const test::Registration likely_blocks{"mips.differential.blocks.branch_likely",
                                       likely<BlockSteppingEngine>};

template <class Engine> void exceptions(test::Context& t) {
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    d.load(System::code, code,
           {syscall(), break_(), teq(t0, t0), tne(t0, t0), tge(t1, t2), tltiu(t3, -1),
            lw(v0, 1, s0), // address error (load)
            sh(v0, 3, s0), // address error (store)
            ld(v0, 0, t0), // an unmapped xkuseg address: TLB refill
            0xec00'0000u,  // reserved opcode (59)
            0x4800'0000u,  // COP2: unusable
            0x4200'0003u,  // reserved CP0 function (CO set, function 3)
            mfc0(v1, cp0::bad_vaddr), mfc0(a0, cp0::cause), nop(), nop(), beq(zero, zero, -4),
            nop()});
    start(d);
    d.run(t, 90);
    // The block engine leaves the three unsupported encodings to reference steps.
    check_decoded(t, d, Engine::decodes_unsupported ? 0 : 3);
}

const test::Registration exceptions_predecoded{"mips.differential.predecoded.exceptions",
                                               exceptions<PredecodedEngine>};
const test::Registration exceptions_blocks{"mips.differential.blocks.exceptions",
                                           exceptions<BlockSteppingEngine>};

template <class Engine> void coprocessor_unusable(test::Context& t) {
    // With CU1 clear, FPU operations and MOVF/MOVT raise coprocessor unusable.
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    d.load(System::code, code,
           {mtc1(t0, 2), add_fmt(fmt_d, 4, 2, 2), ldc1(6, 0, s0), movf(v0, t1, 0), nop(),
            beq(zero, zero, -4), nop()});
    d.both([](System& s) {
        s.start_kernel(code, status::kx);
        s.set(s0, kseg0(System::data));
    });
    d.run(t, 30);
    check_decoded(t, d);
}

const test::Registration coprocessor_unusable_predecoded{
    "mips.differential.predecoded.coprocessor_unusable", coprocessor_unusable<PredecodedEngine>};
const test::Registration coprocessor_unusable_blocks{
    "mips.differential.blocks.coprocessor_unusable", coprocessor_unusable<BlockSteppingEngine>};

template <class Engine> void fpu(test::Context& t) {
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    d.load(System::code, code,
           {dmtc1(t0, 0), dmtc1(t1, 2), add_fmt(fmt_d, 4, 0, 2), mul_fmt(fmt_d, 6, 4, 2),
            sdc1(6, 8, s0), ldc1(8, 8, s0), sub_fmt(fmt_d, 10, 8, 0), c_cond(fmt_d, 2, 0, 2, 0),
            bc1t(0, 8), nop(), addiu(v0, v0, 1), movt(v1, t2, 0), dmfc1(a0, 10), nop(),
            beq(zero, zero, -4), nop()});
    start(d);
    d.run(t, 30);
    check_decoded(t, d);
}

const test::Registration fpu_predecoded{"mips.differential.predecoded.fpu", fpu<PredecodedEngine>};
const test::Registration fpu_blocks{"mips.differential.blocks.fpu", fpu<BlockSteppingEngine>};

template <class Engine> void loads_stores(test::Context& t) {
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    d.load(System::code, code,
           {sd(t0, 0, s0),
            sw(t1, 8, s0),
            sh(t2, 12, s0),
            sb(t3, 15, s0),
            ld(v0, 0, s0),
            lw(v1, 8, s0),
            lwu(a0, 8, s0),
            lh(a1, 12, s0),
            lhu(a2, 12, s0),
            lb(a3, 15, s0),
            lbu(t8, 15, s0),
            lwl(t9, 1, s0),
            lwr(t9, 4, s0),
            ldl(v0, 3, s0),
            ldr(v0, 10, s0),
            swl(t4, 17, s0),
            swr(t5, 22, s0),
            sdl(t6, 25, s0),
            sdr(t7, 38, s0),
            ll(v1, 8, s0),
            sc(t0, 8, s0),
            lld(a0, 0, s0),
            scd(t1, 0, s0),
            sc(t2, 8, s0), // LL bit clear: fails
            ll(a1, 16, s0),
            syscall(),
            sc(t3, 16, s0), // ERET clears the LL bit
            pref(0, 0, s0),
            sync(),
            cache(0x19, 0, s0),
            nop(),
            nop(),
            beq(zero, zero, -4),
            nop()});
    start(d);
    d.run(t, 50);
    check_decoded(t, d);
}

const test::Registration loads_stores_predecoded{"mips.differential.predecoded.loads_and_stores",
                                                 loads_stores<PredecodedEngine>};
const test::Registration loads_stores_blocks{"mips.differential.blocks.loads_and_stores",
                                             loads_stores<BlockSteppingEngine>};

template <class Engine> void count_compare(test::Context& t) {
    // Compare a few cycles ahead with the timer interrupt enabled: the interrupt is taken
    // in the middle of the loop; the handler at the general vector rewrites Compare.
    DifferentialWith<Engine> d;
    d.load(0x180, kseg0(0x180),
           {mfc0(k0, cp0::count), addiu(k0, k0, 7), mtc0(k0, cp0::compare), eret()});
    d.load(System::code, code,
           {mfc0(t0, cp0::count), addiu(t0, t0, 5), mtc0(t0, cp0::compare), addiu(v0, v0, 1),
            addiu(v1, v1, 1), mfc0(a0, cp0::count), beq(zero, zero, -16), nop(),
            beq(zero, zero, -4), nop()});
    d.both([](System& s) {
        s.start_kernel(code, status::kx | status::ie | (0x80u << status::im_shift));
    });
    d.run(t, 300);
    check_decoded(t, d);
}

const test::Registration count_compare_predecoded{
    "mips.differential.predecoded.count_compare_boundaries", count_compare<PredecodedEngine>};
const test::Registration count_compare_blocks{"mips.differential.blocks.count_compare_boundaries",
                                              count_compare<BlockSteppingEngine>};

template <class Engine> void random_and_tlb(test::Context& t) {
    // TLBWR at Random while exceptions (SYSCALL) take cycles that retire nothing; Wired
    // changes restart Random.
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    d.load(System::code, code,
           {addiu(t0, zero, 3), mtc0(t0, cp0::wired), dmtc0(t1, cp0::entry_hi),
            dmtc0(zero, cp0::entry_lo0), dmtc0(zero, cp0::entry_lo1), tlbwr(), syscall(),
            mfc0(v0, cp0::random), daddiu(t1, t1, 0x4000), tlbwr(), tlbp(), mfc0(v1, cp0::index),
            tlbr(), dmfc0(a0, cp0::entry_hi), beq(zero, zero, -48), nop(), beq(zero, zero, -4),
            nop()});
    start(d);
    d.both([](System& s) { s.set(t1, 0x10'0000); });
    d.run(t, 400);
    check_decoded(t, d);
}

const test::Registration random_and_tlb_predecoded{
    "mips.differential.predecoded.random_and_tlb_writes", random_and_tlb<PredecodedEngine>};
const test::Registration random_and_tlb_blocks{"mips.differential.blocks.random_and_tlb_writes",
                                               random_and_tlb<BlockSteppingEngine>};

template <class Engine> void interrupt_lines(test::Context& t) {
    // External interrupt lines raised and lowered between specific cycles.
    DifferentialWith<Engine> d;
    d.load(0x180, kseg0(0x180), {mfc0(k1, cp0::cause), addiu(k0, k0, 1), eret()});
    d.load(System::code, code,
           {addiu(v0, v0, 1), addiu(v1, v1, 1), beq(zero, zero, -8), addiu(a0, a0, 1)});
    d.both([](System& s) {
        s.start_kernel(code, status::kx | status::ie | (0x0cu << status::im_shift));
    });
    d.run(t, 200, [](std::uint64_t n, System& s) {
        if (n == 17 || n == 90) {
            s.cpu.set_interrupt_level(0, true);
        }
        if (n == 19 || n == 150) {
            s.cpu.set_interrupt_level(0, false);
        }
        if (n == 60) {
            s.cpu.set_interrupt_level(1, true);
        }
        if (n == 61) {
            s.cpu.set_interrupt_level(1, false);
        }
    });
    check_decoded(t, d);
}

const test::Registration interrupt_lines_predecoded{"mips.differential.predecoded.interrupt_lines",
                                                    interrupt_lines<PredecodedEngine>};
const test::Registration interrupt_lines_blocks{"mips.differential.blocks.interrupt_lines",
                                                interrupt_lines<BlockSteppingEngine>};

template <class Engine> void fetch_faults(test::Context& t) {
    // Jumps to a misaligned address and to an unmapped user address fault on the fetch;
    // those cycles run as reference steps (nothing is decoded there).
    DifferentialWith<Engine> d;
    d.load(0x180, kseg0(0x180), {dmtc0(s2, cp0::epc), eret()}); // resume at the address in s2
    // TLB refills resume at the halt loop in s3.
    d.load(0x000, kseg0(0x000), {dmtc0(s3, cp0::epc), eret()});
    d.load(0x080, kseg0(0x080), {dmtc0(s3, cp0::epc), eret()});
    d.load(System::code, code,
           {daddiu(t0, s2, -6), jr(t0), nop(), // misaligned target
            addiu(v0, v0, 1), jr(t1), nop(),   // unmapped target
            addiu(v1, v1, 1), nop(), nop(), beq(zero, zero, -4), nop()});
    start(d);
    d.both([](System& s) {
        s.set(s2, code + 12);
        s.set(s3, code + 36);
        s.set(t1, 0x4000'0000);
    });
    d.run(t, 20);
    t.check(d.engine.decoded_steps > 0, "decoded instructions executed");
    t.check_equal(d.engine.fallback_steps, std::uint64_t{2}); // the two faulting fetches
}

const test::Registration fetch_faults_predecoded{"mips.differential.predecoded.fetch_faults",
                                                 fetch_faults<PredecodedEngine>};
const test::Registration fetch_faults_blocks{"mips.differential.blocks.fetch_faults",
                                             fetch_faults<BlockSteppingEngine>};

template <class Engine> void self_modifying(test::Context& t) {
    // Stores rewrite instructions ahead in the same block (through kseg0, the host fast
    // path), including the next one; the new words run, as a fetch at each cycle would see.
    DifferentialWith<Engine> d;
    d.load_skip_handlers();
    d.load(System::code, code,
           {sw(t8, 16, s1), sw(t9, 12, s1), addiu(v0, v0, 1), addiu(v0, v0, 2), addiu(v0, v0, 4),
            addiu(v1, v1, 1), nop(), beq(zero, zero, -4), nop()});
    start(d);
    d.both([](System& s) {
        s.set(s1, code);
        s.set(t8, addiu(v0, v0, 0x40));
        s.set(t9, addiu(v0, v0, 0x20));
    });
    d.run(t, 30);
    t.check_equal(d.candidate.gpr(v0), std::uint64_t{0x61});
    t.check(d.engine.decoded_steps > 0, "decoded instructions executed");
}

const test::Registration self_modifying_blocks{"mips.differential.blocks.self_modifying_code",
                                               self_modifying<BlockSteppingEngine>};

// Random programs over most integer operations, with loads and stores into the data area
// and short forward branches.
std::uint32_t random_instruction(std::mt19937_64& rng) {
    const auto reg = [&] { return static_cast<unsigned>(2 + rng() % 14); }; // v0-t7
    const auto imm = [&] { return static_cast<std::int32_t>(rng() % 0x10000) - 0x8000; };
    const auto offset = [&] { return static_cast<std::int32_t>(rng() % 64); };
    const auto skip = [&] { return static_cast<std::int32_t>(4 * (1 + rng() % 3)); };
    switch (rng() % 40) {
    case 0:
        return addu(reg(), reg(), reg());
    case 1:
        return subu(reg(), reg(), reg());
    case 2:
        return daddu(reg(), reg(), reg());
    case 3:
        return dsubu(reg(), reg(), reg());
    case 4:
        return add(reg(), reg(), reg());
    case 5:
        return dadd(reg(), reg(), reg());
    case 6:
        return and_(reg(), reg(), reg());
    case 7:
        return or_(reg(), reg(), reg());
    case 8:
        return xor_(reg(), reg(), reg());
    case 9:
        return slt(reg(), reg(), reg());
    case 10:
        return sltu(reg(), reg(), reg());
    case 11:
        return sll(reg(), reg(), static_cast<unsigned>(rng() % 32));
    case 12:
        return dsra32(reg(), reg(), static_cast<unsigned>(rng() % 32));
    case 13:
        return srav(reg(), reg(), reg());
    case 14:
        return addiu(reg(), reg(), imm());
    case 15:
        return daddiu(reg(), reg(), imm());
    case 16:
        return addi(reg(), reg(), imm());
    case 17:
        return ori(reg(), reg(), imm() & 0xffff);
    case 18:
        return lui(reg(), static_cast<std::uint32_t>(imm()) & 0xffff);
    case 19:
        return mult(reg(), reg());
    case 20:
        return ddivu(reg(), reg());
    case 21:
        return mflo(reg());
    case 22:
        return mfhi(reg());
    case 23:
        return lw(reg(), offset() & ~3, s0);
    case 24:
        return ld(reg(), offset(), s0); // sometimes misaligned
    case 25:
        return lb(reg(), offset(), s0);
    case 26:
        return lwl(reg(), offset(), s0);
    case 27:
        return ldr(reg(), offset(), s0);
    case 28:
        return sw(reg(), offset() & ~3, s0);
    case 29:
        return sd(reg(), offset() & ~7, s0);
    case 30:
        return sb(reg(), offset(), s0);
    case 31:
        return swr(reg(), offset(), s0);
    case 32:
        return beq(reg(), reg(), skip());
    case 33:
        return bne(reg(), reg(), skip());
    case 34:
        return bgez(reg(), skip());
    case 35:
        return beql(reg(), reg(), skip());
    case 36:
        return bltzl(reg(), skip());
    case 37:
        return movn(reg(), reg(), reg());
    case 38:
        return teq(reg(), reg());
    default:
        return mfc0(reg(), rng() % 2 == 0 ? cp0::count : cp0::random);
    }
}

template <class Engine> void random_programs(test::Context& t) {
    std::mt19937_64 rng{20260929};
    for (int program = 0; program < 60; ++program) {
        DifferentialWith<Engine> d;
        d.load_skip_handlers();
        std::vector<std::uint32_t> words;
        words.reserve(126);
        for (int i = 0; i < 120; ++i) {
            words.push_back(random_instruction(rng));
        }
        // Loop back to the start; forward branches near the end land in the NOPs and the
        // self-loop after it, so every cycle stays in decoded code.
        words.push_back(beq(zero, zero, -4 * static_cast<std::int32_t>(words.size() + 1)));
        for (const std::uint32_t word : {nop(), nop(), nop(), beq(zero, zero, -4), nop()}) {
            words.push_back(word);
        }
        for (std::size_t i = 0; i < words.size(); ++i) {
            d.load(System::code + 4 * i, code + 4 * i, {words[i]});
        }
        start(d);
        if (!d.run(t, 600)) {
            t.check(false, std::format("program {}", program));
            return;
        }
        if constexpr (Engine::decodes_unsupported) {
            check_decoded(t, d);
        } else {
            // A random branch can land in another's delay slot, which the block engine
            // leaves to a reference step (BlockEnd::delay_slot); nothing else falls back.
            t.check(d.engine.decoded_steps > 0, "decoded instructions executed");
            t.check_equal(d.engine.fallback_steps,
                          d.engine.fallback_reasons[static_cast<std::size_t>(NoBlock::delay_slot)]);
        }
    }
}

const test::Registration random_programs_predecoded{"mips.differential.predecoded.random_programs",
                                                    random_programs<PredecodedEngine>};
const test::Registration random_programs_blocks{"mips.differential.blocks.random_programs",
                                                random_programs<BlockSteppingEngine>};

} // namespace
