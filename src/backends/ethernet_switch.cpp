#include <ultraviolent/backends/ethernet_switch.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ultraviolent::backends {

namespace {

SwitchTable::Mac address(std::span<const std::byte> frame, std::size_t offset) {
    SwitchTable::Mac mac{};
    std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(offset), mac.size(), mac.begin());
    return mac;
}

// A Unix socket address for `path`; false if the path does not fit.
bool socket_address(const std::string& path, sockaddr_un& out) {
    out = {};
    out.sun_family = AF_UNIX;
    if (path.size() >= sizeof(out.sun_path)) {
        return false;
    }
    std::memcpy(out.sun_path, path.c_str(), path.size() + 1);
    return true;
}

} // namespace

std::vector<unsigned> SwitchTable::forward(unsigned from, std::span<const std::byte> frame,
                                           std::span<const unsigned> ports, Clock::time_point now) {
    if (frame.size() < 12) {
        return {};
    }
    const Mac destination = address(frame, 0);
    const Mac source = address(frame, 6);
    // A group source address is not a station's; it is never learned.
    if ((std::to_integer<unsigned>(source[0]) & 1) == 0) {
        table_[source] = Entry{from, now};
    }
    if ((std::to_integer<unsigned>(destination[0]) & 1) == 0) {
        if (const auto known = table_.find(destination); known != table_.end()) {
            if (now - known->second.seen <= max_age_ &&
                std::ranges::contains(ports, known->second.port)) {
                if (known->second.port == from) {
                    return {};
                }
                return {known->second.port};
            }
            table_.erase(known);
        }
    }
    std::vector<unsigned> flood;
    for (const unsigned port : ports) {
        if (port != from) {
            flood.push_back(port);
        }
    }
    return flood;
}

void SwitchTable::forget(unsigned port) {
    std::erase_if(table_, [port](const auto& entry) { return entry.second.port == port; });
}

std::expected<std::unique_ptr<SwitchServer>, std::string>
SwitchServer::open(const std::string& socket_path, const std::string& tap) {
    sockaddr_un where{};
    if (!socket_address(socket_path, where)) {
        return std::unexpected{"socket path too long: " + socket_path};
    }
    std::unique_ptr<TapLink> tap_link;
    if (!tap.empty()) {
        std::string error;
        tap_link = TapLink::open(tap, error);
        if (!tap_link) {
            return std::unexpected{error};
        }
    }
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return std::unexpected{std::string{"socket: "} + std::strerror(errno)};
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&where), sizeof where) != 0 ||
        ::listen(fd, 64) != 0) {
        const std::string why = std::strerror(errno);
        ::close(fd);
        return std::unexpected{"cannot listen on " + socket_path + ": " + why};
    }
    std::unique_ptr<SwitchServer> server{new SwitchServer{fd, socket_path}};
    if (tap_link) {
        server->tap_ = std::move(tap_link);
        server->ports_.push_back(Port{server->next_port_++, -1});
    }
    return server;
}

SwitchServer::~SwitchServer() {
    for (const Port& port : ports_) {
        if (port.fd >= 0) {
            ::close(port.fd);
        }
    }
    ::close(listener_);
    ::unlink(path_.c_str());
}

void SwitchServer::run(const volatile bool& stop, std::chrono::milliseconds poll_interval) {
    while (!stop) {
        serve_once(poll_interval);
    }
}

void SwitchServer::accept_port() {
    for (;;) {
        const int fd = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            return;
        }
        ports_.push_back(Port{next_port_++, fd});
    }
}

void SwitchServer::close_port(std::size_t index) {
    table_.forget(ports_[index].number);
    ::close(ports_[index].fd);
    ports_.erase(ports_.begin() + static_cast<std::ptrdiff_t>(index));
}

