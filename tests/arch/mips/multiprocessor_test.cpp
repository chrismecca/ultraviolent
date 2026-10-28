// Two CPUs sharing one bus through a Coherence (SMP.adoc "Memory, LL/SC, SYNC"), running one
// at a time as in reference mode (ADR-023), or each on its own host thread (SMP.adoc "S4").
#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/block_interpreter.hpp>
#include <ultraviolent/arch/mips/coherence.hpp>

#include <cstdint>
#include <initializer_list>
#include <thread>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using testing::kseg0;
using testing::System;

constexpr std::uint64_t data = System::data;
// CPU B's program, away from CPU A's at System::code.
constexpr std::uint64_t code_b = 0x2000;

// CPU A is the System's CPU; CPU B is a second processor on its bus, requester 1.
struct Pair {
    Pair() {
        s.cpu.connect_coherence(coherence);
        b.connect_coherence(coherence);
        coherence.attach(0, s.cpu);
        coherence.attach(1, b);
        s.set(a0, kseg0(data));
        b.set_gpr(a0, kseg0(data));
    }

    // Loads each CPU's program and starts both in kernel mode.
    void load(std::initializer_list<std::uint32_t> program_a,
              std::initializer_list<std::uint32_t> program_b) {
        s.load(System::code, program_a);
        s.load(code_b, program_b);
        s.start_kernel();
        b.mtc0(cp0::status, status::kx);
        b.state().pc = kseg0(code_b);
        b.state().next_pc = kseg0(code_b) + 4;
    }

    System s;
    Cpu b{s.bus, s.tracer, r10000({}), 1};
    Interpreter interpreter_b{b};
    Coherence coherence;
};

const test::Registration broken_by_other_cpu{
    "mips.smp.store_by_other_cpu_breaks_reservation", [](test::Context& t) {
        Pair p;
        p.s.poke(data, AccessWidth::bits32, 5);
        p.s.set(t1, 7);
        p.b.set_gpr(t2, 9);
        p.load({ll(t0, 0, a0), sc(t1, 0, a0)}, {sw(t2, 0, a0)});
        p.s.interpreter.run(1); // A: LL
        p.interpreter_b.run(1); // B: store to the reserved line
        p.s.interpreter.run(1); // A: SC fails
        t.check_equal(p.s.gpr(t0), std::uint64_t{5});
        t.check_equal(p.s.gpr(t1), std::uint64_t{0});
        t.check_equal(p.s.peek(data, AccessWidth::bits32), std::uint64_t{9});
    }};

const test::Registration other_line{
    "mips.smp.store_to_another_line_keeps_reservation", [](test::Context& t) {
        Pair p;
        p.s.set(t1, 7);
        p.load({ll(t0, 0, a0), sc(t1, 0, a0)},
               {sw(t2, static_cast<std::int16_t>(Coherence::line_size), a0)});
        p.s.interpreter.run(1);
        p.interpreter_b.run(1);
        p.s.interpreter.run(1);
        t.check_equal(p.s.gpr(t1), std::uint64_t{1});
        t.check_equal(p.s.peek(data, AccessWidth::bits32), std::uint64_t{7});
    }};

// CPU B caches the frame as a writable host page before A's LL; A's reservation makes B drop
// it, so B's next store reaches the bus and breaks the reservation.
const test::Registration cached_writable_page{
    "mips.smp.reservation_reaches_cached_writable_page", [](test::Context& t) {
        Pair p;
        p.s.set(t1, 7);
        p.load({ll(t0, 0, a0), sc(t1, 0, a0)}, {sw(t2, 256, a0), sw(t2, 0, a0)});
        p.interpreter_b.run(1); // B: caches the data frame as writable
        p.s.interpreter.run(1); // A: LL
        p.interpreter_b.run(1); // B: store to the reserved line
        p.s.interpreter.run(1); // A: SC
        t.check_equal(p.s.gpr(t1), std::uint64_t{0});
    }};

// CPU A runs decoded blocks from a frame that CPU B holds as a writable host page; B then
// rewrites the code, and A must run the new instruction (IR.adoc "Keeping blocks valid").
const test::Registration code_changed_by_other_cpu{
    "mips.smp.code_store_by_other_cpu_invalidates_blocks", [](test::Context& t) {
        Pair p;
        // A: v0 = 1, forever. B: touch the code frame, then replace A's first instruction
        // with v0 = 2.
        p.b.set_gpr(a1, kseg0(System::code));
        p.b.set_gpr(t3, ori(v0, zero, 2));
        p.load({ori(v0, zero, 1), beq(zero, zero, -8), nop()},
               {sw(zero, 0x100, a1), sw(t3, 0, a1)});
        BlockInterpreter blocks{p.s.cpu};
        p.interpreter_b.run(1); // B: the code frame is a writable host page
        blocks.run_until(p.s.cpu.cycles() + 30);
        t.check_equal(p.s.gpr(v0), std::uint64_t{1});
        p.interpreter_b.run(1); // B: rewrite A's code
        p.s.set(v0, 0);
        blocks.run_until(p.s.cpu.cycles() + 30);
        t.check_equal(p.s.gpr(v0), std::uint64_t{2});
    }};

