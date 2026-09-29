// Execution-boundary behavior of the reference interpreter that every execution engine must
// reproduce (IR.adoc "Execution rules every engine keeps").

#include "arch/mips/assembler.hpp"
#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/cp0.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <utility>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using namespace ultraviolent::mips::testing;

// A device register window that reports every access, the way a synchronized device access
// can schedule events or change interrupt lines in the middle of an instruction.
struct Device final : MmioTarget {
    std::function<void()> on_access;
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t /*offset*/,
                                                        AccessWidth /*width*/) override {
        if (on_access) {
            on_access();
        }
        return 0x1234;
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t /*offset*/, AccessWidth /*width*/,
                                                std::uint64_t /*value*/) override {
        if (on_access) {
            on_access();
        }
        return {};
    }
};

constexpr std::uint32_t code_of(ExceptionCode code) {
    return static_cast<std::uint32_t>(std::to_underlying(code));
}

constexpr std::uint64_t device_base = 0x0800'0000;

void attach(System& s, Device& device) {
    const std::uint64_t windows[] = {
        0,
        system_address::uncached_window(0),
        system_address::uncached_window(1),
        system_address::uncached_window(2),
        system_address::uncached_window(3),
    };
    for (const std::uint64_t window : windows) {
        invariant(
            s.bus.map_mmio({PhysicalAddress{window + device_base}, 0x1000}, device).has_value(),
            "test device mapping");
    }
}

const test::Registration limit_lowered{
    "mips.run_until_limit_lowered_during_an_access", [](test::Context& t) {
        // A device access during an instruction lowers the run limit to the cycle in progress:
        // the instruction completes and the run stops at the next boundary.
        System s;
        Device device;
        attach(s, device);
        std::uint64_t limit = 100;
        device.on_access = [&] { limit = s.cpu.cycles(); };
        s.set(t0, kseg1(device_base));
        s.load(System::code, {nop(), nop(), lw(t1, 0, t0), nop(), nop(), nop()});
        s.start_kernel();
        s.interpreter.run_until(limit);
        t.check_equal(s.cpu.cycles(), std::uint64_t{3});
        t.check_equal(s.gpr(t1), std::uint64_t{0x1234});
        t.check_equal(s.pc(), kseg0(System::code) + 12);
    }};

const test::Registration interrupt_from_access{
    "mips.interrupt_raised_during_an_access", [](test::Context& t) {
        // An interrupt line raised by a device during an instruction's access is taken at the
        // next boundary. That cycle retires nothing: Random holds, and the instruction after
        // the access has not executed.
        System s;
        Device device;
        attach(s, device);
        device.on_access = [&] { s.cpu.set_interrupt_level(0, true); };
        s.set(t0, kseg1(device_base));
        s.load(System::code, {nop(), lw(t1, 0, t0), addiu(t2, zero, 1), nop()});
        s.start_kernel(kseg0(System::code), status::kx | status::ie | (0x04u << status::im_shift));
        s.interpreter.run(2);
        t.check_equal(s.gpr(t1), std::uint64_t{0x1234});
        t.check_equal(s.pc(), kseg0(System::code) + 8);
        const std::uint64_t random = s.cpu.mfc0(cp0::random);
        s.interpreter.step();
        t.check_equal(s.pc(), general_vector);
        t.check_equal(s.exception_code(), code_of(ExceptionCode::interrupt));
        t.check_equal(s.cpu.dmfc0(cp0::epc), kseg0(System::code) + 8);
        t.check_equal(s.gpr(t2), std::uint64_t{0});
        t.check_equal(s.cpu.cycles(), std::uint64_t{3});
        t.check_equal(s.cpu.mfc0(cp0::random), random);
    }};

const test::Registration count_and_random_positions{
    "mips.count_and_random_at_each_cycle", [](test::Context& t) {
        // MFC0 sees Count and Random as of its own cycle: Count advances every other cycle
        // (UM 14.8), Random once per graduated instruction (UM 14.2).
        System s;
        s.load(System::code,
               {mtc0(zero, cp0::count), nop(), nop(), mfc0(t0, cp0::count), mfc0(t1, cp0::count),
                nop(), mfc0(t2, cp0::count), mfc0(t3, cp0::random)});
        s.start_kernel();
        s.interpreter.run(8);
        t.check_equal(s.gpr(t0), std::uint64_t{1}); // cycle 3
        t.check_equal(s.gpr(t1), std::uint64_t{2}); // cycle 4
        t.check_equal(s.gpr(t2), std::uint64_t{3}); // cycle 6
        t.check_equal(s.gpr(t3), std::uint64_t{63 - 7});
    }};

} // namespace
