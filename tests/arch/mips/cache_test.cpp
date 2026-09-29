#include "arch/mips/system.hpp"
#include "support/test.hpp"

#include <cstdint>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips;
using namespace ultraviolent::mips::assembler;
using testing::kseg0;
using testing::System;

// CACHE operation fields (UM Table 10-1): operation << 2 | cache.
constexpr unsigned index_invalidate_i = 0x00;
constexpr unsigned index_load_tag_i = 0x04;
constexpr unsigned index_store_tag_i = 0x08;
constexpr unsigned hit_invalidate_i = 0x10;
constexpr unsigned index_writeback_invalidate_d = 0x01;
constexpr unsigned index_load_tag_d = 0x05;
constexpr unsigned index_store_tag_d = 0x09;
constexpr unsigned index_load_data_d = 0x19;
constexpr unsigned index_store_data_d = 0x1d;
constexpr unsigned index_load_tag_s = 0x07;
constexpr unsigned index_store_tag_s = 0x0b;
constexpr unsigned hit_invalidate_s = 0x13;
constexpr unsigned index_load_data_s = 0x1b;
constexpr unsigned index_store_data_s = 0x1f;

// Kernel mode, so CP0 is usable.
void kernel(System& s) {
    s.cpu.mtc0(cp0::status, status::kx);
}

void do_cache(test::Context& t, System& s, unsigned op, std::uint64_t address) {
    t.check(s.cpu.cache(op, address).has_value(), "CACHE completes");
}

void store_tag(test::Context& t, System& s, unsigned op, std::uint64_t address, std::uint32_t lo,
               std::uint32_t hi = 0) {
    s.cpu.mtc0(cp0::tag_lo, lo);
    s.cpu.mtc0(cp0::tag_hi, hi);
    do_cache(t, s, op, address);
}

std::uint64_t load_tag_lo(test::Context& t, System& s, unsigned op, std::uint64_t address) {
    s.cpu.mtc0(cp0::tag_lo, 0);
    do_cache(t, s, op, address);
    return s.cpu.mfc0(cp0::tag_lo) & 0xffff'ffff;
}

