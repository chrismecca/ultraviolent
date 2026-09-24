#include <ultraviolent/machines/ip27/hub.hpp>

#include <iterator>
#include <utility>

// Register offsets and names are from the public Linux headers, several of which state that
// they derive from IRIX headers (LINUX in IP27.adoc). Each register's behavior cites its
// evidence; IP27.adoc "Hub" has the table.
namespace ultraviolent::ip27 {

namespace {

// Processor interface (PI), LINUX sn0/hubpi.h.
constexpr std::uint64_t pi_cpu_num = 0x00'0020;
constexpr std::uint64_t pi_calias_size = 0x00'0028;
constexpr std::uint64_t pi_cpu_present_a = 0x00'0040;
constexpr std::uint64_t pi_cpu_present_b = 0x00'0048;
constexpr std::uint64_t pi_cpu_enable_a = 0x00'0050;
constexpr std::uint64_t pi_cpu_enable_b = 0x00'0058;
constexpr std::uint64_t pi_int_pend_mod = 0x00'0090;
constexpr std::uint64_t pi_int_pend0 = 0x00'0098;
constexpr std::uint64_t pi_int_pend1 = 0x00'00a0;
constexpr std::uint64_t pi_int_mask0_a = 0x00'00a8;
constexpr std::uint64_t pi_int_mask1_a = 0x00'00b0;
constexpr std::uint64_t pi_int_mask0_b = 0x00'00b8;
constexpr std::uint64_t pi_int_mask1_b = 0x00'00c0;
constexpr std::uint64_t pi_rt_compare_a = 0x00'0108;
constexpr std::uint64_t pi_rt_compare_b = 0x00'0110;
constexpr std::uint64_t pi_rt_local_ctrl = 0x00'0160;
constexpr std::uint64_t pi_err_stack_addr_a = 0x00'0418;
constexpr std::uint64_t pi_err_stack_addr_b = 0x00'0420;
constexpr std::uint64_t pi_rt_count = 0x03'0100;
// Memory/directory (MD), LINUX sn0/hubmd.h.
constexpr std::uint64_t md_perf_sel = 0x21'0000;
constexpr std::uint64_t md_perf_cnt0 = 0x21'0010;
// Junk bus: MD_UREG0_0 is the PCF8584's S0, MD_UREG0_1 its S1 (observed: the PROM writes
// PCF8584 control values to UREG0_1 and data to UREG0_0).
constexpr std::uint64_t md_ureg0_0 = 0x22'0000;
constexpr std::uint64_t md_ureg0_1 = 0x22'0008;
constexpr std::uint64_t md_slotid_ustat = 0x22'0048;
constexpr std::uint64_t md_led0 = 0x22'0050;
constexpr std::uint64_t md_led1 = 0x22'0058;
// I/O interface (II), LINUX sn0/hubio.h (derived from IRIX sys/SN/SN0/hubio.h).
constexpr std::uint64_t iio_ilcsr = 0x40'0128;
constexpr std::uint64_t iio_scratch_reg0 = 0x40'0150;
constexpr std::uint64_t iio_scratch_reg1 = 0x40'0158;
// Network interface (NI), LINUX sn0/hubni.h.
constexpr std::uint64_t ni_status_rev_id = 0x60'0000;
constexpr std::uint64_t ni_port_reset = 0x60'0008;
constexpr std::uint64_t ni_protection = 0x60'0010;
constexpr std::uint64_t ni_scratch_reg0 = 0x60'0100;
constexpr std::uint64_t ni_scratch_reg1 = 0x60'0108;

// Registers modeled as storage: they hold what is written, masked to their fields, and have
// no other modeled effect. The index is the slot in Hub::stored_.
struct StoredRegister {
    std::uint64_t offset;
    std::uint64_t mask;
};
constexpr std::uint64_t all_bits = ~std::uint64_t{0};
constexpr StoredRegister stored_registers[] = {
    // hypothesis: the cached-alias remapping selected by PI_CALIAS_SIZE is not modeled.
    {pi_calias_size, 0xf},
    // The PROM keeps early boot and failure codes here per CPU (observed); the compare
    // interrupt is not modeled.
    {pi_rt_compare_a, all_bits},
    {pi_rt_compare_b, all_bits},
    // hypothesis: stored only; its clock-source and filtering fields have no modeled effect.
    {pi_rt_local_ctrl, all_bits},
    // Performance monitor select and counters 0-5; event counting is not modeled.
    {md_perf_sel, all_bits},
    {md_perf_cnt0 + 0x00, all_bits},
    {md_perf_cnt0 + 0x08, all_bits},
    {md_perf_cnt0 + 0x10, all_bits},
    {md_perf_cnt0 + 0x18, all_bits},
    {md_perf_cnt0 + 0x20, all_bits},
    {md_perf_cnt0 + 0x28, all_bits},
    {iio_scratch_reg0, all_bits},
    {iio_scratch_reg1, all_bits},
    // hypothesis: stored only; remote-access protection has no effect on one node.
    {ni_protection, all_bits},
    {ni_scratch_reg0, all_bits},
    {ni_scratch_reg1, all_bits},
};
static_assert(std::size(stored_registers) == Hub::stored_register_count);

// Slot of a storage register, or stored_register_count.
constexpr std::size_t find_stored(std::uint64_t offset) {
    for (std::size_t i = 0; i < std::size(stored_registers); ++i) {
        if (stored_registers[i].offset == offset) {
            return i;
        }
    }
    return Hub::stored_register_count;
}

// The CPU slice of the requester. The target has one CPU, in slice A.
constexpr std::uint64_t requesting_slice = 0;
// hypothesis: slice A holds the only CPU; slice B is empty (IP27-TARGET.adoc).
constexpr std::uint64_t cpu_present[2] = {1, 0};

// IIO_ILCSR fields (hubii_ilcsr_t): max_burst 41:32, max_retry 25:16, lnk_stat 13:12, bm8 11,
// llp_en 10, wrm_reset 8, null_to 5:0.
constexpr std::uint64_t ilcsr_writable =
    (std::uint64_t{0x3ff} << 32) | (0x3ffu << 16) | (1u << 11) | (1u << 10) | (1u << 8) | 0x3fu;
constexpr std::uint64_t ilcsr_warm_reset = 1u << 8;
// lnk_stat = LNK_STAT_WORKING (2), IIO_LLP_CSR_IS_UP (0x2000).
constexpr std::uint64_t ilcsr_link_working = 2u << 12;

// NI_STATUS_REV_ID (LINUX sn0/hubni.h, sn0/hub.h):
//   node ID 16:8 = 0: all node IDs are zero after reset (PRM 1.2.3), documented;
//   chip ID 3:0 = CHIPID_HUB (0), IRIX-derived header;
//   link up 29 = 0, down reason 28 = 1 ("never came out of reset"): hypothesis for a node with
//   no CrayLink cable.
constexpr std::uint64_t nsri_down_never_reset = std::uint64_t{1} << 28;
constexpr unsigned nsri_rev_shift = 4;

// PI_INT_PEND_MOD (LINUX asm/sn/intr.h, "copied from Irix"): writing 0x100 | level sets
// pending interrupt `level` (0-127), writing `level` clears it. Levels 0-63 are INT_PEND0 bits,
// 64-127 INT_PEND1 bits. inferred from the IRIX-derived header.
constexpr std::uint64_t pend_mod_set = 0x100;
constexpr std::uint64_t pend_mod_level = 0x7f;

// MD_SLOTID_USTAT (LINUX sn0/hubmd.h): FPROMRDY bit 4, I2CINTR bit 3, SN0 slot ID 2:0.
// inferred: the flash is always ready (programming is not modeled); I2CINTR follows the
// PCF8584's interrupt output. hypothesis: slot ID 0, and CORECLK, NETSYNC, CORECLK_TST 0.
constexpr std::uint64_t msu_fpromrdy = 1u << 4;
constexpr std::uint64_t msu_i2cintr = 1u << 3;
constexpr std::uint64_t node_slot_id = 0;

// NI_PORT_RESET bits (LINUX): send warm reset on the link; reset the entire Hub.
constexpr std::uint64_t npr_portreset = 1u << 7;
constexpr std::uint64_t npr_localreset = 1u << 0;

// inferred: PI_RT_COUNT is 52 bits wide (LINUX ip27-timer.c registers a 52-bit sched_clock).
constexpr std::uint64_t rt_count_mask = (std::uint64_t{1} << 52) - 1;

} // namespace

Hub::Hub(Scheduler& scheduler, Tracer& tracer, unsigned revision, devices::Pcf8584& i2c,
         std::function<void()> reset_node)
    : scheduler_{scheduler}, tracer_{tracer}, revision_{revision}, i2c_{i2c},
      reset_node_{std::move(reset_node)},
      local_reset_event_{scheduler.add_event("hub.local_reset", [this] { local_reset(); })} {
    reset(ResetKind::power_on);
    // Nothing survives power-on.
    err_stack_addr_ = {};
}

std::uint64_t Hub::rt_count() const {
    return (rt_base_ + cycles_at(scheduler_.now(), rtc_clock) - rt_epoch_ticks_) & rt_count_mask;
}

void Hub::set_rt_count(std::uint64_t value) {
    rt_base_ = value & rt_count_mask;
    rt_epoch_ticks_ = cycles_at(scheduler_.now(), rtc_clock);
}

void Hub::set_led(unsigned slice, std::uint64_t value) {
    // Eight-bit LEDs, one bank per CPU slice (LINUX MD_LED0/MD_LED1). The PROM reports its
    // boot phase here (PRM 3.1, Table 3-1).
    leds_[slice] = value & 0xff;
    tracer_.log(TraceCategory::firmware, "led {} {:#04x}", slice == 0 ? 'A' : 'B', leds_[slice]);
}

void Hub::local_reset() {
    // hypothesis: a Hub local reset resets the node's CPUs with a cold reset and reinitializes
    // the Hub's modeled registers, except the error stack addresses, which the PROM relies on
    // to survive it (IP27.adoc "Hub local reset").
    tracer_.log(TraceCategory::hub, "local reset");
    reset(ResetKind::cold);
    reset_node_();
}

void Hub::connect_cpu(unsigned slice, InterruptSink& cpu) {
    cpu_lines_[slice][0].connect(cpu, 0);
    cpu_lines_[slice][1].connect(cpu, 1);
}

void Hub::update_interrupts() {
    for (std::size_t slice = 0; slice < 2; ++slice) {
        for (std::size_t word = 0; word < 2; ++word) {
            cpu_lines_[slice][word].set_level((pending_[word] & interrupt_mask_[slice][word]) != 0);
        }
    }
}

void Hub::reset(ResetKind kind) {
    if (kind == ResetKind::warm) {
        // hypothesis: a soft reset leaves the Hub's registers unchanged.
        return;
    }
    // hypothesis: undocumented reset values are zero, and present CPUs start enabled
    // (PRM 3.1: the PROM disables a failing CPU by clearing its PI_CPU_ENABLE bit).
    stored_ = {};
    leds_ = {};
    ilcsr_ = 0;
    pending_ = {};
    interrupt_mask_ = {};
    update_interrupts();
    cpu_enable_ = {cpu_present[0], cpu_present[1]};
    set_rt_count(0);
}

std::expected<std::uint64_t, AccessFault> Hub::mmio_read(std::uint64_t offset, AccessWidth width) {
    if (width == AccessWidth::bits64) {
        if (const std::size_t slot = find_stored(offset); slot < stored_register_count) {
            return stored_[slot];
        }
        switch (offset) {
        case pi_cpu_num:
            return requesting_slice;
        case pi_cpu_present_a:
            return cpu_present[0];
        case pi_cpu_present_b:
            return cpu_present[1];
        case pi_int_pend0:
            return pending_[0];
        case pi_int_pend1:
            return pending_[1];
        case pi_int_mask0_a:
            return interrupt_mask_[0][0];
        case pi_int_mask1_a:
            return interrupt_mask_[0][1];
        case pi_int_mask0_b:
            return interrupt_mask_[1][0];
        case pi_int_mask1_b:
            return interrupt_mask_[1][1];
        case pi_cpu_enable_a:
            return cpu_enable_[0];
        case pi_cpu_enable_b:
            return cpu_enable_[1];
        case pi_err_stack_addr_a:
            return err_stack_addr_[0];
        case pi_err_stack_addr_b:
            return err_stack_addr_[1];
        case pi_rt_count:
            return rt_count();
        case md_ureg0_0:
        case md_ureg0_1:
            // hypothesis: the byte-wide junk bus reads zero in bits 63:8.
            return i2c_.read(offset == md_ureg0_1);
        case md_slotid_ustat:
            return msu_fpromrdy | (i2c_.interrupt_asserted() ? msu_i2cintr : 0) | node_slot_id;
        case md_led0:
            // hypothesis: the LED latches read back their last value.
            return leds_[0];
        case md_led1:
            return leds_[1];
        case iio_ilcsr:
            // The target's Hub is cabled to a Crossbow, so the link reports working.
            return (ilcsr_ & ilcsr_writable) | ilcsr_link_working;
        case ni_status_rev_id:
            return nsri_down_never_reset | (std::uint64_t{revision_ & 0xf} << nsri_rev_shift);
        default:
            break;
        }
    }
    tracer_.log(TraceCategory::hub, "unmodeled read {:#08x} width {}", offset, byte_count(width));
    return std::unexpected(AccessFault::unsupported);
}

std::expected<void, AccessFault> Hub::mmio_write(std::uint64_t offset, AccessWidth width,
                                                 std::uint64_t value) {
    if (width == AccessWidth::bits64) {
        if (const std::size_t slot = find_stored(offset); slot < stored_register_count) {
            stored_[slot] = value & stored_registers[slot].mask;
            return {};
        }
        switch (offset) {
        case pi_cpu_num:
        case pi_cpu_present_a:
        case pi_cpu_present_b:
            // Read-only. inferred: the PROM's early exception handler stores Cause, EPC, and
            // BadVAddr in PI_CPU_NUM and continues, so the write has no effect.
            return {};
        case pi_int_pend_mod: {
            const std::uint64_t level = value & pend_mod_level;
            const std::uint64_t bit = std::uint64_t{1} << (level % 64);
            std::uint64_t& word = pending_[level / 64];
            word = (value & pend_mod_set) != 0 ? word | bit : word & ~bit;
            update_interrupts();
            return {};
        }
        case pi_int_mask0_a:
        case pi_int_mask1_a:
        case pi_int_mask0_b:
        case pi_int_mask1_b: {
            const std::uint64_t index = (offset - pi_int_mask0_a) / 8;
            interrupt_mask_[index / 2][index % 2] = value;
            update_interrupts();
            return {};
        }
        case pi_cpu_enable_a:
            cpu_enable_[0] = value & 1;
            return {};
        case pi_cpu_enable_b:
            cpu_enable_[1] = value & 1;
            return {};
        case pi_err_stack_addr_a:
            err_stack_addr_[0] = value;
            return {};
        case pi_err_stack_addr_b:
            err_stack_addr_[1] = value;
            return {};
        case pi_rt_count:
            set_rt_count(value);
            return {};
        case md_ureg0_0:
        case md_ureg0_1:
            i2c_.write(offset == md_ureg0_1, static_cast<std::uint8_t>(value));
            return {};
        case md_led0:
            set_led(0, value);
            return {};
        case md_led1:
            set_led(1, value);
            return {};
        case iio_ilcsr:
            // hypothesis: the control fields hold what is written, including the warm-reset
            // bit, whose effect on the link is not modeled.
            if (((value ^ ilcsr_) & ilcsr_warm_reset) != 0) {
                tracer_.log(TraceCategory::hub, "llp warm reset {}",
                            (value & ilcsr_warm_reset) != 0 ? "asserted" : "released");
            }
            ilcsr_ = value & ilcsr_writable;
            return {};
        case ni_port_reset:
            // The reset takes effect after the store that requested it completes.
            if ((value & npr_localreset) != 0) {
                scheduler_.schedule_after(local_reset_event_, VirtualDuration{0});
            }
            if ((value & npr_portreset) != 0) {
                // No CrayLink port is connected on a one-node machine.
                tracer_.log(TraceCategory::hub, "port reset (no link)");
            }
            return {};
        default:
            break;
        }
    }
    tracer_.log(TraceCategory::hub, "unmodeled write {:#08x} width {} value {:#x}", offset,
                byte_count(width), value);
    return std::unexpected(AccessFault::unsupported);
}

} // namespace ultraviolent::ip27
