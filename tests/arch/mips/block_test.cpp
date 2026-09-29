// Stage D of IR.adoc: block formation rules.

#include "arch/mips/assembler.hpp"
#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <ultraviolent/arch/mips/block.hpp>
#include <ultraviolent/arch/mips/cp0.hpp>

#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <vector>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using namespace ultraviolent::mips::testing;

constexpr std::uint64_t page = 0x1000; // System::code's page
constexpr std::uint64_t base = kseg0(page);

struct Built {
    std::optional<NoBlock> none;
    Block block;
};

// Loads `words` at page offset `offset` and builds the block that starts there.
Built build(std::initializer_list<std::uint32_t> words, std::uint64_t offset = 0) {
    System s;
    s.load(page + offset, words);
    s.start_kernel(base + offset);
    Built built;
    built.none = build_block(s.cpu, base + offset, built.block);
    return built;
}

const test::Registration control_flow{
    "mips.block.ends_after_branch_and_delay_slot", [](test::Context& t) {
        for (const std::uint32_t branch : {beq(t0, t1, 16), beql(t0, t1, 16), j(0x1000), jr(ra),
                                           jalr(ra, t0), bltzal(t0, 8), bgezall(t0, 8)}) {
            const auto b = build(
                {addu(v0, t0, t1), ori(v1, t2, 5), branch, addiu(a0, a0, 1), addiu(a1, a1, 1)});
            t.check(!b.none);
            t.check_equal(b.block.operations.size(), std::size_t{4});
            t.check(b.block.end == BlockEnd::control_flow);
            t.check_equal(b.block.operations[3].instruction.word, addiu(a0, a0, 1));
        }
    }};

const test::Registration state_changes{
    "mips.block.ends_after_state_changes", [](test::Context& t) {
        for (const std::uint32_t word :
             {mtc0(t0, cp0::status), dmtc0(t0, cp0::entry_hi), eret(), tlbr(), tlbwi(), tlbwr(),
              tlbp(), cache(0x19, 0, t0), sync()}) {
            const auto b = build({nop(), word, nop(), nop()});
            t.check(!b.none);
            t.check_equal(b.block.operations.size(), std::size_t{2});
            t.check(b.block.end == BlockEnd::state_change);
        }
        // MFC0 and SYSCALL continue the block (exceptions end execution when taken).
        const auto b = build({mfc0(t0, cp0::count), syscall(), nop(), sync()});
        t.check_equal(b.block.operations.size(), std::size_t{4});
    }};

const test::Registration unsupported{
    "mips.block.ends_before_unsupported", [](test::Context& t) {
        for (const std::uint32_t word : {0xec00'0000u, 0x4800'0000u, 0x4200'0003u}) {
            const auto b = build({nop(), nop(), word, nop()});
            t.check(!b.none);
            t.check_equal(b.block.operations.size(), std::size_t{2});
            t.check(b.block.end == BlockEnd::unsupported);
            const auto first = build({word, nop()});
            t.check(first.none == NoBlock::unsupported);
        }
    }};

const test::Registration delay_slots{
    "mips.block.keeps_branch_and_delay_slot_together", [](test::Context& t) {
        // A delay slot on the next page: the block ends before the branch.
        const auto across = build({nop(), beq(zero, zero, 8)}, 0xff8);
        t.check_equal(across.block.operations.size(), std::size_t{1});
        t.check(across.block.end == BlockEnd::delay_slot);
        t.check(build({beq(zero, zero, 8)}, 0xffc).none == NoBlock::delay_slot);
        // A branch or an unsupported operation in the delay slot: the reference steps it.
        for (const std::uint32_t slot : {bne(t0, t1, 8), jr(ra), 0x4800'0000u}) {
            const auto b = build({nop(), beq(t0, t0, 8), slot, nop()});
            t.check_equal(b.block.operations.size(), std::size_t{1});
            t.check(b.block.end == BlockEnd::delay_slot);
        }
        // A state change in a delay slot stays with its branch.
        const auto b = build({beq(t0, t0, 8), mtc0(t0, cp0::status), nop()});
        t.check_equal(b.block.operations.size(), std::size_t{2});
        t.check(b.block.end == BlockEnd::control_flow);
    }};

const test::Registration page_and_length{
    "mips.block.page_boundary_and_length_limit", [](test::Context& t) {
        const auto tail = build({nop(), nop(), nop(), nop()}, 0xff0);
        t.check_equal(tail.block.operations.size(), std::size_t{4});
        t.check(tail.block.end == BlockEnd::page_boundary);

        System s;
        for (std::uint64_t i = 0; i < 100; ++i) {
            s.load(page + 4 * i, {addiu(v0, v0, 1)});
        }
        s.start_kernel(base);
        Block block;
        t.check(!build_block(s.cpu, base, block));
        t.check_equal(block.operations.size(), max_block_length);
        t.check(block.end == BlockEnd::length_limit);
        // A branch whose delay slot would pass the limit is not split from it.
        s.load(page + 4 * (max_block_length - 1), {beq(zero, zero, 8)});
        t.check(!build_block(s.cpu, base, block));
        t.check_equal(block.operations.size(), max_block_length - 1);
        t.check(block.end == BlockEnd::length_limit);
        // A block may start anywhere in the page; its operations are the words there.
        t.check(!build_block(s.cpu, base + 40, block));
        t.check_equal(block.start_pc, base + 40);
        t.check_equal(block.operations[0].instruction.word, addiu(v0, v0, 1));
        t.check_equal(block.word_now(0), addiu(v0, v0, 1));
    }};

// A device window that counts accesses.
struct Device final : MmioTarget {
    int accesses{};
    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t /*offset*/,
                                                        AccessWidth /*width*/) override {
        ++accesses;
        return 0;
    }
    std::expected<void, AccessFault> mmio_write(std::uint64_t /*offset*/, AccessWidth /*width*/,
                                                std::uint64_t /*value*/) override {
        ++accesses;
        return {};
    }
};

const test::Registration not_host_backed{
    "mips.block.only_from_host_backed_code", [](test::Context& t) {
        System s;
        Device device;
        constexpr std::uint64_t device_base = 0x0800'0000;
        t.check(s.bus.map_mmio({PhysicalAddress{device_base}, 0x1000}, device).has_value());
        s.start_kernel(base);
        Block block;
        // Code in a device window: no block, and building one did not access the device.
        t.check(build_block(s.cpu, kseg0(device_base), block) == NoBlock::not_host_backed);
        t.check_equal(device.accesses, 0);
        // A misaligned pc, and an unmapped user address (a fetch there would fault).
        t.check(build_block(s.cpu, base + 2, block) == NoBlock::not_host_backed);
        t.check(build_block(s.cpu, 0x4000'0000, block) == NoBlock::not_host_backed);
        // Nothing architectural changed.
        t.check_equal(s.cpu.cycles(), std::uint64_t{0});
        t.check_equal(s.pc(), base);
    }};

} // namespace
