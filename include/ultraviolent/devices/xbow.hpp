#pragma once

#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/xtalk/xtalk.hpp>

#include <array>
#include <cstdint>
#include <expected>

namespace ultraviolent::devices {

// The SGI Crossbow (XBOW) crosstalk switch (IP27.adoc "Crossbow"). It answers widget 0 with
// its own registers and forwards requests for widgets 8-15 to what is attached to those
// ports. Register layout from the IRIX-derived Linux arch/ia64/sn/include/xtalk/xbow.h; each
// register is 32 bits, in the low half of a doubleword (offset 4 mod 8).
//
// Only register storage, identity, and link presence are modeled: the switch never reports
// errors, credits and arbitration have no effect, and performance counters do not count.
class Xbow final : public xtalk::Link, public xtalk::Fabric {
  public:
    static constexpr unsigned port_count = 8;
    // XBOW_WIDGET_PART_NUM, XBOW_WIDGET_MFGR_NUM. hypothesis: revision 4, XBOW_REV_1_3. For
    // revisions from 5 the PROM expects a source-ID field in the status register (observed in
    // xbow_sanity), which is not modeled.
    static constexpr std::uint32_t id = xtalk::make_widget_id(4, 0x0000, 0x0);

    explicit Xbow(Tracer& tracer);

    // Connects `widget` to port `port` (8-15).
    void attach(unsigned port, xtalk::Widget& widget);

    void reset();

    std::expected<std::uint64_t, AccessFault> xtalk_read(unsigned widget, std::uint64_t offset,
                                                         AccessWidth width) override;
    std::expected<void, AccessFault> xtalk_write(unsigned widget, std::uint64_t offset,
                                                 AccessWidth width, std::uint64_t value) override;

    void send_interrupt(unsigned target, std::uint64_t address, std::uint8_t vector) override;
    void send_interrupt_clear(unsigned target, std::uint64_t address, std::uint8_t vector) override;
    bool dma_read(unsigned target, std::uint64_t address, std::span<std::byte> bytes) override;
    bool dma_write(unsigned target, std::uint64_t address,
                   std::span<const std::byte> bytes) override;

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    [[nodiscard]] std::expected<std::uint32_t, AccessFault> read_register(std::uint64_t offset);
    std::expected<void, AccessFault> write_register(std::uint64_t offset, std::uint32_t value);
    // A write to a read-only register: records the error and interrupts if enabled.
    void access_error(std::uint64_t offset);

    Tracer& tracer_;
    std::array<xtalk::Widget*, port_count> ports_{};
    std::uint32_t status_{};
    std::uint32_t error_lower_{};
    // Widget 0 registers modeled as storage, indexed by doubleword (offset / 8), 0x000-0x0ff.
    std::array<std::uint32_t, 32> widget_registers_{};
    // Per-port link registers, indexed by doubleword within the port's 0x40-byte block.
    std::array<std::array<std::uint32_t, 8>, port_count> link_registers_{};
};

} // namespace ultraviolent::devices
