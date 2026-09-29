#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace ultraviolent {

// The host side of a guest Ethernet port: the "cable". The guest-visible device (a MAC)
// decides timing, filtering, and descriptors; the link only carries frames. A frame is
// destination address first, without preamble or frame check sequence. Backends implement it
// (a host TAP device, or a queue for tests).
class EthernetLink {
  public:
    virtual ~EthernetLink() = default;

    // A frame the guest transmitted.
    virtual void send(std::span<const std::byte> frame) = 0;
    // The next frame for the guest, if one has arrived. The device polls when it can take one,
    // so a backend never pushes frames into guest state behind the scheduler's back.
    virtual std::optional<std::vector<std::byte>> receive() = 0;

  protected:
    EthernetLink() = default;
    EthernetLink(const EthernetLink&) = default;
    EthernetLink& operator=(const EthernetLink&) = default;
};

} // namespace ultraviolent
