#pragma once

#include <ultraviolent/core/ethernet_link.hpp>
#include <ultraviolent/core/virtual_clock.hpp>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ultraviolent::backends {

// A cable into a capture file: every frame the guest sends is recorded in libpcap format
// (Ethernet link type) with its virtual time, and nothing arrives. Runs with it stay
// deterministic; tcpdump -r or Wireshark read the file.
class PcapLink final : public EthernetLink {
  public:
    static std::unique_ptr<PcapLink> open(const std::string& path, const VirtualClock& clock);
    PcapLink(const PcapLink&) = delete;
    PcapLink& operator=(const PcapLink&) = delete;
    ~PcapLink() override;

    void send(std::span<const std::byte> frame) override;
    std::optional<std::vector<std::byte>> receive() override;

  private:
    PcapLink(std::FILE* file, const VirtualClock& clock) : file_{file}, clock_{clock} {}
    std::FILE* file_;
    const VirtualClock& clock_;
};

} // namespace ultraviolent::backends
