#pragma once

#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/devices/one_wire.hpp>
#include <ultraviolent/machines/ip27/numalink.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace ultraviolent::ip27 {

// The Origin 2000 Router ASIC (SN0 "SPIDER"; MULTINODE.adoc, IP27.adoc "Router"): six ports, a
// crossbar, routing tables, scratch registers, and the router board's MicroLAN with its NIC.
//
// Software reaches it only by vector PIO. Ordinary traffic between nodes is delivered by the
// machine directly (hop timing is not guest-visible), so the routing tables are kept as
// written. Registers are modeled as the PROM is observed to use them; every other offset is
// storage and is traced, so a run names what software touches.
class Router final : public VectorTarget {
  public:
    static constexpr unsigned port_count = 6;
    // observed: the PROM's discover_object takes status bits 3:0 of 1 for a router (0 is a Hub).
    static constexpr std::uint64_t chip_id = 1;

    // `slot_bits`: the router board's slot as its midplane connector tells the chip (status
    // bits 11:8). observed: the PROM maps 0xf to r1 and 0xe to r2 for a module's router, and
    // 7 down to 0 to r1-r8 for a meta router, whose bit 3 is clear (tables at payload offsets
    // 0xd1eb8 and 0xd1ef8).
    Router(Tracer& tracer, std::string name, unsigned slot_bits);

    // The cable on `port` (1-6) to `peer`'s `peer_port`. Connect both ends.
    void connect(unsigned port, VectorTarget& peer, unsigned peer_port);
    // The router board's MicroLAN, with its NIC.
    void connect_mlan(devices::OneWireBus& bus) {
        mlan_bus_ = &bus;
    }
    [[nodiscard]] bool connected(unsigned port) const {
        return port >= 1 && port <= port_count && links_[port].peer != nullptr;
    }

    VectorResult vector_request(std::uint64_t route, unsigned port, unsigned type,
                                std::uint64_t address, std::uint64_t data) override;
    // A warm reset arriving on `port` goes out of every other connected port whose bit (port
    // − 1) is set in the arriving port's reset mask (MULTINODE.adoc "N2"). The router's own
    // registers are kept.
    void link_warm_reset(unsigned port) override;

    void reset();
    // Under `prefix`, such as "router1.".
    void save_state(StateImage& image, const std::string& prefix) const;
    void load_state(const StateImage& image, const std::string& prefix);

    // The register at `offset`, as a request that entered through `port` reads it.
    [[nodiscard]] std::uint64_t read(std::uint64_t offset, unsigned port) const;
    void write(std::uint64_t offset, std::uint64_t value);

  private:
    struct Link {
        VectorTarget* peer{};
        unsigned peer_port{};
    };

    Tracer& tracer_;
    std::string name_;
    unsigned slot_bits_;
    std::array<Link, port_count + 1> links_{};
    devices::OneWireBus* mlan_bus_{};
    std::uint64_t mlan_ctl_{};
    bool mlan_line_{true};
    // Every other register software has written, by offset.
    std::map<std::uint64_t, std::uint64_t> registers_;
    // Set while a warm reset passes through, so one that comes back around a loop of routers
    // stops here.
    bool passing_reset_{};
};

} // namespace ultraviolent::ip27
