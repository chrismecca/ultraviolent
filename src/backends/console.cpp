#include <ultraviolent/backends/console.hpp>

#include <utility>

namespace ultraviolent::backends {

ScriptedConsole::ScriptedConsole(std::FILE* output, std::vector<std::string> lines, bool at_prompt)
    : output_{output}, lines_{lines.begin(), lines.end()}, at_prompt_{at_prompt} {}

namespace {

constexpr std::string_view disc_directive = "@cdrom ";
constexpr std::string_view expect_directive = "@expect ";
constexpr std::string_view stop_directive = "@stop";
constexpr std::size_t expect_window = 8192;

} // namespace

void ScriptedConsole::release_line() {
    while (pending_.empty() && !lines_.empty() && lines_.front().starts_with(disc_directive)) {
        if (change_disc_) {
            change_disc_(lines_.front().substr(disc_directive.size()));
        }
        lines_.pop_front();
        seen_.clear();
    }
    if (pending_.empty() && !lines_.empty() && !lines_.front().starts_with('@')) {
        pending_ = lines_.front() + "\r";
        lines_.pop_front();
        tail_.clear();
        seen_.clear();
        idle_polls_ = 0;
        std::fflush(output_);
    }
}

// "@expect A|B|..." holds the script until one of the alternatives has appeared in the
// output since the previous line or directive; "@stop" asks the machine to stop.
void ScriptedConsole::run_directives() {
    while (pending_.empty() && !lines_.empty()) {
        const std::string& line = lines_.front();
        if (line.starts_with(expect_directive)) {
            std::string_view rest = std::string_view{line}.substr(expect_directive.size());
            bool found = false;
            while (!found) {
                const std::size_t bar = rest.find('|');
                found = seen_.find(rest.substr(0, bar)) != std::string::npos;
                if (bar == std::string_view::npos) {
                    break;
                }
                rest = rest.substr(bar + 1);
            }
            if (!found) {
                return;
            }
            seen_.clear();
            lines_.pop_front();
        } else if (line == stop_directive) {
            lines_.pop_front();
            std::fflush(output_);
            if (stop_) {
                stop_();
            }
        } else {
            return;
        }
    }
}

void ScriptedConsole::serial_transmit(std::uint8_t byte) {
    std::fputc(byte, output_);
    unflushed_ = byte != '\n';
    if (!unflushed_) {
        std::fflush(output_);
    }
    idle_polls_ = 0;
    tail_.push_back(static_cast<char>(byte));
    if (tail_.size() > 8) {
        tail_.erase(0, tail_.size() - 8);
    }
    seen_.push_back(static_cast<char>(byte));
    if (seen_.size() > 2 * expect_window) {
        seen_.erase(0, seen_.size() - expect_window);
    }
}

std::optional<std::uint8_t> ScriptedConsole::serial_receive() {
    // The guest is asking for input (or, with the IOC3's DMA rings, the IOC3 is sampling the
    // line). A line is typed once the guest waits at a prompt: its output ends in a command
    // prompt ("POD ... Dex> "), a shell prompt ("# ", "$ ", "% "), a question ("... ? [n] ",
    // "Option? ", "(dksc) "), or a request ("... press <enter>: ") and it has polled a while
    // without printing, or it has polled much longer after any output. The wait lets a program
    // drain typeahead before prompting without swallowing the line meant for the prompt.
    if (after_ != nullptr && lines_.empty() && pending_.empty()) {
        if (unflushed_) {
            std::fflush(output_);
            unflushed_ = false;
        }
        return after_->serial_receive();
    }
    if (at_prompt_) {
        // A resumed run: the first line goes at the first poll.
        at_prompt_ = false;
        release_line();
    }
    run_directives();
    if (pending_.empty() && idle_polls_ < idle_poll_limit) {
        ++idle_polls_;
    }
    const bool prompt = tail_.ends_with("> ") || tail_.ends_with("] ") || tail_.ends_with("? ") ||
                        tail_.ends_with(": ") || tail_.ends_with(") ") || tail_.ends_with("# ") ||
                        tail_.ends_with("$ ") || tail_.ends_with("% ");
    if (prompt && idle_polls_ >= prompt_poll_limit && lines_.empty() && pending_.empty() &&
        script_done_) {
        std::fflush(output_);
        std::exchange(script_done_, nullptr)();
    }
    if ((prompt && idle_polls_ >= prompt_poll_limit) || idle_polls_ >= idle_poll_limit) {
        release_line();
    }
    if (pending_.empty()) {
        return std::nullopt;
    }
    const auto byte = static_cast<std::uint8_t>(pending_.front());
    pending_.erase(0, 1);
    return byte;
}

} // namespace ultraviolent::backends
