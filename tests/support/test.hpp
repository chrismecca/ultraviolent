#pragma once

// Minimal unit-test harness (ADR-016). Tests register at static-initialization time:
//
//     const test::Registration example{"area.behavior", [](test::Context& t) {
//         t.check_equal(1 + 1, 2);
//     }};
//
// The runner executes cases in name order. Arguments are substrings; only cases whose name
// contains one of them run. `--list` prints the case names.

#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

namespace ultraviolent::test {

// Collects the failures of one test case. A failed check does not stop the case; guard steps
// that depend on an earlier result with `if (!t.check(...)) return;`.
class Context {
  public:
    bool check(bool condition, std::string_view description = {},
               std::source_location location = std::source_location::current());

    template <class Actual, class Expected>
    bool check_equal(const Actual& actual, const Expected& expected,
                     std::source_location location = std::source_location::current()) {
        if (actual == expected) {
            return true;
        }
        fail(location, std::format("expected {}, got {}", describe(expected), describe(actual)));
        return false;
    }

    [[nodiscard]] int failures() const {
        return failures_;
    }

  private:
    template <class T> static std::string describe(const T& value) {
        if constexpr (std::is_default_constructible_v<std::formatter<T, char>>) {
            return std::format("{}", value);
        } else {
            return "<unprintable>";
        }
    }

    void fail(std::source_location location, std::string_view message);

    int failures_{};
};

using TestFunction = void (*)(Context&);

class Registration {
  public:
    Registration(std::string_view name, TestFunction function);
};

} // namespace ultraviolent::test
