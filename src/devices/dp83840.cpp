#include <ultraviolent/devices/dp83840.hpp>

#include <cstring>
#include <span>
#include <utility>

namespace ultraviolent::devices {

namespace {

// IEEE 802.3 clause 22 registers and bits.
constexpr unsigned bmcr = 0;
constexpr unsigned bmsr = 1;
constexpr unsigned phy_id1 = 2;
constexpr unsigned phy_id2 = 3;
constexpr unsigned anar = 4;
constexpr unsigned anlpar = 5;
constexpr unsigned aner = 6;
constexpr std::uint16_t bmcr_reset = 0x8000;
constexpr std::uint16_t bmcr_an_enable = 0x1000;
constexpr std::uint16_t bmcr_power_down = 0x0800;
constexpr std::uint16_t bmcr_isolate = 0x0400;
constexpr std::uint16_t bmcr_an_restart = 0x0200;
constexpr std::uint16_t bmsr_an_complete = 0x0020;
constexpr std::uint16_t bmsr_link = 0x0004;
constexpr std::uint16_t anlpar_ack = 0x4000;
constexpr std::uint16_t aner_partner_an_able = 0x0001;

// Reset values (observed: the enet_phy_reg diagnostic's "exp" values). BMSR: 100BASE-TX and
// 10BASE-T in both duplexes, auto-negotiation ability, extended capabilities; no link.
constexpr std::uint16_t bmcr_reset_value = 0x3100;
constexpr std::uint16_t bmsr_abilities = 0x7809;
constexpr std::uint16_t id1_value = 0x2000;
constexpr std::uint16_t id2_value = 0x5c00;
constexpr std::uint16_t anar_reset_value = 0x01e1;
// Vendor registers, as the diagnostic expects them after reset. Register 25's low five bits
// read as the PHY's MII address (31 on the BaseIO: the diagnostic masks 0x1f and expects it).
constexpr unsigned vendor_23 = 23;
constexpr unsigned vendor_24 = 24;
constexpr unsigned vendor_25 = 25;
constexpr unsigned vendor_28 = 28;
constexpr std::uint16_t vendor_23_reset = 0x8040;
constexpr std::uint16_t vendor_25_reset = 0x005f;
constexpr std::uint16_t vendor_28_reset = 0x0039;
// hypothesis: the partner advertises 100BASE-TX and 10BASE-T, both duplexes.
constexpr std::uint16_t partner_abilities = 0x01e1;

} // namespace

Dp83840::Dp83840(Scheduler& scheduler, Tracer& tracer, std::string name)
    : scheduler_{scheduler}, tracer_{tracer}, name_{std::move(name)},
      negotiated_{scheduler.add_event(name_ + ".negotiated", [this] { complete_negotiation(); })} {
    reset();
}

void Dp83840::reset() {
    scheduler_.cancel(negotiated_);
    registers_ = {};
    registers_[bmcr] = bmcr_reset_value;
    registers_[bmsr] = bmsr_abilities;
    registers_[phy_id1] = id1_value;
    registers_[phy_id2] = id2_value;
    registers_[anar] = anar_reset_value;
    registers_[vendor_23] = vendor_23_reset;
    registers_[vendor_25] = vendor_25_reset;
    registers_[vendor_28] = vendor_28_reset;
    link_up_ = false;
    link_latched_low_ = true;
    start_negotiation();
}

void Dp83840::set_reset(bool asserted) {
    if (asserted == held_in_reset_) {
        return;
    }
    held_in_reset_ = asserted;
    tracer_.log(TraceCategory::ethernet, "{} reset {}", name_, asserted ? "asserted" : "released");
    reset();
}

void Dp83840::set_link_partner(bool present) {
    if (present == partner_) {
        return;
    }
    partner_ = present;
    if (!present) {
        scheduler_.cancel(negotiated_);
        link_up_ = false;
        link_latched_low_ = true;
        registers_[anlpar] = 0;
        registers_[aner] = 0;
        return;
    }
    start_negotiation();
}

void Dp83840::start_negotiation() {
    scheduler_.cancel(negotiated_);
    link_up_ = false;
    link_latched_low_ = true;
    registers_[anlpar] = 0;
    registers_[aner] = 0;
    const std::uint16_t control = registers_[bmcr];
    if (!partner_ || held_in_reset_ || (control & (bmcr_power_down | bmcr_isolate)) != 0) {
        return;
    }
    scheduler_.schedule_after(negotiated_, negotiation_time);
}

void Dp83840::complete_negotiation() {
    if ((registers_[bmcr] & bmcr_an_enable) != 0) {
        registers_[anlpar] = static_cast<std::uint16_t>(anlpar_ack | partner_abilities);
        registers_[aner] = aner_partner_an_able;
    }
    link_up_ = true;
    tracer_.log(TraceCategory::ethernet, "{} link up", name_);
}

std::uint16_t Dp83840::read(unsigned reg) {
    reg &= 31;
    if (reg == bmsr) {
        std::uint16_t value = bmsr_abilities;
        if (link_up_ && !link_latched_low_) {
            value |= bmsr_link;
        }
        if (link_up_ && (registers_[bmcr] & bmcr_an_enable) != 0) {
            value |= bmsr_an_complete;
        }
        link_latched_low_ = false;
        return value;
    }
    return registers_[reg];
}

void Dp83840::write(unsigned reg, std::uint16_t value) {
    reg &= 31;
    tracer_.log(TraceCategory::ethernet, "{} write {} = {:#06x}", name_, reg, value);
    switch (reg) {
    case bmcr:
        if ((value & bmcr_reset) != 0) {
            reset();
            return;
        }
        registers_[bmcr] = static_cast<std::uint16_t>(value & ~bmcr_an_restart);
        if ((value & bmcr_an_restart) != 0 || (value & bmcr_an_enable) == 0) {
            start_negotiation();
        }
        return;
    case bmsr:
    case phy_id1:
    case phy_id2:
    case anlpar:
    case aner:
        return; // read-only
    case vendor_25:
        // hypothesis: the address bits are strapped; the rest is writable.
        registers_[reg] = static_cast<std::uint16_t>((value & ~0x1fu) | (vendor_25_reset & 0x1f));
        return;
    case anar:
    case vendor_23:
    case vendor_24:
    default:
        registers_[reg] = value;
        return;
    }
}

void Dp83840::save_state(StateImage& image) const {
    image.put_bytes(name_ + ".registers", std::as_bytes(std::span{registers_}));
    image.put(name_ + ".partner", partner_);
    image.put(name_ + ".held_in_reset", held_in_reset_);
    image.put(name_ + ".link_up", link_up_);
    image.put(name_ + ".link_latched_low", link_latched_low_);
}

void Dp83840::load_state(const StateImage& image) {
    if (const auto bytes = image.get_bytes(name_ + ".registers");
        bytes && bytes->size() == sizeof registers_) {
        std::memcpy(registers_.data(), bytes->data(), sizeof registers_);
    }
    image.get(name_ + ".partner", partner_);
    image.get(name_ + ".held_in_reset", held_in_reset_);
    image.get(name_ + ".link_up", link_up_);
    image.get(name_ + ".link_latched_low", link_latched_low_);
}

} // namespace ultraviolent::devices