const test::Registration secondary_tags{
    "mips.cache_secondary_tags", [](test::Context& t) {
        System s;
        kernel(s);
        // The PROM's scache tag test pattern: each way keeps its own tag, masked to the
        // stored fields (TagLo 31:14, 11:10, 8:7, 6:0; UM 10.7, 10.10).
        store_tag(t, s, index_store_tag_s, kseg0(0x80), 0x5555'5555, 0x5);
        store_tag(t, s, index_store_tag_s, kseg0(0x81), 0xaaaa'aaaa, 0xa); // way 1: PA bit 0
        t.check_equal(load_tag_lo(t, s, index_load_tag_s, kseg0(0x80)), std::uint64_t{0x5555'4555});
        t.check_equal(s.cpu.mfc0(cp0::tag_hi) & 0xffff'ffff, std::uint64_t{0x5});
        t.check_equal(load_tag_lo(t, s, index_load_tag_s, kseg0(0x81)), std::uint64_t{0xaaaa'88aa});
        // Another set is separate.
        t.check_equal(load_tag_lo(t, s, index_load_tag_s, kseg0(0x100)), std::uint64_t{0});
        // MRU (TagHi 31) belongs to the set, not the way.
        store_tag(t, s, index_store_tag_s, kseg0(0x80), 0, 0x8000'0000);
        do_cache(t, s, index_load_tag_s, kseg0(0x81));
        t.check_equal(s.cpu.mfc0(cp0::tag_hi) & 0xffff'ffff, std::uint64_t{0x8000'000a});
        // Each set has its own: the last of the 4096 sets of 64 bytes is separate.
        do_cache(t, s, index_load_tag_s, kseg0(0x20000 + 0x80));
        t.check_equal(s.cpu.mfc0(cp0::tag_hi) >> 31 & 1, std::uint64_t{0});
    }};

const test::Registration primary_tags{
    "mips.cache_primary_tags", [](test::Context& t) {
        System s;
        kernel(s);
        // I: TagLo 31:8, 6, 2, 0 stored; LRU (3) per set; TagHi 3:0 (UM 10.5, 10.8).
        store_tag(t, s, index_store_tag_i, kseg0(0x40), 0xffff'ffff, 0xffff'ffff);
        t.check_equal(load_tag_lo(t, s, index_load_tag_i, kseg0(0x40)), std::uint64_t{0xffff'ff4d});
        t.check_equal(s.cpu.mfc0(cp0::tag_hi) & 0xffff'ffff, std::uint64_t{0xf});
        // D: TagLo 31:8, 7:6, 3:0; TagHi 31:29 StateMod and 3:0 (UM 10.6, 10.9).
        store_tag(t, s, index_store_tag_d, kseg0(0x21), 0xffff'ffff, 0xffff'ffff);
        t.check_equal(load_tag_lo(t, s, index_load_tag_d, kseg0(0x21)), std::uint64_t{0xffff'ffcf});
        t.check_equal(s.cpu.mfc0(cp0::tag_hi) & 0xffff'ffff, std::uint64_t{0xe000'000f});
        // Index WriteBack Invalidate (D): State 00, SCWay 0, StateMod 001, state parity 0.
        do_cache(t, s, index_writeback_invalidate_d, kseg0(0x21));
        t.check_equal(load_tag_lo(t, s, index_load_tag_d, kseg0(0x21)), std::uint64_t{0xffff'ff09});
        t.check_equal(s.cpu.mfc0(cp0::tag_hi) & 0xffff'ffff, std::uint64_t{0x2000'000f});
        // Index Invalidate (I) clears the state bit and its parity.
        do_cache(t, s, index_invalidate_i, kseg0(0x40));
        t.check_equal(load_tag_lo(t, s, index_load_tag_i, kseg0(0x40)), std::uint64_t{0xffff'ff09});
    }};

const test::Registration hit_operations{
    "mips.cache_hit_invalidate", [](test::Context& t) {
        System s;
        kernel(s);
        // A valid I-cache entry for physical page 0x5 at set 0x40 >> 6, way 0.
        store_tag(t, s, index_store_tag_i, kseg0(0x5040), (0x5u << 8) | (1u << 6));
        do_cache(t, s, hit_invalidate_i, kseg0(0x6040)); // another page: no match
        t.check_equal(load_tag_lo(t, s, index_load_tag_i, kseg0(0x5040)) & (1u << 6),
                      std::uint64_t{1u << 6});
        do_cache(t, s, hit_invalidate_i, kseg0(0x5040));
        t.check_equal(load_tag_lo(t, s, index_load_tag_i, kseg0(0x5040)) & (1u << 6),
                      std::uint64_t{0});

        // Secondary (512 KB, 64-byte blocks): a valid entry for physical 0x40000 (tag PA
        // 39:18 = 1) in way 1, with a subset D-cache block.
        store_tag(t, s, index_store_tag_s, kseg0(0x40001), (1u << 14) | (3u << 10));
        store_tag(t, s, index_store_tag_d, kseg0(0x40000), (0x40u << 8) | (3u << 6));
        do_cache(t, s, hit_invalidate_s, kseg0(0x40000));
        t.check((s.cpu.mfc0(cp0::status) & status::ch) != 0, "CH set on a hit");
        const std::uint64_t stag = load_tag_lo(t, s, index_load_tag_s, kseg0(0x40001));
        t.check_equal(stag & (3u << 10), std::uint64_t{0}); // invalid
        t.check_equal(stag >> 14, std::uint64_t{1});        // tag written from the PA
        // The MRU bit points away from the invalidated way.
        t.check_equal(s.cpu.mfc0(cp0::tag_hi) >> 31 & 1, std::uint64_t{0});
        t.check_equal(load_tag_lo(t, s, index_load_tag_d, kseg0(0x40000)) & (3u << 6),
                      std::uint64_t{0}); // subset invalidated
        do_cache(t, s, hit_invalidate_s, kseg0(0x40000));
        t.check((s.cpu.mfc0(cp0::status) & status::ch) == 0, "CH cleared on a miss");
    }};

const test::Registration data_arrays{
    "mips.cache_data_arrays", [](test::Context& t) {
        System s;
        kernel(s);
        // Secondary: a doubleword from TagHi:TagLo, the rest of its quadword zeroed, and the
        // quadword's ten check bits from ECC (UM 10.19, 10.22).
        s.cpu.mtc0(cp0::ecc, 0xffff);
        t.check_equal(s.cpu.mfc0(cp0::ecc), std::uint64_t{0x3ff}); // ten bits (UM 14.21)
        store_tag(t, s, index_store_data_s, kseg0(0x200), 0x1111'1111, 0x2222'2222);
        store_tag(t, s, index_store_data_s, kseg0(0x208), 0x3333'3333, 0x4444'4444);
        do_cache(t, s, index_load_data_s, kseg0(0x200));
        t.check_equal(s.cpu.mfc0(cp0::tag_lo) & 0xffff'ffff, std::uint64_t{0}); // padded
        do_cache(t, s, index_load_data_s, kseg0(0x208));
        t.check_equal(s.cpu.mfc0(cp0::tag_hi) & 0xffff'ffff, std::uint64_t{0x4444'4444});
        t.check_equal(s.cpu.mfc0(cp0::tag_lo) & 0xffff'ffff, std::uint64_t{0x3333'3333});
        t.check_equal(s.cpu.mfc0(cp0::ecc), std::uint64_t{0x3ff});
        do_cache(t, s, index_load_data_s, kseg0(0x209)); // way 1 is separate
        t.check_equal(s.cpu.mfc0(cp0::tag_lo) & 0xffff'ffff, std::uint64_t{0});
        // Primary data: a word and four parity bits.
        s.cpu.mtc0(cp0::ecc, 0x5);
        store_tag(t, s, index_store_data_d, kseg0(0x44), 0xdead'beef);
        s.cpu.mtc0(cp0::ecc, 0);
        t.check_equal(load_tag_lo(t, s, index_load_data_d, kseg0(0x44)),
                      std::uint64_t{0xdead'beef});
        t.check_equal(s.cpu.mfc0(cp0::ecc), std::uint64_t{0x5});
    }};

const test::Registration instruction{
    "mips.cache_instruction", [](test::Context& t) {
        System s;
        // CACHE through the interpreter: Index Store Tag (S) then Index Load Tag (S).
        s.set(a0, kseg0(0x80));
        s.set(t0, 0x5555'5555);
        s.run_program({mtc0(t0, cp0::tag_lo), cache(index_store_tag_s, 0, a0), mtc0(0, cp0::tag_lo),
                       cache(index_load_tag_s, 0, a0), mfc0(t1, cp0::tag_lo)});
        t.check_equal(s.gpr(t1), std::uint64_t{0x5555'4555});
        // A mapped address without a TLB entry takes a TLB refill like a load.
        s.set(a0, 0xffff'ffff'c000'0000);
        s.run_program({cache(index_load_tag_s, 0, a0)});
        t.check_equal(s.exception_code(), static_cast<std::uint32_t>(ExceptionCode::tlb_load));
    }};

} // namespace
