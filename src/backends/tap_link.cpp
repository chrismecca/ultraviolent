#include <ultraviolent/backends/tap_link.hpp>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ultraviolent::backends {

namespace {

// The largest frame a TAP device delivers without offloads (a VLAN-tagged maximum frame).
constexpr std::size_t maximum_frame = 1522;

} // namespace

std::unique_ptr<TapLink> TapLink::open(const std::string& name, std::string& error) {
    if (name.empty() || name.size() >= IFNAMSIZ) {
        error = "not a network interface name: " + name;
        return nullptr;
    }
    const int fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = std::string{"cannot open /dev/net/tun: "} + std::strerror(errno);
        return nullptr;
    }
    ifreq request{};
    // A TAP device (Ethernet frames), without the packet-information header.
    request.ifr_flags = IFF_TAP | IFF_NO_PI;
    std::memcpy(request.ifr_name, name.data(), name.size());
    if (::ioctl(fd, TUNSETIFF, &request) < 0) {
        error = "cannot attach to TAP interface " + name + ": " + std::strerror(errno) +
                " (create it with: sudo ip tuntap add dev " + name + " mode tap user $USER)";
        ::close(fd);
        return nullptr;
    }
    return std::unique_ptr<TapLink>{new TapLink{fd}};
}

TapLink::~TapLink() {
    ::close(fd_);
}

void TapLink::send(std::span<const std::byte> frame) {
    // A frame the host cannot take is lost, as on a congested wire.
    [[maybe_unused]] const ssize_t written = ::write(fd_, frame.data(), frame.size());
}

std::optional<std::vector<std::byte>> TapLink::receive() {
    std::vector<std::byte> frame(maximum_frame);
    const ssize_t count = ::read(fd_, frame.data(), frame.size());
    if (count <= 0) {
        return std::nullopt;
    }
    frame.resize(static_cast<std::size_t>(count));
    return frame;
}

} // namespace ultraviolent::backends
