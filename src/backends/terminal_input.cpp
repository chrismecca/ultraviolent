#include <ultraviolent/backends/terminal_input.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>

#include <fcntl.h>
#include <unistd.h>

namespace ultraviolent::backends {

namespace {

// Reading the clock on every poll would cost more than the poll; look every this many calls.
constexpr unsigned clock_stride = 16;

} // namespace

TerminalInput::TerminalInput(int fd, std::chrono::microseconds check_interval)
    : fd_{fd}, check_interval_{check_interval} {
    termios mode{};
    if (::isatty(fd_) != 0 && ::tcgetattr(fd_, &mode) == 0) {
        saved_ = mode;
        // Raw input: bytes as typed, no echo, no line editing, no signal or flow-control keys,
        // Enter as CR (what a serial terminal sends). Reads return at once (VMIN = VTIME = 0).
        // Output processing stays on for the host terminal.
        mode.c_iflag &= ~static_cast<tcflag_t>(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                                               ICRNL | IXON);
        mode.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        mode.c_cc[VMIN] = 0;
        mode.c_cc[VTIME] = 0;
        ::tcsetattr(fd_, TCSANOW, &mode);
    } else {
        const int flags = ::fcntl(fd_, F_GETFL);
        if (flags >= 0) {
            ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        }
    }
}

TerminalInput::~TerminalInput() {
    if (saved_) {
        ::tcsetattr(fd_, TCSANOW, &*saved_);
    }
}

void TerminalInput::fill() {
    if (end_of_file_) {
        return;
    }
    std::array<std::uint8_t, 256> buffer{};
    const ssize_t count = ::read(fd_, buffer.data(), buffer.size());
    if (count == 0 && !saved_) {
        end_of_file_ = true;
        return;
    }
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            end_of_file_ = true;
        }
        return;
    }
    for (ssize_t i = 0; i < count; ++i) {
        const std::uint8_t byte = buffer[static_cast<std::size_t>(i)];
        if (in_command_) {
            if (byte == '\r' || byte == '\n') {
                std::fputc('\n', stderr);
                in_command_ = false;
                if (command_) {
                    command_(line_);
                }
            } else if ((byte == 0x7f || byte == 0x08) && !line_.empty()) {
                line_.pop_back();
                std::fputs("\b \b", stderr);
            } else if (byte >= 0x20 && byte < 0x7f) {
                line_.push_back(static_cast<char>(byte));
                std::fputc(byte, stderr);
            }
            continue;
        }
        if (byte == escape_byte) {
            in_command_ = true;
            line_.clear();
            std::fputs("\r\nultraviolent> ", stderr);
            continue;
        }
        pending_.push_back(byte);
    }
}

std::optional<std::uint8_t> TerminalInput::serial_receive() {
    if (pending_.empty() && (calls_++ % clock_stride) == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_check_ >= check_interval_) {
            last_check_ = now;
            fill();
        }
    }
    if (pending_.empty()) {
        return std::nullopt;
    }
    const std::uint8_t byte = pending_.front();
    pending_.pop_front();
    return byte;
}

} // namespace ultraviolent::backends
