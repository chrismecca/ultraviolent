#pragma once

#include <compare>
#include <cstdint>
#include <limits>

namespace ultraviolent {

// An address in one address domain. Each domain is a distinct type, so crossing domains
// (virtual to physical, physical to Xtalk, ...) takes an explicit conversion owned by
// whatever hardware performs the translation (ARCHITECTURE "Typed addresses").
//
// Arithmetic on addresses wraps modulo 2^64. Ranges validate their own bounds.
template <class Domain> struct Address {
    std::uint64_t value{};

    auto operator<=>(const Address&) const = default;

    constexpr Address operator+(std::uint64_t offset) const {
        return {value + offset};
    }
};

// Distance from `origin` to `address` in the same domain.
template <class Domain>
constexpr std::uint64_t operator-(Address<Domain> address, Address<Domain> origin) {
    return address.value - origin.value;
}

struct PhysicalDomain;
struct VirtualDomain;

// Address as seen on the CPU's physical (system bus) side, after any translation.
using PhysicalAddress = Address<PhysicalDomain>;
// CPU virtual address, before TLB or segment translation.
using VirtualAddress = Address<VirtualDomain>;

// A contiguous, non-empty run of addresses [base, base + size).
template <class AddressType> struct AddressRange {
    AddressType base{};
    std::uint64_t size{};

    bool operator==(const AddressRange&) const = default;

    // Non-empty and not wrapping past the top of the domain.
    [[nodiscard]] constexpr bool is_valid() const {
        return size != 0 && size - 1 <= std::numeric_limits<std::uint64_t>::max() - base.value;
    }

    // The last address in the range. Inclusive, so a range may end at the top of the domain.
    [[nodiscard]] constexpr AddressType last() const {
        return {base.value + (size - 1)};
    }

    [[nodiscard]] constexpr bool contains(AddressType address) const {
        return address >= base && address.value - base.value < size;
    }

    [[nodiscard]] constexpr bool overlaps(const AddressRange& other) const {
        return base <= other.last() && other.base <= last();
    }
};

using PhysicalRange = AddressRange<PhysicalAddress>;

} // namespace ultraviolent
