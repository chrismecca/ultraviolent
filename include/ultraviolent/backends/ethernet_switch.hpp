#pragma once

#include <ultraviolent/backends/tap_link.hpp>
#include <ultraviolent/core/ethernet_link.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ultraviolent::backends {

// A learning Ethernet switch's forwarding decisions (ORCHESTRATION.adoc "O2"), apart from
// any sockets: frames from broadcast, multicast, or unknown destinations go to every other
// port; a known unicast destination goes to the port it was last seen on, or nowhere if that
// is the sender's own port. Entries expire after `max_age` without a frame from them.
class SwitchTable {
  public:
    using Clock = std::chrono::steady_clock;
    using Mac = std::array<std::byte, 6>;
    static constexpr std::chrono::seconds default_max_age{300};

    explicit SwitchTable(std::chrono::seconds max_age = default_max_age) : max_age_{max_age} {}

    // The ports `frame` (destination first, then source) received on `from` goes to, among
    // `ports`; learns the source. A frame shorter than its two addresses goes nowhere.
    std::vector<unsigned> forward(unsigned from, std::span<const std::byte> frame,
                                  std::span<const unsigned> ports, Clock::time_point now);
    // A port went away: forget what was learned on it.
    void forget(unsigned port);
    [[nodiscard]] std::size_t entries() const {
        return table_.size();
    }

  private:
    struct Entry {
        unsigned port;
        Clock::time_point seen;
    };
    std::chrono::seconds max_age_;
    std::map<Mac, Entry> table_;
};

// The switch on a Unix socket: `ultraviolent switch SOCKET` (src/switch_command.cpp). Each
// machine connects with SwitchLink; one message is one frame. An optional TAP port joins the
// host. A slow port loses frames rather than holding up the others, as a congested switch
// would.
class SwitchServer {
  public:
    // Listens on `socket_path` (which must not exist), with `tap` as one more port if given.
    static std::expected<std::unique_ptr<SwitchServer>, std::string>
    open(const std::string& socket_path, const std::string& tap);
    SwitchServer(const SwitchServer&) = delete;
    SwitchServer& operator=(const SwitchServer&) = delete;
    ~SwitchServer();

    // Serves until `stop` becomes true (checked at least every `poll_interval`).
    void run(const volatile bool& stop,
             std::chrono::milliseconds poll_interval = std::chrono::milliseconds{200});
    // One round: waits up to `timeout` for frames or connections and handles them.
    void serve_once(std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t ports() const {
        return ports_.size();
    }

  private:
    // A machine's connection, or the TAP (fd -1: read and written through tap_).
    struct Port {
        unsigned number;
        int fd;
    };
    SwitchServer(int listener, std::string path) : listener_{listener}, path_{std::move(path)} {}
    void accept_port();
    void close_port(std::size_t index);
    void deliver(const Port& to, std::span<const std::byte> frame);

    int listener_;
    std::string path_;
    std::unique_ptr<TapLink> tap_;
    std::vector<Port> ports_;
    unsigned next_port_{1};
    SwitchTable table_;
    std::vector<std::byte> buffer_ = std::vector<std::byte>(65536);
};

// A guest Ethernet port plugged into a SwitchServer's socket. If the switch goes away the
// cable is unplugged from then on (frames are dropped), as with a switch turned off. Arrival
// times depend on the host, so runs with it are not deterministic.
class SwitchLink final : public EthernetLink {
  public:
    static std::unique_ptr<SwitchLink> open(const std::string& socket_path, std::string& error);
    SwitchLink(const SwitchLink&) = delete;
    SwitchLink& operator=(const SwitchLink&) = delete;
    ~SwitchLink() override;

    void send(std::span<const std::byte> frame) override;
    std::optional<std::vector<std::byte>> receive() override;

  private:
    explicit SwitchLink(int fd) : fd_{fd} {}
    void lost();
    int fd_;
    std::vector<std::byte> buffer_ = std::vector<std::byte>(65536);
};

} // namespace ultraviolent::backends
