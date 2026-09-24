#pragma once

#include <cstdint>

// R10000 Coprocessor 0 definitions. Section references are to the MIPS R10000
// Microprocessor User's Manual, Version 2.0 (UM), chapter 14 unless noted.
namespace ultraviolent::mips {

// CP0 register numbers (UM Table 14-1).
namespace cp0 {
inline constexpr unsigned index = 0;
inline constexpr unsigned random = 1;
inline constexpr unsigned entry_lo0 = 2;
inline constexpr unsigned entry_lo1 = 3;
inline constexpr unsigned context = 4;
inline constexpr unsigned page_mask = 5;
inline constexpr unsigned wired = 6;
inline constexpr unsigned bad_vaddr = 8;
inline constexpr unsigned count = 9;
inline constexpr unsigned entry_hi = 10;
inline constexpr unsigned compare = 11;
inline constexpr unsigned status = 12;
inline constexpr unsigned cause = 13;
inline constexpr unsigned epc = 14;
inline constexpr unsigned prid = 15;
inline constexpr unsigned config = 16;
inline constexpr unsigned ll_addr = 17;
inline constexpr unsigned watch_lo = 18;
inline constexpr unsigned watch_hi = 19;
inline constexpr unsigned xcontext = 20;
inline constexpr unsigned frame_mask = 21;
inline constexpr unsigned diagnostic = 22;
inline constexpr unsigned performance_counter = 25;
inline constexpr unsigned ecc = 26;
inline constexpr unsigned cache_error = 27;
inline constexpr unsigned tag_lo = 28;
inline constexpr unsigned tag_hi = 29;
inline constexpr unsigned error_epc = 30;
} // namespace cp0

// Status register fields (UM 14.10, Figures 14-11 and 14-12).
namespace status {
inline constexpr std::uint32_t ie = 1u << 0;
inline constexpr std::uint32_t exl = 1u << 1;
inline constexpr std::uint32_t erl = 1u << 2;
inline constexpr unsigned ksu_shift = 3;
inline constexpr std::uint32_t ksu_mask = 3u << ksu_shift;
inline constexpr std::uint32_t ux = 1u << 5;
inline constexpr std::uint32_t sx = 1u << 6;
inline constexpr std::uint32_t kx = 1u << 7;
inline constexpr unsigned im_shift = 8;
inline constexpr std::uint32_t de = 1u << 16;
inline constexpr std::uint32_t ce = 1u << 17;
inline constexpr std::uint32_t ch = 1u << 18;
inline constexpr std::uint32_t nmi = 1u << 19;
inline constexpr std::uint32_t sr = 1u << 20;
inline constexpr std::uint32_t ts = 1u << 21;
inline constexpr std::uint32_t bev = 1u << 22;
inline constexpr std::uint32_t re = 1u << 25;
inline constexpr std::uint32_t fr = 1u << 26;
inline constexpr std::uint32_t rp = 1u << 27;
inline constexpr std::uint32_t cu0 = 1u << 28;
inline constexpr std::uint32_t cu1 = 1u << 29;
inline constexpr std::uint32_t cu2 = 1u << 30;
inline constexpr std::uint32_t xx = 1u << 31;
// Bits 24:23 are reserved and read as zero.
inline constexpr std::uint32_t writable = ~(3u << 23);
} // namespace status

// Cause register fields (UM 14.11, Figure 14-13).
namespace cause {
inline constexpr std::uint32_t bd = 1u << 31;
inline constexpr unsigned ce_shift = 28;
inline constexpr std::uint32_t ce_mask = 3u << ce_shift;
inline constexpr unsigned ip_shift = 8;
inline constexpr unsigned exc_code_shift = 2;
inline constexpr std::uint32_t exc_code_mask = 0x1fu << exc_code_shift;
// Only the software interrupt bits IP[1:0] are writable.
inline constexpr std::uint32_t software_interrupts = 3u << ip_shift;
} // namespace cause

// Cause.ExcCode values (UM Table 14-13).
enum class ExceptionCode : std::uint8_t {
    interrupt = 0,
    tlb_modified = 1,
    tlb_load = 2,
    tlb_store = 3,
    address_error_load = 4,
    address_error_store = 5,
    bus_error_instruction = 6,
    bus_error_data = 7,
    syscall = 8,
    breakpoint = 9,
    reserved_instruction = 10,
    coprocessor_unusable = 11,
    overflow = 12,
    trap = 13,
    floating_point = 15,
    watch = 23,
};

// EntryLo fields (UM 14.3, Figure 14-3).
namespace entry_lo {
inline constexpr std::uint64_t g = 1u << 0;
inline constexpr std::uint64_t v = 1u << 1;
inline constexpr std::uint64_t d = 1u << 2;
inline constexpr unsigned c_shift = 3;
inline constexpr unsigned pfn_shift = 6;
inline constexpr std::uint64_t pfn_mask = ((std::uint64_t{1} << 28) - 1) << pfn_shift;
inline constexpr unsigned uc_shift = 62;
// UC, PFN, C, D, V, G. Bits 61:34 are reserved and read as zero.
inline constexpr std::uint64_t writable = (std::uint64_t{3} << uc_shift) | pfn_mask | 0x3f;
} // namespace entry_lo

// EntryHi fields (UM 14.9, Figure 14-10): R 63:62, VPN2 43:13, ASID 7:0.
namespace entry_hi {
inline constexpr unsigned vpn2_shift = 13;
inline constexpr std::uint64_t vpn2_mask = ((std::uint64_t{1} << 31) - 1) << vpn2_shift;
inline constexpr unsigned region_shift = 62;
inline constexpr std::uint64_t asid_mask = 0xff;
inline constexpr std::uint64_t writable =
    (std::uint64_t{3} << region_shift) | vpn2_mask | asid_mask;
} // namespace entry_hi

// PageMask MASK field, bits 24:13 (UM 14.5).
inline constexpr std::uint64_t page_mask_writable = std::uint64_t{0xfff} << 13;

// Config.BE: memory is big-endian (UM Table 14-15).
inline constexpr std::uint32_t config_big_endian = 1u << 15;

// Cache coherency attributes that bypass the caches (UM 14.14, Table 14-15).
constexpr bool is_uncached(unsigned coherency) {
    return coherency == 2 || coherency == 7;
}

} // namespace ultraviolent::mips
