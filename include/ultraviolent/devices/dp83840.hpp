#pragma once

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace ultraviolent::devices {

// A National DP83840-family 10/100 Ethernet PHY, as the BaseIO's IOC3 reaches it over MII
// management (IP27.adoc "IOC3 Ethernet"). Registers 0-6 are IEEE 802.3 clause 22 (BMCR, BMSR,
// PHY identifier, auto-negotiation advertisement, link partner, expansion). observed (the
// BASEIO PROM's enet_phy_reg diagnostic): identifier 0x2000/0x5c0x, and the reset values and
// writable bits it checks, including vendor registers 23, 24, 25, and 28.
//
// The cable is a machine setting: with a link partner, auto-negotiation completes
// `negotiation_time` after it starts and the link comes up at 100 Mb/s full duplex (the
// partner advertises everything). hypothesis: the partner's abilities and the time.
class Dp83840 {
  public:
    static constexpr VirtualDuration negotiation_time{1'500'000'000};

    Dp83840(Scheduler& scheduler, Tracer& tracer, std::string name);

    // Whether a link partner is cabled to the port.
    void set_link_partner(bool present);

    [[nodiscard]] std::uint16_t read(unsigned reg);
    void write(unsigned reg, std::uint16_t value);
    // Hardware reset (power-on, and BMCR bit 15).
    void reset();
    // The RESET pin (active low on the BaseIO, driven by IOC3 GPIO 5): held in reset while
    // asserted; negotiation starts over when released.
    void set_reset(bool asserted);

    // The negotiated mode, for the MAC's backend: whether the link is up.
    [[nodiscard]] bool link_up() const {
        return link_up_;
    }
    // Transmitted frames return to the MAC: BMCR loopback (clause 22 bit 14), or a loopback
    // mode in vendor register 24 bits 9:8. observed: the PROM's enet_tw_loop ("TWISTER
    // LOOPBACK") writes 0x0100 there with BMCR loopback off, and its write test finds bits 8,
    // 9, and 11 writable. hypothesis: any nonzero mode loops frames back at the wire side.
    [[nodiscard]] bool loopback() const {
        return (registers_[0] & 0x4000) != 0 || (registers_[24] & 0x0300) != 0;
    }

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    void start_negotiation();
    void complete_negotiation();

    Scheduler& scheduler_;
    Tracer& tracer_;
    std::string name_;
    EventId negotiated_;
    std::array<std::uint16_t, 32> registers_{};
    bool partner_{};
    bool held_in_reset_{};
    bool link_up_{};
    // BMSR link status latches low until read (clause 22).
    bool link_latched_low_{};
};

} // namespace ultraviolent::devices
