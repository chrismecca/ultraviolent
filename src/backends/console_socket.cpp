#include <ultraviolent/backends/console_socket.hpp>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ultraviolent::backends {

namespace {

// Looking at the clock on every poll would cost more than the poll.
constexpr unsigned clock_stride = 64;
// Output is sent in pieces of at most this, or at a line's end, or when the guest polls.
constexpr std::size_t send_threshold = 256;

} // namespace

std::expected<std::unique_ptr<ConsoleSocket>, std::string>
ConsoleSocket::open(const std::string& path, devices::SerialSink& output,
                    std::chrono::milliseconds check_interval) {
    sockaddr_un where{};
    where.sun_family = AF_UNIX;
    if (path.size() >= sizeof(where.sun_path)) {
        return std::unexpected{"socket path too long: " + path};
    }
    std::memcpy(where.sun_path, path.c_str(), path.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return std::unexpected{std::string{"socket: "} + std::strerror(errno)};
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&where), sizeof where) != 0 ||
        ::listen(fd, 4) != 0) {
        const std::string why = std::strerror(errno);
        ::close(fd);
        return std::unexpected{"cannot listen on " + path + ": " + why};
    }
    return std::unique_ptr<ConsoleSocket>{new ConsoleSocket{fd, path, output, check_interval}};
}

ConsoleSocket::~ConsoleSocket() {
    detach();
    ::close(listener_);
    ::unlink(path_.c_str());
}

void ConsoleSocket::detach() {
    input_.reset();
    if (client_ >= 0) {
        ::close(client_);
        client_ = -1;
    }
    unsent_.clear();
}

void ConsoleSocket::flush() {
    if (client_ < 0 || unsent_.empty()) {
        return;
    }
    // A client that does not keep up loses output rather than holding up the guest.
    const ssize_t sent =
        ::send(client_, unsent_.data(), unsent_.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0 && (errno == EPIPE || errno == ECONNRESET)) {
        detach();
        return;
    }
    unsent_.clear();
}

void ConsoleSocket::serial_transmit(std::uint8_t byte) {
    output_.serial_transmit(byte);
    recent_.push_back(byte);
    if (recent_.size() > replay_bytes) {
        recent_.pop_front();
    }
    if (client_ >= 0) {
        unsent_.push_back(byte);
        if (byte == '\n' || unsent_.size() >= send_threshold) {
            flush();
        }
    }
}

void ConsoleSocket::check() {
    flush();
    if (input_ && input_->ended()) {
        detach();
    }
    const int fd = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) {
        return;
    }
    // A new client takes the console over from an old one.
    detach();
    client_ = fd;
    input_.emplace(fd, std::chrono::microseconds{0});
    if (command_) {
        input_->on_command(command_);
    }
    unsent_.assign(recent_.begin(), recent_.end());
    flush();
}

std::optional<std::uint8_t> ConsoleSocket::serial_receive() {
    if ((calls_++ % clock_stride) == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_check_ >= check_interval_) {
            last_check_ = now;
            check();
        }
    }
    if (!input_) {
        return std::nullopt;
    }
    return input_->serial_receive();
}

} // namespace ultraviolent::backends
