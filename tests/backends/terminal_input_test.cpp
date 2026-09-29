#include "support/test.hpp"

#include <ultraviolent/backends/console.hpp>
#include <ultraviolent/backends/terminal_input.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

using namespace ultraviolent;

// A pipe standing in for the user's terminal.
struct Pipe {
    Pipe() {
        if (::pipe(fds) != 0) {
            fds[0] = fds[1] = -1;
        }
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    ~Pipe() {
        ::close(fds[0]);
        if (fds[1] >= 0) {
            ::close(fds[1]);
        }
    }
    void type(std::string_view text) const {
        [[maybe_unused]] const auto written = ::write(fds[1], text.data(), text.size());
    }
    void close_writer() {
        ::close(fds[1]);
        fds[1] = -1;
    }
    int fds[2]{};
};

// Polls up to `count` times; returns the first byte received.
std::optional<std::uint8_t> poll(devices::SerialSource& source, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        if (const auto byte = source.serial_receive()) {
            return byte;
        }
    }
    return std::nullopt;
}

const test::Registration bytes{
    "terminal_input.bytes_and_escape", [](test::Context& t) {
        Pipe pipe;
        backends::TerminalInput input{pipe.fds[0], std::chrono::microseconds{0}};
        t.check(!input.terminal());
        std::vector<std::string> commands;
        input.on_command([&commands](std::string_view line) { commands.emplace_back(line); });
        t.check(!poll(input, 64).has_value());
        pipe.type("ls\r");
        t.check_equal(unsigned{poll(input, 64).value_or(0)}, unsigned{'l'});
        t.check_equal(unsigned{poll(input, 1).value_or(0)}, unsigned{'s'});
        t.check_equal(unsigned{poll(input, 1).value_or(0)}, unsigned{'\r'});
        // Ctrl-] starts a monitor command, read to Enter and not passed on.
        pipe.type("\x1d"
                  "cdrom disc.iso\r"
                  "x");
        t.check_equal(unsigned{poll(input, 64).value_or(0)}, unsigned{'x'});
        t.check(commands.size() == 1 && commands[0] == "cdrom disc.iso");
        // End of input: nothing more, and no error.
        pipe.close_writer();
        t.check(!poll(input, 64).has_value());
    }};

const test::Registration handoff{
    "terminal_input.after_the_script", [](test::Context& t) {
        Pipe pipe;
        backends::TerminalInput input{pipe.fds[0], std::chrono::microseconds{0}};
        std::FILE* output = std::tmpfile();
        {
            backends::ScriptedConsole console{output, {"a"}, true};
            console.then_read(input);
            pipe.type("z");
            // The script's line comes first, then the terminal.
            t.check_equal(unsigned{poll(console, 64).value_or(0)}, unsigned{'a'});
            t.check_equal(unsigned{poll(console, 1).value_or(0)}, unsigned{'\r'});
            t.check_equal(unsigned{poll(console, 64).value_or(0)}, unsigned{'z'});
            // A prompt without a newline is flushed before the terminal is read.
            for (const char c : std::string_view{"login: "}) {
                console.serial_transmit(static_cast<std::uint8_t>(c));
            }
            poll(console, 1);
            t.check(std::fseek(output, 0, SEEK_SET) == 0);
            char text[16]{};
            t.check(std::fgets(text, sizeof text, output) != nullptr &&
                    std::string_view{text} == "login: ");
        }
        std::fclose(output);
    }};

} // namespace