// A primary data line made valid by Index Store Tag holds data only that CPU's cache has
// (UM chapter 10; the IP27 PROM's first stack, IP27.adoc "Two CPUs on a Hub"): two CPUs see
// their own data at one physical address, and memory gets a line only when it is written
// back.
const test::Registration private_cache_lines{
    "mips.smp.cache_lines_installed_by_software_are_private", [](test::Context& t) {
        Pair p;
        // Way 0 of the set for `data`: tag PA 39:12, DirtyExclusive; StateMod Normal.
        constexpr std::uint32_t tag_lo = (data >> 12) << 8 | 3u << 6;
        constexpr std::uint32_t tag_hi = 1u << 29;
        for (Cpu* cpu : {&p.s.cpu, &p.b}) {
            cpu->mtc0(cp0::tag_lo, tag_lo);
            cpu->mtc0(cp0::tag_hi, tag_hi);
        }
        p.s.set(t1, 0x1111);
        p.b.set_gpr(t1, 0x2222);
        const auto program = {cache(0x9, 0, a0), sw(t1, 0, a0), lw(t2, 0, a0), cache(0x1, 0, a0),
                              lw(t3, 0, a0)};
        p.load(program, program);
        p.s.interpreter.run(3);
        p.interpreter_b.run(3);
        t.check_equal(p.s.gpr(t2), std::uint64_t{0x1111});
        t.check_equal(p.b.state().gpr[t2], std::uint64_t{0x2222});
        t.check_equal(p.s.peek(data, AccessWidth::bits32), std::uint64_t{0});
        // A's Index WriteBack Invalidate sends its line to memory; B's stays in B's cache.
        p.s.interpreter.run(2);
        t.check_equal(p.s.peek(data, AccessWidth::bits32), std::uint64_t{0x1111});
        t.check_equal(p.s.gpr(t3), std::uint64_t{0x1111});
        p.interpreter_b.run(2);
        t.check_equal(p.s.peek(data, AccessWidth::bits32), std::uint64_t{0x2222});
        t.check_equal(p.b.state().gpr[t3], std::uint64_t{0x2222});
    }};

// Host threads: the CPUs switch to compare-and-swap LL/SC.
void make_parallel(Pair& p) {
    p.s.cpu.set_parallel(true);
    p.b.set_parallel(true);
}

// Two CPUs on two host threads add 1 to one counter 20,000 times each with LL/SC loops; no
// increment is lost.
const test::Registration parallel_counter{
    "mips.smp.parallel_llsc_counter", [](test::Context& t) {
        Pair p;
        make_parallel(p);
        constexpr std::uint64_t increments = 20'000;
        const auto program = {ll(t0, 0, a0),        addiu(t0, t0, 1),       sc(t0, 0, a0),
                              beq(t0, zero, -16),   nop(),                  addiu(t1, t1, -1),
                              bne(t1, zero, -28),   nop(),                  beq(zero, zero, -4),
                              nop()};
        p.s.set(t1, increments);
        p.b.set_gpr(t1, increments);
        p.load(program, program);
        {
            std::jthread b{[&p] { p.interpreter_b.run(2'000'000); }};
            p.s.interpreter.run(2'000'000);
        }
        t.check_equal(p.s.gpr(t1), std::uint64_t{0});
        t.check_equal(p.b.state().gpr[t1], std::uint64_t{0});
        t.check_equal(p.s.peek(data, AccessWidth::bits32), 2 * increments);
    }};

// Host threads: while CPU B is parked, A's first mark of a code frame does not wait for it;
// B drops its writable pages when it unparks, so its rewrite of A's code reaches the bus and
// A runs the new instruction.
const test::Registration parked_cpu{
    "mips.smp.parked_cpu_drops_writable_pages", [](test::Context& t) {
        Pair p;
        make_parallel(p);
        p.b.set_gpr(a1, kseg0(System::code));
        p.b.set_gpr(t3, ori(v0, zero, 2));
        p.load({ori(v0, zero, 1), beq(zero, zero, -8), nop()},
               {sw(zero, 0x100, a1), sw(t3, 0, a1)});
        BlockInterpreter blocks{p.s.cpu};
        p.interpreter_b.run(1); // B: the code frame is a writable host page
        p.b.park();
        blocks.run_until(p.s.cpu.cycles() + 30); // A claims the frame without waiting
        t.check_equal(p.s.gpr(v0), std::uint64_t{1});
        p.b.unpark();
        p.interpreter_b.run(1); // B: rewrite A's code
        // B stops running; A's new first mark of the frame must not wait for it.
        p.b.park();
        p.s.set(v0, 0);
        blocks.run_until(p.s.cpu.cycles() + 30);
        t.check_equal(p.s.gpr(v0), std::uint64_t{2});
    }};

// What another thread posts takes effect at the CPU's next instruction boundary: interrupt
// levels, then a reset.
const test::Registration posted{
    "mips.smp.posted_interrupts_and_reset", [](test::Context& t) {
        Pair p;
        make_parallel(p);
        p.load({nop(), nop(), nop()}, {nop()});
        // IE and IM2, the mask bit of external input 0.
        p.s.cpu.mtc0(cp0::status, status::kx | status::ie | 1u << 10);
        p.s.cpu.post_interrupt_levels(1);
        t.check(p.s.cpu.attention_raised());
        p.s.interpreter.run(1);
        t.check((p.s.cpu.mfc0(cp0::status) & status::exl) != 0, "the interrupt was taken");
        t.check((p.s.cpu.mfc0(cp0::cause) >> 10 & 1) != 0, "IP2 is set");
        p.s.cpu.post_interrupt_levels(0);
        p.s.interpreter.run(1);
        t.check((p.s.cpu.mfc0(cp0::cause) >> 10 & 1) == 0, "IP2 is clear");
        p.s.cpu.post_reset(ResetKind::cold, 0);
        p.s.interpreter.run(1);
        t.check_equal(p.s.cpu.state().pc, Cpu::reset_vector);
        t.check(!p.s.cpu.attention_raised());
    }};

} // namespace
