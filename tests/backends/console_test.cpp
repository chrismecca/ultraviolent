#include "support/test.hpp"

#include <ultraviolent/backends/console.hpp>

#include <cstdio>
#include <string>
#include <string_view>

namespace {

using namespace ultraviolent;

struct Bench {
    explicit Bench(std::vector<std::string> lines, bool at_prompt = false)
        : output{std::tmpfile()}, console{output, std::move(lines), at_prompt} {}
    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;
    ~Bench() {
        std::fclose(output);
    }
    void print(std::string_view text) {
        for (const char c : text) {
            console.serial_transmit(static_cast<std::uint8_t>(c));
        }
    }
    // Polls `count` times; returns the first byte received, or 0.
    char poll(unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            if (const auto byte = console.serial_receive()) {
                return static_cast<char>(*byte);
            }
        }
        return 0;
    }
    std::FILE* output;
    backends::ScriptedConsole console;
};

const test::Registration prompt{"console.lines_wait_for_a_prompt", [](test::Context& t) {
                                    Bench b{{"help"}};
                                    b.print("Option: 1\n");
                                    t.check_equal(b.poll(1000), '\0'); // not a prompt ending
                                    b.print("POD> ");
                                    // A program draining typeahead polls a few times first.
                                    t.check_equal(b.poll(10), '\0');
                                    t.check_equal(b.poll(1000), 'h');
                                    t.check_equal(b.poll(1), 'e');
                                }};

const test::Registration shell{"console.shell_prompts", [](test::Context& t) {
                                   Bench b{{"a", "b", "c"}};
                                   b.print("IRIS 1# ");
                                   t.check_equal(b.poll(1000), 'a');
                                   t.check_equal(b.poll(1), '\r');
                                   b.print("\n$ ");
                                   t.check_equal(b.poll(1000), 'b');
                                   t.check_equal(b.poll(1), '\r');
                                   b.print("\nIRIS% ");
                                   t.check_equal(b.poll(1000), 'c');
                               }};

const test::Registration idle{"console.any_output_then_long_wait", [](test::Context& t) {
                                  Bench b{{"x"}};
                                  b.print("Type control-C to interrupt.\n");
                                  t.check_equal(b.poll(10'000), '\0');
                                  t.check_equal(b.poll(20'000), 'x');
                                  t.check_equal(b.poll(1), '\r');
                                  t.check_equal(b.poll(100'000), '\0'); // script exhausted
                              }};

const test::Registration done{"console.script_done_at_next_prompt", [](test::Context& t) {
                                  Bench b{{"a"}};
                                  int calls = 0;
                                  b.console.on_script_done([&] { ++calls; });
                                  b.print("> ");
                                  t.check_equal(b.poll(1000), 'a');
                                  t.check_equal(b.poll(1), '\r');
                                  t.check_equal(calls, 0);
                                  b.print("next> ");
                                  b.poll(1000);
                                  t.check_equal(calls, 1);
                                  b.poll(1000);
                                  t.check_equal(calls, 1); // once
                              }};

const test::Registration expect{"console.expect_and_stop_directives", [](test::Context& t) {
                                    Bench b{{"@expect CD.|Inst> ", "@stop", "go"}};
                                    int stops = 0;
                                    b.console.on_stop([&] { ++stops; });
                                    b.print("Installing ...\n");
                                    b.poll(50'000);
                                    t.check_equal(stops, 0);
                                    b.print("Please insert the CD.\n");
                                    b.poll(1);
                                    t.check_equal(stops, 1);
                                    // The next line waits for a prompt as usual.
                                    b.print("Inst> ");
                                    t.check_equal(b.poll(1000), 'g');
                                }};

const test::Registration resumed{"console.snapshot_resume_types_at_once", [](test::Context& t) {
                                     Bench b{{"y"}, true};
                                     t.check_equal(b.poll(1), 'y');
                                 }};

} // namespace
