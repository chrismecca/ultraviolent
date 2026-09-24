#include <ultraviolent/arch/mips/tlb.hpp>

#include <ultraviolent/arch/mips/cp0.hpp>
#include <ultraviolent/core/invariant.hpp>

#include <bit>

namespace ultraviolent::mips {

namespace {

// PageMask MASK field as a mask over VPN2 bits.
std::uint64_t vpn2_mask_bits(std::uint64_t page_mask) {
    return (page_mask >> 13) & 0xfff;
}

std::uint64_t region(std::uint64_t entry_hi_value) {
    return entry_hi_value >> entry_hi::region_shift;
}

std::uint64_t vpn2(std::uint64_t entry_hi_value) {
    return (entry_hi_value & entry_hi::vpn2_mask) >> entry_hi::vpn2_shift;
}

std::uint64_t asid(std::uint64_t entry_hi_value) {
    return entry_hi_value & entry_hi::asid_mask;
}

bool matches(const TlbEntry& entry, std::uint64_t address_region, std::uint64_t address_vpn2,
             std::uint64_t address_asid) {
    if (!entry.enabled || region(entry.entry_hi) != address_region) {
        return false;
    }
    const std::uint64_t ignored = vpn2_mask_bits(entry.page_mask);
    if ((vpn2(entry.entry_hi) & ~ignored) != (address_vpn2 & ~ignored)) {
        return false;
    }
    return entry.global || asid(entry.entry_hi) == address_asid;
}

// True when some address could be matched by both entries.
bool conflicts(const TlbEntry& a, const TlbEntry& b) {
    if (!a.enabled || !b.enabled || region(a.entry_hi) != region(b.entry_hi)) {
        return false;
    }
    const std::uint64_t ignored = vpn2_mask_bits(a.page_mask) | vpn2_mask_bits(b.page_mask);
    if ((vpn2(a.entry_hi) & ~ignored) != (vpn2(b.entry_hi) & ~ignored)) {
        return false;
    }
    return a.global || b.global || asid(a.entry_hi) == asid(b.entry_hi);
}

} // namespace

void Tlb::clear() {
    entries_ = {};
    last_match_ = 0;
}

const TlbEntry& Tlb::entry(std::size_t index) const {
    invariant(index < entry_count, "TLB index out of range");
    return entries_[index];
}

bool Tlb::write(std::size_t index, TlbEntry entry) {
    invariant(index < entry_count, "TLB index out of range");
    entry.enabled = true;
    bool conflict = false;
    for (std::size_t i = 0; i < entry_count; ++i) {
        if (i != index && conflicts(entries_[i], entry)) {
            // hypothesis: an invalidated entry stops matching entirely, so a later reference
            // takes a refill rather than an invalid exception. UM 14.10 says only that the
            // entries are "invalidated".
            entries_[i].enabled = false;
            conflict = true;
        }
    }
    entries_[index] = entry;
    return conflict;
}

std::optional<std::size_t> Tlb::probe(std::uint64_t entry_hi_value) const {
    for (std::size_t i = 0; i < entry_count; ++i) {
        if (matches(entries_[i], region(entry_hi_value), vpn2(entry_hi_value),
                    asid(entry_hi_value))) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<TlbLookup> Tlb::lookup(std::uint64_t address, std::uint8_t address_asid) const {
    const std::uint64_t address_region = address >> 62;
    const std::uint64_t address_vpn2 = (address >> 13) & ((std::uint64_t{1} << 31) - 1);
    if (matches(entries_[last_match_], address_region, address_vpn2, address_asid)) {
        return TlbLookup{last_match_, (address & page_size(entries_[last_match_])) != 0};
    }
    for (std::size_t i = 0; i < entry_count; ++i) {
        if (matches(entries_[i], address_region, address_vpn2, address_asid)) {
            last_match_ = i;
            return TlbLookup{i, (address & page_size(entries_[i])) != 0};
        }
    }
    return std::nullopt;
}

std::uint64_t Tlb::page_size(const TlbEntry& entry) {
    // Valid masks are pairs of ones (UM Table 14-6); each set bit doubles the 4 KiB page.
    return std::uint64_t{4096} << std::popcount(vpn2_mask_bits(entry.page_mask));
}

} // namespace ultraviolent::mips
