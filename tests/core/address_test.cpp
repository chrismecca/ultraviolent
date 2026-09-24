#include "support/test.hpp"

#include <ultraviolent/core/address.hpp>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace {

using namespace ultraviolent;

// Domains are distinct types: a virtual address cannot be passed where a physical one is
// expected without an explicit conversion.
static_assert(!std::is_convertible_v<VirtualAddress, PhysicalAddress>);
static_assert(!std::is_convertible_v<std::uint64_t, PhysicalAddress>);
static_assert(sizeof(PhysicalAddress) == sizeof(std::uint64_t));

const test::Registration address_arithmetic{
    "address.arithmetic", [](test::Context& t) {
        constexpr PhysicalAddress base{0x1000};
        t.check_equal((base + 0x20).value, std::uint64_t{0x1020});
        t.check_equal(PhysicalAddress{0x1020} - base, std::uint64_t{0x20});
        t.check(base < PhysicalAddress{0x1001});
    }};

const test::Registration range_contains{
    "address.range_contains", [](test::Context& t) {
        constexpr PhysicalRange range{PhysicalAddress{0x1000}, 0x100};
        t.check(range.contains(PhysicalAddress{0x1000}), "base is inside");
        t.check(range.contains(PhysicalAddress{0x10ff}), "last is inside");
        t.check(!range.contains(PhysicalAddress{0x1100}), "end is outside");
        t.check(!range.contains(PhysicalAddress{0xfff}), "below base is outside");
        t.check_equal(range.last().value, std::uint64_t{0x10ff});
    }};

const test::Registration range_validity{
    "address.range_validity", [](test::Context& t) {
        constexpr std::uint64_t top = std::numeric_limits<std::uint64_t>::max();
        t.check(!PhysicalRange{PhysicalAddress{0}, 0}.is_valid(), "empty range is invalid");
        t.check(PhysicalRange{PhysicalAddress{top}, 1}.is_valid(), "range may end at the top");
        t.check(!PhysicalRange{PhysicalAddress{top}, 2}.is_valid(), "range may not wrap");
        const PhysicalRange whole{PhysicalAddress{0}, top};
        t.check(whole.is_valid());
        t.check(whole.contains(PhysicalAddress{top - 1}));
    }};

const test::Registration range_overlap{
    "address.range_overlap", [](test::Context& t) {
        constexpr PhysicalRange a{PhysicalAddress{0x1000}, 0x100};
        t.check(a.overlaps(a));
        t.check(a.overlaps({PhysicalAddress{0x10ff}, 1}), "touching the last byte overlaps");
        t.check(!a.overlaps({PhysicalAddress{0x1100}, 0x100}), "adjacent ranges do not overlap");
        t.check(!a.overlaps({PhysicalAddress{0xf00}, 0x100}), "adjacent ranges do not overlap");
        t.check(a.overlaps({PhysicalAddress{0}, 0x2000}), "enclosing range overlaps");
    }};

} // namespace
