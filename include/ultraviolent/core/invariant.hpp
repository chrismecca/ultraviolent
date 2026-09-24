#pragma once

#include <source_location>
#include <string_view>

namespace ultraviolent {

// Reports a violated internal invariant and terminates the process. An invariant failure is
// an emulator bug, never guest-visible behavior, so it is not recoverable.
[[noreturn]] void invariant_failed(std::string_view message,
                                   std::source_location location = std::source_location::current());

// Checks an internal invariant. Enabled in every build type: the reference machine prefers
// stopping conspicuously to continuing with corrupt state (ADR-017).
constexpr void invariant(bool condition, std::string_view message,
                         std::source_location location = std::source_location::current()) {
    if (!condition) [[unlikely]] {
        invariant_failed(message, location);
    }
}

} // namespace ultraviolent
