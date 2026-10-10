// The processor personality (ADR-022): the R10000 model, the one implementation.

#include "arch/mips/assembler.hpp"
#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/cp0.hpp>
#include <ultraviolent/arch/mips/decoded.hpp>
#include <ultraviolent/arch/mips/processor_model.hpp>

#include <cstdint>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using namespace ultraviolent::mips::testing;

const test::Registration r10000_model{"mips.processor_model.r10000", [](test::Context& t) {
    const ProcessorModel model = r10000(CpuConfig{.processor_id = 0x0934});
    t.check(model.name() == "R10000");
    t.check_equal(model.identity().processor_id, std::uint32_t{0x0934});
    t.check(model.supports(Feature::mips3) && model.supports(Feature::mips4), "MIPS IV");
    // Every MIPS IV encoding decodes as the ISA defines it.
    t.check(model.operations() == mips::detail::mips_iv_operations, "decode table");
    t.check(decode(model, movz(t0, t1, t2)).opcode == DecodedOpcode::movz);
    t.check(decode(model, dadd(t0, t1, t2)).opcode == DecodedOpcode::dadd);
    t.check_equal(model.count_shift(), 1u);
}};

const test::Registration count_rate{"mips.processor_model.count_rate", [](test::Context& t) {
    // Count advances once every 2^count_shift cycles: on the R10000, every other cycle.
    System s;
    s.load(System::code, {nop(), nop(), nop(), nop(), nop(), nop(), nop(), nop()});
    s.start_kernel();
    s.cpu.mtc0(cp0::count, 100);
    const std::uint64_t start = s.cpu.cycles();
    s.interpreter.run_until(start + 6);
    t.check_equal(s.cpu.mfc0(cp0::count), std::uint64_t{103});
    t.check_equal(std::uint64_t{s.cpu.model().identity().processor_id}, s.cpu.mfc0(cp0::prid));
}};

} // namespace
