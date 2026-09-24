#include "support/test.hpp"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace ultraviolent::test {

namespace {

struct Case {
    std::string name;
    TestFunction function;
};

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

bool selected(std::string_view name, std::span<const std::string_view> filters) {
    if (filters.empty()) {
        return true;
    }
    return std::ranges::any_of(filters,
                               [&](std::string_view filter) { return name.contains(filter); });
}

int run(std::span<const std::string_view> arguments) {
    std::vector<Case>& cases = registry();
    std::ranges::sort(cases, {}, &Case::name);
    if (std::ranges::adjacent_find(cases, {}, &Case::name) != cases.end()) {
        std::fputs("duplicate test case name\n", stderr);
        return 1;
    }

    std::vector<std::string_view> filters;
    bool list_only = false;
    for (std::string_view argument : arguments) {
        if (argument == "--list") {
            list_only = true;
        } else {
            filters.push_back(argument);
        }
    }

    int ran = 0;
    int failed = 0;
    for (const Case& test_case : cases) {
        if (!selected(test_case.name, filters)) {
            continue;
        }
        if (list_only) {
            std::printf("%s\n", test_case.name.c_str());
            continue;
        }
        // If the case crashes, the last line printed names the case that was running.
        std::printf("run  %s\n", test_case.name.c_str());
        std::fflush(stdout);
        Context context;
        test_case.function(context);
        ++ran;
        if (context.failures() != 0) {
            ++failed;
            std::printf("FAIL %s\n", test_case.name.c_str());
        } else {
            std::printf("pass %s\n", test_case.name.c_str());
        }
    }

    if (list_only) {
        return 0;
    }
    std::printf("%d passed, %d failed\n", ran - failed, failed);
    if (ran == 0) {
        std::fputs("no test cases selected\n", stderr);
        return 1;
    }
    return failed == 0 ? 0 : 1;
}

} // namespace

bool Context::check(bool condition, std::string_view description, std::source_location location) {
    if (!condition) {
        fail(location, description.empty() ? "check failed" : description);
    }
    return condition;
}

void Context::fail(std::source_location location, std::string_view message) {
    ++failures_;
    std::printf("  %s:%u: %.*s\n", location.file_name(), static_cast<unsigned>(location.line()),
                static_cast<int>(message.size()), message.data());
}

Registration::Registration(std::string_view name, TestFunction function) {
    registry().push_back({std::string{name}, function});
}

} // namespace ultraviolent::test

int main(int argc, char** argv) {
    std::vector<std::string_view> arguments(argv + 1, argv + argc);
    return ultraviolent::test::run(arguments);
}
