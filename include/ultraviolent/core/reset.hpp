#pragma once

#include <cstdint>

namespace ultraviolent {

// Machine-wide reset kinds (ARCHITECTURE "Reset semantics"). Each component documents, with
// a source, what it preserves under each kind; the rules below are the only defaults.
// Device-local resets are operations of the device that supports them, not a ResetKind.
enum class ResetKind : std::uint8_t {
    // Power applied. Components start from their construction state; persistent state (NVRAM,
    // flash, disks) is reloaded from its backing store, never reinitialized.
    power_on,
    // Hardware reset asserted with power held. Volatile state follows each component's
    // documented reset values; persistent state is untouched.
    cold,
    // System or soft reset. Anything the hardware preserves across such a reset is kept;
    // persistent state is untouched.
    warm,
};

} // namespace ultraviolent
