#pragma once

#include <ultraviolent/devices/uart16550.hpp>

#include <cstdio>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ultraviolent::backends {

// The guest console on the host: output goes to a stdio stream; input comes from a script of
// lines, each released when the guest waits for input after printing a prompt (output ending
// in "> ", "] ", "? ", ": ", ") ", "# ", "$ ", or "% " and 200 input polls without output, or
// any output and 20000 polls), then fed a byte at a time as the guest asks for it. Lines
// starting with '@' are directives: "@cdrom FILE", "@expect A|B", "@stop".
// Deterministic: input depends only on guest behavior. Once the script is used up, input can
// pass to another source (an interactive terminal; see then_read).
class ScriptedConsole final : public devices::SerialSink, public devices::SerialSource {
  public:
    // With `at_prompt`, the guest is taken to be at a prompt already (a run resumed from a
    // snapshot), so the first line is typed at once.
    ScriptedConsole(std::FILE* output, std::vector<std::string> lines, bool at_prompt);

    // A script line "@cdrom PATH" is not typed: when its turn comes, `change_disc` runs with
    // PATH, and the next line follows at once.
    void on_disc_change(std::function<void(const std::string&)> change_disc) {
        change_disc_ = std::move(change_disc);
    }
    // Called once when the script is used up and the guest waits at a recognized prompt.
    void on_script_done(std::function<void()> done) {
        script_done_ = std::move(done);
    }
    // Called for a script line "@stop".
    void on_stop(std::function<void()> stop) {
        stop_ = std::move(stop);
    }

    // When the script is used up, input comes from `source`, and output is flushed before
    // each read so that prompts without a newline appear.
    void then_read(devices::SerialSource& source) {
        after_ = &source;
    }

    void serial_transmit(std::uint8_t byte) override;
    void release_line();
    std::optional<std::uint8_t> serial_receive() override;

  private:
    std::FILE* output_;
    std::deque<std::string> lines_;
    std::string tail_;
    std::string pending_;
    std::function<void(const std::string&)> change_disc_;
    std::function<void()> script_done_;
    std::function<void()> stop_;
    devices::SerialSource* after_{};
    bool unflushed_{};
    // Output since the last line or directive, for "@expect".
    std::string seen_;
    void run_directives();
    bool at_prompt_;
    // Input polls since the guest last printed. hypothesis: a program that polls this many
    // times in a row without output is waiting, at a recognized prompt or at any output.
    static constexpr unsigned prompt_poll_limit = 200;
    static constexpr unsigned idle_poll_limit = 20'000;
    unsigned idle_polls_{};
};

} // namespace ultraviolent::backends
