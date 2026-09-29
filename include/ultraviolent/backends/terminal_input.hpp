#pragma once

#include <ultraviolent/devices/uart16550.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <termios.h>

namespace ultraviolent::backends {

// Console input from a host file descriptor, normally standard input: what the user types
// reaches the guest's serial port. A terminal is put in raw input mode (no line editing, no
// echo, no signal keys, so Ctrl-C reaches the guest) with output processing kept; the
// destructor restores it. Other descriptors (pipes, files) are read without blocking until
// end of file.
//
// The guest polls its UART far more often than anyone types, so the descriptor is read at
// most once per `check_interval` of host time. Input timing depends on the host, so runs
// with it are not deterministic; the scripted console is the reproducible path.
//
// Ctrl-] (0x1d) is not passed on: it starts a monitor command, a line read up to Enter (and
// echoed on standard error) that goes to the command handler instead of the guest: "q" to end
// the run, "cdrom FILE" to change the disc, and so on (the handler decides).
class TerminalInput final : public devices::SerialSource {
  public:
    static constexpr std::uint8_t escape_byte = 0x1d;

    explicit TerminalInput(int fd,
                           std::chrono::microseconds check_interval = std::chrono::microseconds{
                               1000});
    TerminalInput(const TerminalInput&) = delete;
    TerminalInput& operator=(const TerminalInput&) = delete;
    ~TerminalInput() override;

    void on_command(std::function<void(std::string_view line)> command) {
        command_ = std::move(command);
    }
    // Whether the descriptor is a terminal in raw mode.
    [[nodiscard]] bool terminal() const {
        return saved_.has_value();
    }

    std::optional<std::uint8_t> serial_receive() override;

  private:
    void fill();

    int fd_;
    std::chrono::microseconds check_interval_;
    std::chrono::steady_clock::time_point last_check_{};
    std::optional<termios> saved_;
    bool end_of_file_{};
    unsigned calls_{};
    std::deque<std::uint8_t> pending_;
    std::function<void(std::string_view)> command_;
    // Collecting a monitor command after Ctrl-].
    bool in_command_{};
    std::string line_;
};

} // namespace ultraviolent::backends
