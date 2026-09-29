#pragma once

#include <ultraviolent/core/ethernet_link.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ultraviolent::backends {

// A guest Ethernet port on a Linux TAP interface: frames the guest sends appear on the host
// interface, and frames the host sends to it reach the guest. It attaches to an existing
// persistent TAP device the user owns (created once with
// `ip tuntap add dev NAME mode tap user USER`), so running needs no privileges. Reads never
// block: the guest's MAC polls. Arrival times depend on the host, so runs with it are not
// deterministic.
class TapLink final : public EthernetLink {
  public:
    // Attaches to TAP interface `name`; on failure returns null and sets `error`.
    static std::unique_ptr<TapLink> open(const std::string& name, std::string& error);
    TapLink(const TapLink&) = delete;
    TapLink& operator=(const TapLink&) = delete;
    ~TapLink() override;

    void send(std::span<const std::byte> frame) override;
    std::optional<std::vector<std::byte>> receive() override;

  private:
    explicit TapLink(int fd) : fd_{fd} {}
    int fd_;
};

} // namespace ultraviolent::backends