void SwitchServer::deliver(const Port& to, std::span<const std::byte> frame) {
    if (to.fd < 0) {
        tap_->send(frame);
        return;
    }
    // Dropped if the port's queue is full or it has gone (closed on its next read).
    [[maybe_unused]] const ssize_t sent =
        ::send(to.fd, frame.data(), frame.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
}

void SwitchServer::serve_once(std::chrono::milliseconds timeout) {
    std::vector<pollfd> waits;
    waits.push_back(pollfd{listener_, POLLIN, 0});
    for (const Port& port : ports_) {
        waits.push_back(pollfd{port.fd < 0 ? tap_->descriptor() : port.fd, POLLIN, 0});
    }
    if (::poll(waits.data(), waits.size(), static_cast<int>(timeout.count())) <= 0) {
        return;
    }
    // Ports by number, as they are before this round's arrivals and departures.
    std::vector<unsigned> numbers;
    for (const Port& port : ports_) {
        numbers.push_back(port.number);
    }
    const auto now = SwitchTable::Clock::now();
    std::vector<unsigned> closed;
    for (std::size_t i = 0; i < ports_.size(); ++i) {
        if ((waits[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
            continue;
        }
        const Port from = ports_[i];
        // Every frame waiting on this port.
        for (;;) {
            std::span<const std::byte> frame;
            std::optional<std::vector<std::byte>> tap_frame;
            if (from.fd < 0) {
                tap_frame = tap_->receive();
                if (!tap_frame) {
                    break;
                }
                frame = *tap_frame;
            } else {
                const ssize_t n = ::recv(from.fd, buffer_.data(), buffer_.size(), MSG_DONTWAIT);
                if (n == 0 ||
                    (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    closed.push_back(from.number);
                    break;
                }
                if (n < 0) {
                    break;
                }
                frame = std::span{buffer_}.first(static_cast<std::size_t>(n));
            }
            for (const unsigned to : table_.forward(from.number, frame, numbers, now)) {
                const auto port = std::ranges::find(ports_, to, &Port::number);
                if (port != ports_.end() && !std::ranges::contains(closed, port->number)) {
                    deliver(*port, frame);
                }
            }
        }
    }
    for (const unsigned number : closed) {
        const auto port = std::ranges::find(ports_, number, &Port::number);
        close_port(static_cast<std::size_t>(port - ports_.begin()));
    }
    if ((waits[0].revents & POLLIN) != 0) {
        accept_port();
    }
}

std::unique_ptr<SwitchLink> SwitchLink::open(const std::string& socket_path, std::string& error) {
    sockaddr_un where{};
    if (!socket_address(socket_path, where)) {
        error = "socket path too long: " + socket_path;
        return nullptr;
    }
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        error = std::string{"socket: "} + std::strerror(errno);
        return nullptr;
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&where), sizeof where) != 0) {
        error = "cannot connect to the switch at " + socket_path + ": " + std::strerror(errno) +
                " (start it with: ultraviolent switch " + socket_path + ")";
        ::close(fd);
        return nullptr;
    }
    return std::unique_ptr<SwitchLink>{new SwitchLink{fd}};
}

SwitchLink::~SwitchLink() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void SwitchLink::lost() {
    std::fputs("the Ethernet switch went away; the cable is unplugged\n", stderr);
    ::close(fd_);
    fd_ = -1;
}

void SwitchLink::send(std::span<const std::byte> frame) {
    if (fd_ < 0) {
        return;
    }
    const ssize_t sent = ::send(fd_, frame.data(), frame.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0 && (errno == EPIPE || errno == ECONNRESET)) {
        lost();
    }
}

std::optional<std::vector<std::byte>> SwitchLink::receive() {
    if (fd_ < 0) {
        return std::nullopt;
    }
    // The guest polls often, so the common answer, nothing, allocates nothing.
    const ssize_t n = ::recv(fd_, buffer_.data(), buffer_.size(), MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        lost();
        return std::nullopt;
    }
    if (n < 0) {
        return std::nullopt;
    }
    return std::vector<std::byte>(buffer_.begin(), buffer_.begin() + n);
}

} // namespace ultraviolent::backends
