#pragma once

#include <ultraviolent/core/address_space.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

// Crosstalk (XIO), the point-to-point I/O fabric of SGI Origin-class systems (IP27.adoc "Xtalk").
// Register names and layouts: the IRIX-derived Linux headers asm/xtalk/xwidget.h and
// arch/ia64/sn/include/xtalk/xbow.h.
namespace ultraviolent::xtalk {

// Widget numbers are 0-15: 0 is a crossbow's own registers, 8-15 its ports.
inline constexpr unsigned widget_count = 16;
inline constexpr unsigned first_port = 8;

// Each widget's small window, the part of its address space a Hub reaches through PIO (16 MB).
inline constexpr std::uint64_t small_window_size = std::uint64_t{1} << 24;

// Standard widget configuration registers (xwidget.h): 32-bit registers, each in the low half
// of a big-endian doubleword.
inline constexpr std::uint64_t widget_id = 0x04;
inline constexpr std::uint64_t widget_status = 0x0c;
inline constexpr std::uint64_t widget_err_upper = 0x14;
inline constexpr std::uint64_t widget_err_lower = 0x1c;
inline constexpr std::uint64_t widget_control = 0x24;
inline constexpr std::uint64_t widget_req_timeout = 0x2c;
inline constexpr std::uint64_t widget_intdest_upper = 0x34;
inline constexpr std::uint64_t widget_intdest_lower = 0x3c;
inline constexpr std::uint64_t widget_err_cmd_word = 0x44;
inline constexpr std::uint64_t widget_llp_cfg = 0x4c;
inline constexpr std::uint64_t widget_tflush = 0x54;

// WIDGET_ID: revision 31:28, part number 27:12, manufacturer 11:1, and bit 0 set. inferred:
// this is the JTAG IDCODE layout (IEEE 1149.1, whose LSB is always 1), and the IP27 PROM
// rejects a Bridge ID whose LSB is not 1 (observed string "Bad bridge id register value (LSB
// != 1)").
constexpr std::uint32_t make_widget_id(unsigned revision, unsigned part, unsigned manufacturer) {
    return (static_cast<std::uint32_t>(revision & 0xf) << 28) |
           (static_cast<std::uint32_t>(part & 0xffff) << 12) |
           (static_cast<std::uint32_t>(manufacturer & 0x7ff) << 1) | 1u;
}

// A widget as a crossbow port sees it: PIO requests to its space, and interrupts other widgets
// send it. An interrupt is a write of `vector` to `address` (the sender's INTDEST registers,
// xwidget.h WIDGET_INTDEST_UPPER_ADDR/LOWER_ADDR).
class Widget : public MmioTarget {
  public:
    virtual void xtalk_interrupt(std::uint64_t address, std::uint8_t vector) = 0;
    // The link to this widget was reset (a crossbow's LINK_RESET for its port): widgets that
    // reset with their link return to their power-on state.
    virtual void xtalk_reset() {}
    // An interrupt clear packet for `vector` (a Bridge sends one when a level-mode interrupt
    // pin falls). Widgets that keep no interrupt state ignore it.
    virtual void xtalk_interrupt_clear(std::uint64_t /*address*/, std::uint8_t /*vector*/) {}

    // A block read or write another widget masters (DMA) at crosstalk byte `address` in this
    // widget's space, bytes in address order. Widgets that are not DMA targets refuse.
    virtual bool xtalk_dma_read(std::uint64_t /*address*/, std::span<std::byte> /*bytes*/) {
        return false;
    }
    virtual bool xtalk_dma_write(std::uint64_t /*address*/, std::span<const std::byte> /*bytes*/) {
        return false;
    }

  protected:
    Widget() = default;
    Widget(const Widget&) = default;
    Widget& operator=(const Widget&) = default;
};

// What a widget sends interrupts through: a crossbow delivers an interrupt to the widget on
// port `target`.
class Fabric {
  public:
    virtual ~Fabric() = default;
    virtual void send_interrupt(unsigned target, std::uint64_t address, std::uint8_t vector) = 0;
    virtual void send_interrupt_clear(unsigned /*target*/, std::uint64_t /*address*/,
                                      std::uint8_t /*vector*/) {}
    // A DMA request to the widget on port `target`; fails when nothing there accepts it.
    virtual bool dma_read(unsigned /*target*/, std::uint64_t /*address*/,
                          std::span<std::byte> /*bytes*/) {
        return false;
    }
    virtual bool dma_write(unsigned /*target*/, std::uint64_t /*address*/,
                           std::span<const std::byte> /*bytes*/) {
        return false;
    }

  protected:
    Fabric() = default;
    Fabric(const Fabric&) = default;
    Fabric& operator=(const Fabric&) = default;
};

// The far end of a crosstalk link: it takes PIO requests addressed to a widget and an offset
// in that widget's space. A request nobody answers fails; the requester reports a bus error.
class Link {
  public:
    virtual ~Link() = default;
    virtual std::expected<std::uint64_t, AccessFault>
    xtalk_read(unsigned widget, std::uint64_t offset, AccessWidth width) = 0;
    virtual std::expected<void, AccessFault>
    xtalk_write(unsigned widget, std::uint64_t offset, AccessWidth width, std::uint64_t value) = 0;

  protected:
    Link() = default;
    Link(const Link&) = default;
    Link& operator=(const Link&) = default;
};

} // namespace ultraviolent::xtalk
