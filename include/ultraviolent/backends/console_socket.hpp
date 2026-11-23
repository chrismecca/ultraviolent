#pragma once

#include <ultraviolent/backends/terminal_input.hpp>
#include <ultraviolent/devices/uart16550.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ultraviolent::backends {

// The guest console on a Unix stream socket (ORCHESTRATION.adoc "O3"), for a machine run in
// the background: one client at a time attaches (`uvctl console`, or `socat - UNIX:PATH`),
// sees the console's last few kilobytes and then its output, and types to the guest. A
// client that leaves detaches; the next one may attach. Output also goes on to `output` (the
// scripted console, which writes the log and recognizes prompts). Input is read as
// TerminalInput reads a pipe, so Ctrl-] starts a monitor command there too. Not
// deterministic, as with any typed input.
class ConsoleSocket final : public devices::SerialSink, public devices::SerialSource {
  public:
    static constexpr std::size_t replay_bytes = 4096;

    // Listens on `path` (which must not exist; it is removed when the console goes).
    static std::expected<std::unique_ptr<ConsoleSocket>, std::string>
    open(const std::string& path, devices::SerialSink& output,
         std::chrono::milliseconds check_interval = std::chrono::milliseconds{20});
    ConsoleSocket(const ConsoleSocket&) = delete;
    ConsoleSocket& operator=(const ConsoleSocket&) = delete;
    ~ConsoleSocket() override;

    // Monitor commands from a client (see TerminalInput).
    void on_command(std::function<void(std::string_view)> command) {
        command_ = std::move(command);
    }
    [[nodiscard]] bool attached() const {
        return client_ >= 0;
    }

    void serial_transmit(std::uint8_t byte) override;
    std::optional<std::uint8_t> serial_receive() override;

  private:
    ConsoleSocket(int listener, std::string path, devices::SerialSink& output,
                  std::chrono::milliseconds check_interval)
        : listener_{listener}, path_{std::move(path)}, output_{output},
          check_interval_{check_interval} {}
    void check();
    void flush();
    void detach();

    int listener_;
    std::string path_;
    devices::SerialSink& output_;
    std::chrono::milliseconds check_interval_;
    std::chrono::steady_clock::time_point last_check_{};
    unsigned calls_{};
    int client_{-1};
    std::optional<TerminalInput> input_;
    std::function<void(std::string_view)> command_;
    std::deque<std::uint8_t> recent_;
    std::vector<std::uint8_t> unsent_;
};

} // namespace ultraviolent::backends
