#include <ultraviolent/machines/ip27/router.hpp>

#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace ultraviolent::ip27 {

namespace {

// Register offsets in the router's vector space (IP27.adoc "Router"). observed: the PROM's
// discover_router, cache_router_nic, router lock, and router MicroLAN functions, by
// disassembly; names after the SN1 router's IRIX-derived header where the role matches.
constexpr std::uint64_t rr_status_rev_id = 0x0;
// observed: SCR0 caches the router board's NIC in bits 47:0; SCR1 is the PROM's lock (an
// exchange of 0xffff takes it when bits 15:0 were 0; a write of 0 releases it).
constexpr std::uint64_t rr_scratch0 = 0x20;
// observed: the router's MicroLAN control, written and polled like the Hub's MD_MLAN_CTL
// (PULSE 19:10, SAMPLE 9:2, DONE 1, RD_DATA 0).
constexpr std::uint64_t rr_mlan_ctl = 0x38;
constexpr std::uint64_t mlan_control = 0x3'ffff'fffc;
constexpr std::uint64_t mlan_done = 0x2;
constexpr std::uint64_t mlan_rd_data = 0x1;
// observed: status bits 48:46 hold the port the request came in on ("Router path ... inport
// is"); each port's link-up bit is bit 29 + 3 × (port − 1).
constexpr unsigned status_inport_shift = 46;
constexpr unsigned status_link_shift = 29;
constexpr unsigned status_slot_shift = 8;
constexpr unsigned status_link_stride = 3;
// hypothesis: revision 1 in bits 7:4 (the PROM keeps the field; no value is observed).
constexpr std::uint64_t router_revision = 1;

} // namespace

Router::Router(Tracer& tracer, std::string name, unsigned slot_bits)
    : tracer_{tracer}, name_{std::move(name)}, slot_bits_{slot_bits & 0xf} {}

void Router::connect(unsigned port, VectorTarget& peer, unsigned peer_port) {
    links_.at(port) = {.peer = &peer, .peer_port = peer_port};
}

VectorResult Router::vector_request(std::uint64_t route, unsigned port, unsigned type,
                                    std::uint64_t address, std::uint64_t data) {
    const auto exit = static_cast<unsigned>(route & 0xf);
    if (exit != 0) {
        // Pass the request on through `exit`, taking this hop off the route.
        if (exit > port_count || links_[exit].peer == nullptr) {
            tracer_.log(TraceCategory::hub, "{} vector route {:#x} from port {}: port {} unconnected",
                        name_, route, port, exit);
            return {};
        }
        return links_[exit].peer->vector_request(route >> 4, links_[exit].peer_port, type,
                                                 address, data);
    }
    std::uint64_t old = 0;
    if (type != piotype_write) {
        old = read(address, port);
    }
    if (type != piotype_read) {
        write(address, data);
    }
    tracer_.log(TraceCategory::hub, "{} vector type {} address {:#x} from port {} data {:#x} -> {:#x}",
                name_, type, address, port, data, old);
    return {.kind = VectorResult::Kind::response, .value = old};
}

std::uint64_t Router::read(std::uint64_t offset, unsigned port) const {
    switch (offset) {
        case rr_status_rev_id: {
            std::uint64_t status = chip_id | router_revision << 4 |
                                   std::uint64_t{slot_bits_} << status_slot_shift |
                                   std::uint64_t{port & 7} << status_inport_shift;
            for (unsigned p = 1; p <= port_count; ++p) {
                if (links_[p].peer != nullptr) {
                    status |= std::uint64_t{1} << (status_link_shift + status_link_stride * (p - 1));
                }
            }
            return status;
        }
        case rr_mlan_ctl:
            return (mlan_ctl_ & mlan_control) | mlan_done | (mlan_line_ ? mlan_rd_data : 0);
        default: {
            const auto it = registers_.find(offset);
            return it == registers_.end() ? 0 : it->second;
        }
    }
}

void Router::write(std::uint64_t offset, std::uint64_t value) {
    switch (offset) {
        case rr_status_rev_id:
            // hypothesis: status is read-only.
            return;
        case rr_mlan_ctl:
            mlan_ctl_ = value & mlan_control;
            mlan_line_ = mlan_bus_ == nullptr ||
                         mlan_bus_->pulse((value >> 10) & 0x3ff, (value >> 2) & 0xff);
            return;
        default:
            registers_[offset] = value;
    }
}

void Router::reset() {
    // hypothesis: undocumented reset values are zero (the scratch registers too: the PROM
    // reads SCR0's NIC field as 0 before it caches the NIC).
    registers_.clear();
    mlan_ctl_ = 0;
    mlan_line_ = true;
}

void Router::save_state(StateImage& image, const std::string& prefix) const {
    std::vector<std::uint64_t> pairs;
    for (const auto& [offset, value] : registers_) {
        pairs.push_back(offset);
        pairs.push_back(value);
    }
    image.put_bytes(prefix + "registers", std::as_bytes(std::span{pairs}));
    image.put(prefix + "mlan_ctl", mlan_ctl_);
    image.put(prefix + "mlan_line", std::uint8_t{mlan_line_});
}

void Router::load_state(const StateImage& image, const std::string& prefix) {
    registers_.clear();
    if (const auto bytes = image.get_bytes(prefix + "registers")) {
        std::vector<std::uint64_t> pairs(bytes->size() / sizeof(std::uint64_t));
        std::memcpy(pairs.data(), bytes->data(), pairs.size() * sizeof(std::uint64_t));
        for (std::size_t i = 0; i + 1 < pairs.size(); i += 2) {
            registers_[pairs[i]] = pairs[i + 1];
        }
    }
    image.get(prefix + "mlan_ctl", mlan_ctl_);
    std::uint8_t line = 1;
    image.get(prefix + "mlan_line", line);
    mlan_line_ = line != 0;
}

} // namespace ultraviolent::ip27
