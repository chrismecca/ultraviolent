#include <ultraviolent/devices/xbow.hpp>

// Offsets and bits: the IRIX-derived Linux arch/ia64/sn/include/xtalk/xbow.h (XBOW_WID_*,
// XB_LINK_*, XB_STAT_*, XB_AUX_*) and asm/xtalk/xwidget.h (WIDGET_*).
namespace ultraviolent::devices {

namespace {

// Widget 0 registers modeled as storage (xbow.h XBOW_WID_*): control, request timeout,
// interrupt destination, LLP configuration, arbitration reload, performance counters A/B.
constexpr std::uint64_t stored_widget_registers[] = {
    xtalk::widget_control,
    xtalk::widget_req_timeout,
    xtalk::widget_intdest_upper,
    xtalk::widget_intdest_lower,
    xtalk::widget_llp_cfg,
    0x5c,
    0x64,
    0x6c,
};

// Per-port link registers (XB_LINK_REG_BASE(x) = 0x100 + (x & 7) * 0x40).
constexpr std::uint64_t link_base = 0x100;
constexpr std::uint64_t link_stride = 0x40;
constexpr std::uint64_t link_ibuf_flush = 0x04;
constexpr std::uint64_t link_control = 0x0c;
constexpr std::uint64_t link_status = 0x14;
constexpr std::uint64_t link_arb_upper = 0x1c;
constexpr std::uint64_t link_arb_lower = 0x24;
constexpr std::uint64_t link_status_clr = 0x2c;
constexpr std::uint64_t link_reset = 0x34;
constexpr std::uint64_t link_aux_status = 0x3c;

constexpr std::uint32_t stat_linkalive = 0x8000'0000; // XB_STAT_LINKALIVE
constexpr std::uint32_t aux_present = 0x20;           // XB_AUX_STAT_PRESENT
// XB_WID_STAT_REG_ACC_ERR, and the control bit that enables its interrupt (XB_WID_CTRL_*).
constexpr std::uint32_t stat_reg_acc_err = 0x20;
constexpr std::uint32_t ctrl_reg_acc_ie = 0x20;
// WIDGET_INTDEST_UPPER_ADDR fields: vector 31:24, target 19:16, address 47:32 in 15:0.
constexpr std::uint32_t intdest_upper_fields = 0xff0f'ffff;
// observed: the PROM's xbow_sanity expects target-ID bit 3 (bit 19) to read 1 whatever is
// written (it writes 0xa5a5a5a5 and expects 0xa50da5a5); crosstalk targets are 8-15.
constexpr std::uint32_t intdest_target_bit3 = 0x0008'0000;

bool is_stored(std::uint64_t offset) {
    for (const std::uint64_t stored : stored_widget_registers) {
        if (stored == offset) {
            return true;
        }
    }
    return false;
}

} // namespace

Xbow::Xbow(Tracer& tracer) : tracer_{tracer} {
    reset();
}

void Xbow::attach(unsigned port, xtalk::Widget& widget) {
    ports_[port - xtalk::first_port] = &widget;
}

void Xbow::reset() {
    // hypothesis: undocumented reset values are zero.
    widget_registers_ = {};
    link_registers_ = {};
    status_ = 0;
    error_lower_ = 0;
}

void Xbow::access_error(std::uint64_t offset) {
    // observed (xbow_sanity): a write to a read-only register sets REG_ACC_ERR in the status
    // and, with REG_ACC_IE set in the control register, interrupts through INTDEST.
    // hypothesis: the error address registers capture the offset.
    status_ |= stat_reg_acc_err;
    error_lower_ = static_cast<std::uint32_t>(offset);
    tracer_.log(TraceCategory::xbow, "register access error at {:#06x}", offset);
    if ((widget_registers_[xtalk::widget_control / 8] & ctrl_reg_acc_ie) == 0) {
        return;
    }
    const std::uint32_t upper = widget_registers_[xtalk::widget_intdest_upper / 8];
    const std::uint32_t lower = widget_registers_[xtalk::widget_intdest_lower / 8];
    const unsigned target = ((upper >> 16) & 0xf) | 0x8;
    const std::uint64_t address = (std::uint64_t{upper & 0xffff} << 32) | lower;
    send_interrupt(target, address, static_cast<std::uint8_t>(upper >> 24));
}

void Xbow::send_interrupt(unsigned target, std::uint64_t address, std::uint8_t vector) {
    if (target >= xtalk::first_port && target < xtalk::widget_count) {
        if (xtalk::Widget* widget = ports_[target - xtalk::first_port]) {
            widget->xtalk_interrupt(address, vector);
            return;
        }
    }
    tracer_.log(TraceCategory::xbow, "interrupt to absent widget {:#x}", target);
}

void Xbow::send_interrupt_clear(unsigned target, std::uint64_t address, std::uint8_t vector) {
    if (target >= xtalk::first_port && target < xtalk::widget_count) {
        if (xtalk::Widget* widget = ports_[target - xtalk::first_port]) {
            widget->xtalk_interrupt_clear(address, vector);
        }
    }
}

bool Xbow::dma_read(unsigned target, std::uint64_t address, std::span<std::byte> bytes) {
    if (target >= xtalk::first_port && target < xtalk::widget_count) {
        if (xtalk::Widget* widget = ports_[target - xtalk::first_port]) {
            return widget->xtalk_dma_read(address, bytes);
        }
    }
    tracer_.log(TraceCategory::xbow, "DMA read from absent widget {:#x}", target);
    return false;
}

bool Xbow::dma_write(unsigned target, std::uint64_t address, std::span<const std::byte> bytes) {
    if (target >= xtalk::first_port && target < xtalk::widget_count) {
        if (xtalk::Widget* widget = ports_[target - xtalk::first_port]) {
            return widget->xtalk_dma_write(address, bytes);
        }
    }
    tracer_.log(TraceCategory::xbow, "DMA write to absent widget {:#x}", target);
    return false;
}

std::expected<std::uint32_t, AccessFault> Xbow::read_register(std::uint64_t offset) {
    if (offset >= link_base && offset < link_base + port_count * link_stride) {
        const std::size_t port = (offset - link_base) / link_stride;
        const std::uint64_t reg = (offset - link_base) % link_stride;
        const bool attached = ports_[port] != nullptr;
        switch (reg) {
        case link_status:
        case link_status_clr:
            // An attached widget's link is alive; no link ever reports an error.
            return attached ? stat_linkalive : 0;
        case link_aux_status:
            // hypothesis: PRESENT for an attached widget; retry counts, timeout destination,
            // and PORT_WIDTH read 0.
            return attached ? aux_present : 0;
        case link_control:
        case link_arb_upper:
        case link_arb_lower:
            return link_registers_[port][reg / 8];
        case link_ibuf_flush:
        case link_reset:
            return 0;
        default:
            break;
        }
    } else {
        switch (offset) {
        case xtalk::widget_id:
            return id;
        case xtalk::widget_status:
            // observed (xbow_sanity): reading XBOW_WID_STAT twice shows the error both times.
            return status_;
        case xtalk::widget_tflush: {
            // XBOW_WID_STAT_CLR: the status, cleared by the read (observed: xbow_sanity then
            // requires XBOW_WID_STAT to read 0 on crossbows before revision 5).
            const std::uint32_t status = status_;
            status_ = 0;
            return status;
        }
        case xtalk::widget_err_lower:
            return error_lower_;
        case xtalk::widget_err_upper:
        case xtalk::widget_err_cmd_word:
            return 0;
        case xtalk::widget_intdest_upper:
            return (widget_registers_[offset / 8] & intdest_upper_fields) | intdest_target_bit3;
        default:
            if (is_stored(offset)) {
                return widget_registers_[offset / 8];
            }
            break;
        }
    }
    tracer_.log(TraceCategory::xbow, "unmodeled read {:#06x}", offset);
    return std::unexpected(AccessFault::unsupported);
}

std::expected<void, AccessFault> Xbow::write_register(std::uint64_t offset, std::uint32_t value) {
    if (offset >= link_base && offset < link_base + port_count * link_stride) {
        const std::size_t port = (offset - link_base) / link_stride;
        const std::uint64_t reg = (offset - link_base) % link_stride;
        switch (reg) {
        case link_control:
        case link_arb_upper:
        case link_arb_lower:
            link_registers_[port][reg / 8] = value;
            return {};
        case link_status_clr:
        case link_ibuf_flush:
            return {};
        case link_reset:
            // XB_LINK_RESET resets the link and the widget on it (hypothesis for the widget;
            // observed: the PROM writes it for the BaseIO's port as it boots, and after an
            // IRIX restart expects the Bridge and its PCI devices at their reset values).
            tracer_.log(TraceCategory::xbow, "link {:#x} reset", port + xtalk::first_port);
            if (ports_[port] != nullptr) {
                ports_[port]->xtalk_reset();
            }
            return {};
        default:
            break;
        }
    } else if (is_stored(offset)) {
        widget_registers_[offset / 8] = value;
        return {};
    } else if (offset == xtalk::widget_status || offset == xtalk::widget_tflush ||
               offset == xtalk::widget_err_cmd_word) {
        // hypothesis: writes clear the error state.
        status_ = 0;
        return {};
    } else if (offset == xtalk::widget_id || offset == xtalk::widget_err_upper ||
               offset == xtalk::widget_err_lower) {
        // Read-only: the write completes and is an access error.
        access_error(offset);
        return {};
    }
    tracer_.log(TraceCategory::xbow, "unmodeled write {:#06x} value {:#x}", offset, value);
    return std::unexpected(AccessFault::unsupported);
}

std::expected<std::uint64_t, AccessFault> Xbow::xtalk_read(unsigned widget, std::uint64_t offset,
                                                           AccessWidth width) {
    if (widget >= xtalk::first_port && widget < xtalk::widget_count) {
        if (MmioTarget* target = ports_[widget - xtalk::first_port]) {
            return target->mmio_read(offset, width);
        }
    } else if (widget == 0) {
        // 32-bit registers at offset 4 mod 8, or a doubleword with the register in its low
        // half (hypothesis).
        if ((width == AccessWidth::bits32 && offset % 8 == 4) ||
            (width == AccessWidth::bits64 && offset % 8 == 0)) {
            const auto value = read_register(offset | 4);
            tracer_.log(TraceCategory::xbow, "read {:#06x} = {:#x}", offset | 4, value.value_or(0));
            return value;
        }
        tracer_.log(TraceCategory::xbow, "unmodeled read {:#06x} width {}", offset,
                    byte_count(width));
        return std::unexpected(AccessFault::unsupported);
    }
    tracer_.log(TraceCategory::xbow, "no widget {:#x} (read {:#x})", widget, offset);
    return std::unexpected(AccessFault::unmapped);
}

std::expected<void, AccessFault> Xbow::xtalk_write(unsigned widget, std::uint64_t offset,
                                                   AccessWidth width, std::uint64_t value) {
    if (widget >= xtalk::first_port && widget < xtalk::widget_count) {
        if (MmioTarget* target = ports_[widget - xtalk::first_port]) {
            return target->mmio_write(offset, width, value);
        }
    } else if (widget == 0) {
        if ((width == AccessWidth::bits32 && offset % 8 == 4) ||
            (width == AccessWidth::bits64 && offset % 8 == 0)) {
            tracer_.log(TraceCategory::xbow, "write {:#06x} = {:#x}", offset | 4, value);
            return write_register(offset | 4, static_cast<std::uint32_t>(value));
        }
        tracer_.log(TraceCategory::xbow, "unmodeled write {:#06x} width {}", offset,
                    byte_count(width));
        return std::unexpected(AccessFault::unsupported);
    }
    tracer_.log(TraceCategory::xbow, "no widget {:#x} (write {:#x})", widget, offset);
    return std::unexpected(AccessFault::unmapped);
}

void Xbow::save_state(StateImage& image) const {
    image.put("xbow.widget_registers", widget_registers_);
    image.put("xbow.link_registers", link_registers_);
    image.put("xbow.status", status_);
    image.put("xbow.error_lower", error_lower_);
}

void Xbow::load_state(const StateImage& image) {
    image.get("xbow.widget_registers", widget_registers_);
    image.get("xbow.link_registers", link_registers_);
    image.get("xbow.status", status_);
    image.get("xbow.error_lower", error_lower_);
}

} // namespace ultraviolent::devices
