// Two CPUs sharing one bus through a Coherence (SMP.adoc "Memory, LL/SC, SYNC"), running one
// at a time as in reference mode (ADR-023).
#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/block_interpreter.hpp>
#include <ultraviolent/arch/mips/coherence.hpp>

#include <cstdint>
#include <initializer_list>

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

} // namespace
