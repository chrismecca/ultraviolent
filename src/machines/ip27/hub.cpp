#include <ultraviolent/machines/ip27/hub.hpp>

#include <ultraviolent/core/invariant.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <optional>
#include <tuple>
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
constexpr std::uint64_t pi_int_pend0 = Hub::int_pend0_offset;
constexpr std::uint64_t pi_int_pend1 = Hub::int_pend1_offset;
constexpr std::uint64_t pi_int_mask0_a = 0x00'00a8;
constexpr std::uint64_t pi_int_mask1_a = 0x00'00b0;
constexpr std::uint64_t pi_int_mask0_b = 0x00'00b8;
constexpr std::uint64_t pi_int_mask1_b = 0x00'00c0;
constexpr std::uint64_t pi_rt_compare_a = 0x00'0108;
constexpr std::uint64_t pi_rt_compare_b = 0x00'0110;
constexpr std::uint64_t pi_rt_local_ctrl = 0x00'0160;
constexpr std::uint64_t pi_profile_compare = 0x00'0118;
constexpr std::uint64_t pi_rt_pend_a = 0x00'0120;
constexpr std::uint64_t pi_rt_pend_b = 0x00'0128;
constexpr std::uint64_t pi_prof_pend_a = 0x00'0130;
constexpr std::uint64_t pi_prof_pend_b = 0x00'0138;
constexpr std::uint64_t pi_rt_en_a = 0x00'0140;
constexpr std::uint64_t pi_rt_en_b = 0x00'0148;
constexpr std::uint64_t pi_prof_en_a = 0x00'0150;
constexpr std::uint64_t pi_prof_en_b = 0x00'0158;
// PI error block (LINUX sn0/hubpi.h "Error and timeout registers").
constexpr std::uint64_t pi_err_int_pend = 0x00'0400;
constexpr std::uint64_t pi_err_int_mask_a = 0x00'0408;
constexpr std::uint64_t pi_err_int_mask_b = 0x00'0410;
constexpr std::uint64_t pi_err_stack_addr_a = 0x00'0418;
constexpr std::uint64_t pi_err_stack_addr_b = 0x00'0420;
constexpr std::uint64_t pi_err_stack_size = 0x00'0428;
constexpr std::uint64_t pi_err_status_first = 0x00'0430; // ERR_STATUS0_A ...
constexpr std::uint64_t pi_err_status_last = 0x00'0468;  // ... ERR_STATUS1_B_RCLR
constexpr std::uint64_t pi_sysad_errchk_en = 0x00'0490;
constexpr std::uint64_t pi_rt_count = 0x03'0100;
// Memory/directory (MD), LINUX sn0/hubmd.h.
constexpr std::uint64_t md_memory_config = 0x20'0018;
constexpr std::uint64_t md_refresh_control = 0x20'0020;
constexpr std::uint64_t md_mlan_ctl = 0x20'00a8;
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
constexpr std::uint64_t iio_wid = 0x40'0000;
constexpr std::uint64_t iio_wcr = 0x40'0020;
constexpr std::uint64_t iio_ilcsr = 0x40'0128;
constexpr std::uint64_t iio_scratch_reg0 = 0x40'0150;
constexpr std::uint64_t iio_scratch_reg1 = 0x40'0158;
// Network interface (NI), LINUX sn0/hubni.h.
constexpr std::uint64_t ni_status_rev_id = 0x60'0000;
constexpr std::uint64_t ni_port_reset = 0x60'0008;
constexpr std::uint64_t ni_protection = 0x60'0010;
constexpr std::uint64_t ni_global_parms = 0x60'0018;
constexpr std::uint64_t ni_diag_parms = 0x60'0110;
constexpr std::uint64_t ni_port_parms = 0x60'8000;
constexpr std::uint64_t ni_port_error = 0x60'8008;
constexpr std::uint64_t ni_port_error_clear = 0x60'8088;
constexpr std::uint64_t ni_scratch_reg0 = 0x60'0100;
constexpr std::uint64_t ni_scratch_reg1 = 0x60'0108;
constexpr std::uint64_t ni_vector_parms = 0x60'0200;
constexpr std::uint64_t ni_vector = 0x60'0208;
constexpr std::uint64_t ni_vector_data = 0x60'0210;
constexpr std::uint64_t ni_vector_status = 0x60'0300;
constexpr std::uint64_t ni_return_vector = 0x60'0308;
constexpr std::uint64_t ni_vector_read_data = 0x60'0310;
constexpr std::uint64_t ni_vector_clear = 0x60'0380;
constexpr std::uint64_t ni_base = 0x60'0000;
// NI_META_TABLE0 (32 entries) then NI_LOCAL_TABLE0 (16), contiguous: routing tables whose
// entries hold an exit port, NMT_/NLT_EXIT_PORT_MASK (LINUX sn0/hubni.h). hypothesis:
// stored only; no modeled packet is routed by them (no router), and they reset to 0.
constexpr std::uint64_t ni_routing_first = 0x63'8000;
constexpr std::uint64_t ni_routing_last = 0x63'8178;
constexpr std::uint64_t exit_port_mask = 0xf;

// Ranges of registers that report an idle, error-free Hub: reads return 0 and writes (error
// clears) are accepted with no effect. Nothing modeled produces errors, keeps requests in
// flight, or runs the block-transfer engines. Names from LINUX sn0/hubmd.h, hubio.h, hubni.h
// (IRIX-derived).
struct IdleRange {
    std::uint64_t first;
    std::uint64_t last;
    bool writable;
};
constexpr IdleRange idle_ranges[] = {
    // MD_DIR_ERROR, MD_PROTOCOL_ERROR, MD_MEM_ERROR, MD_MISC_ERROR and their _CLR forms.
    {0x20'0050, 0x20'0088, true},
    // IIO_WSTAT: widget status, nothing pending or timed out. Writes clear it.
    {0x40'0008, 0x40'0008, true},
    // IIO_ILLR: LLP log (check-bit and sequence-number error counts), no errors.
    {0x40'0130, 0x40'0130, true},
    // IIO_IECLR: I/O error clear.
    {0x40'01f8, 0x40'01f8, true},
    // IIO_ICRB_A..D(0..14): coherent request buffers, none valid. The PROM zeroes them
    // during INITII (observed); writes are accepted.
    {0x40'0400, 0x40'05d8, true},
};

constexpr const IdleRange* find_idle(std::uint64_t offset) {
    for (const IdleRange& range : idle_ranges) {
        if (offset >= range.first && offset <= range.last && offset % 8 == 0) {
            return &range;
        }
    }
    return nullptr;
}

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
    // Real-time and profile comparators (see Hub::schedule_comparators). The PROM also keeps
    // early boot and failure codes in the RT compare registers (observed).
    {pi_rt_compare_a, all_bits},
    {pi_rt_compare_b, all_bits},
    {pi_profile_compare, all_bits},
    // hypothesis: stored only; its clock-source and filtering fields have no modeled effect.
    {pi_rt_local_ctrl, all_bits},
    // Error interrupt masks, error stack size, SysAD error checking enables: stored. No
    // modeled hardware raises these errors.
    {pi_err_int_mask_a, all_bits},
    {pi_err_int_mask_b, all_bits},
    {pi_err_stack_size, all_bits},
    {pi_sysad_errchk_en, all_bits},
    // Performance monitor select and counters 0-5; event counting is not modeled.
    {md_perf_sel, all_bits},
    {md_perf_cnt0 + 0x00, all_bits},
    {md_perf_cnt0 + 0x08, all_bits},
    {md_perf_cnt0 + 0x10, all_bits},
    {md_perf_cnt0 + 0x18, all_bits},
    {md_perf_cnt0 + 0x20, all_bits},
    {md_perf_cnt0 + 0x28, all_bits},
    // IIO_SCRATCH_MASK: "not all bits available".
    {iio_scratch_reg0, 0x0000'000f'00f1'1fff},
    {iio_scratch_reg1, 0x0000'000f'00f1'1fff},
    // II configuration programmed by software (LINUX sn0/hubio.h, IRIX-derived): stored.
    // Their effects on routing, protection, and CRB management are not modeled yet.
    {0x40'0020, all_bits}, // IIO_WCR widget control
    {0x40'0100, all_bits}, // IIO_ILAPR local access protection
    {0x40'0108, all_bits}, // IIO_ILAPO protection override
    {0x40'0110, all_bits}, // IIO_IOWA outbound widget access
    {0x40'0118, all_bits}, // IIO_IIWA inbound widget access
    {0x40'0120, all_bits}, // IIO_IIDEM inbound device error mask
    {0x40'0138, all_bits}, // IIO_IIDSR interrupt destination
    {0x40'0140, all_bits}, // IIO_IGFX_0
    {0x40'0148, all_bits}, // IIO_IGFX_1
    // IIO_ITTE 1..7 (big-window translation, see Hub::BigWindow). inferred: the seven slots
    // between the scratch registers and IIO_IOPRB_0, matching IIO_NUM_ITTES.
    {0x40'0160, all_bits},
    {0x40'0168, all_bits},
    {0x40'0170, all_bits},
    {0x40'0178, all_bits},
    {0x40'0180, all_bits},
    {0x40'0188, all_bits},
    {0x40'0190, all_bits},
    {0x40'01e0, all_bits}, // IIO_IXCC crosstalk credit count timeout
    {0x40'01e8, all_bits}, // IIO_IMEM miscellaneous enable mask
    {0x40'01f0, all_bits}, // IIO_IXTT crosstalk tail timeout
    {0x40'0200, all_bits}, // IIO_IBCN BTE CRB count
    {0x40'0300, all_bits}, // IIO_IPCA PRB counter adjust
    {0x40'0308, all_bits}, // IIO_PRTE_0..7 PIO read address table
    {0x40'0310, all_bits},
    {0x40'0318, all_bits},
    {0x40'0320, all_bits},
    {0x40'0328, all_bits},
    {0x40'0330, all_bits},
    {0x40'0338, all_bits},
    {0x40'0340, all_bits},
    {0x40'0388, all_bits}, // IIO_IPDR PIO table entry deallocation
    {0x40'0390, all_bits}, // IIO_ICDR CRB entry deallocation
    {0x40'0398, all_bits}, // IIO_IFDR IOQ FIFO depth
    {0x40'03a0, all_bits}, // IIO_IIAP IIQ arbitration parameters
    {0x40'03a8, all_bits}, // IIO_ICMR CRB management
    {0x40'03b0, all_bits}, // IIO_ICCR CRB control
    {0x40'03b8, all_bits}, // IIO_ICTO CRB timeout
    {0x40'03c0, all_bits}, // IIO_ICTP CRB timeout prescaler
    {0x43'0000, all_bits}, // IIO_IPCR performance control
    {0x43'0008, all_bits}, // IIO_IPPR performance profiling
    // NI_AGE_* age controls (CPU0/1, GBR, IO; memory and PIO): stored.
    {0x60'0500, all_bits},
    {0x60'0508, all_bits},
    {0x60'0510, all_bits},
    {0x60'0518, all_bits},
    {0x60'0520, all_bits},
    {0x60'0528, all_bits},
    {0x60'0530, all_bits},
    {0x60'0538, all_bits},
    {0x00'0038, all_bits}, // PI_CRB_SFACTOR
    // PI_IO_PROTECT: which regions may send I/O interrupts. hypothesis: no effect on one node.
    {0x00'0010, all_bits},
    // IIO_IOPRB_0, _8.._F: outbound PIO request buffer per widget (iprb_t). The PROM sets
    // fire_and_forget (bit 42) for each (observed); modes and counters have no modeled effect.
    {0x40'0198, all_bits},
    {0x40'01a0, all_bits},
    {0x40'01a8, all_bits},
    {0x40'01b0, all_bits},
    {0x40'01b8, all_bits},
    {0x40'01c0, all_bits},
    {0x40'01c8, all_bits},
    {0x40'01d0, all_bits},
    {0x40'01d8, all_bits},
    // MD page-migration thresholds and outgoing queue size: stored.
    {0x20'0030, all_bits}, // MD_MIG_DIFF_THRESH
    {0x20'0038, all_bits}, // MD_MIG_VALUE_THRESH
    {0x20'00a0, all_bits}, // MD_MOQ_SIZE
    // MD_MEM_DIMM_INIT, MD_DIR_DIMM_INIT: DIMM select 35:32 and SDRAM mode 11:0 (MDI_*).
    // MDIRINIT writes each DIMM's mode (observed); DIMM timing is not modeled.
    {0x20'0090, 0xf'0000'0fff},
    {0x20'0098, 0xf'0000'0fff},
    // hypothesis: stored only; remote-access protection has no effect on one node.
    {ni_protection, all_bits},
    // NACK counters and compare, PIO protection: stored.
    {0x00'04a8, all_bits}, // PI_NACK_CNT_A
    {0x00'04b0, all_bits}, // PI_NACK_CNT_B
    {0x00'04b8, all_bits}, // PI_NACK_CMP
    {0x60'0400, all_bits}, // NI_IO_PROTECT
    {0x60'0408, all_bits}, // NI_IO_PROT_OVRRD
    // Vector PIO parameters, route, and data: stored; a write of the parameters sends the
    // request (Hub::start_vector_pio).
    {0x60'0200, all_bits}, // NI_VECTOR_PARMS
    {0x60'0208, all_bits}, // NI_VECTOR
    {0x60'0210, all_bits}, // NI_VECTOR_DATA
    // LLP and diagnostic parameters: stored; a lone node has no link for them to act on.
    {ni_global_parms, all_bits},
    {ni_diag_parms, all_bits},
    {ni_port_parms, all_bits},
    {ni_scratch_reg0, all_bits},
    {ni_scratch_reg1, all_bits},
};
static_assert(std::size(stored_registers) == Hub::stored_register_count);

// Slot of a storage register, or stored_register_count.
// stored_registers indices sorted by offset, for a binary search on every access. Measured:
// the linear search was about 10% of IO6 PROM run time, which polls PI_RT_COUNT (perf,
// 2026-09-27).
constexpr auto stored_by_offset = [] {
    std::array<std::pair<std::uint64_t, std::size_t>, std::size(stored_registers)> order{};
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = {stored_registers[i].offset, i};
    }
    std::ranges::sort(order);
    return order;
}();

constexpr std::size_t find_stored(std::uint64_t offset) {
    const auto* found = std::ranges::lower_bound(stored_by_offset, offset, {},
                                                 &std::pair<std::uint64_t, std::size_t>::first);
    return found != stored_by_offset.end() && found->first == offset ? found->second
                                                                     : Hub::stored_register_count;
}

// Requests that do not come from a CPU (crosstalk and the BTEs' ITTE lookups) reach only
// registers that do not depend on the requester; they are answered as for slice A.
constexpr unsigned not_a_cpu = 0;

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
// With a cable (SMP.adoc "S5"): link up, documented by the IRIX-derived header and observed
// as the PROM's link test (bit 29).
constexpr std::uint64_t nsri_link_up = std::uint64_t{1} << 29;
// NSRI_MORENODES (18) and NSRI_REGIONSIZE (17) are software settings (observed: the PROM
// writes REGIONSIZE_FINE); inferred writable. hypothesis: so is the node ID, which software
// assigns during discovery (SMP.adoc "S5: the second node"; to be confirmed by the PROM's
// writes with two nodes).
constexpr std::uint64_t nsri_writable =
    (std::uint64_t{1} << 18) | (std::uint64_t{1} << 17) | (std::uint64_t{0x1ff} << 8);
constexpr unsigned nsri_rev_shift = 4;

// Vector PIO (LINUX sn0/hubni.h NVP_*, NVS_*, PIOTYPE_*): NI_VECTOR_PARMS holds the PIO ID
// 49:40, write ID 39:32, the target address 19:3, and the type 1:0; NI_VECTOR_STATUS holds
// VALID 63, OVERRUN 62, TARGET 60:51, the PIO ID, write ID, address 31:3, and the type 2:0.
constexpr std::uint64_t nvp_echo = (std::uint64_t{0x3ff} << 40) | (std::uint64_t{0xff} << 32);
constexpr std::uint64_t nvp_address = 0xf'fff8;
constexpr std::uint64_t nvp_type = 0x3;
constexpr std::uint64_t nvs_valid = std::uint64_t{1} << 63;
constexpr std::uint64_t nvs_overrun = std::uint64_t{1} << 62;
constexpr unsigned piotype_read = 0;
constexpr unsigned piotype_write = 1;
constexpr unsigned piotype_exchange = 3;
constexpr unsigned piotype_addr_err = 4;
constexpr unsigned piotype_cmd_err = 5;

// PI_INT_PEND_MOD (LINUX asm/sn/intr.h, "copied from Irix"): writing 0x100 | level sets
// pending interrupt `level` (0-127), writing `level` clears it. Levels 0-63 are INT_PEND0 bits,
// 64-127 INT_PEND1 bits. inferred from the IRIX-derived header.
constexpr std::uint64_t pend_mod_set = 0x100;
constexpr std::uint64_t pend_mod_level = 0x7f;

// MD_SLOTID_USTAT (LINUX sn0/hubmd.h): FPROMRDY bit 4, I2CINTR bit 3, SN0 slot ID 2:0.
// inferred: the flash is always ready (programming is not modeled); I2CINTR follows the
// PCF8584's interrupt output. The slot ID comes from the machine (IP27.adoc "Machine type").
// hypothesis: CORECLK, NETSYNC, CORECLK_TST 0.
constexpr std::uint64_t msu_fpromrdy = 1u << 4;
constexpr std::uint64_t msu_i2cintr = 1u << 3;

// MD_MEMORY_CONFIG reset value, MMC_RESET_DEFAULTS in the IRIX-derived sn0/hubmd.h: FPROM
// cycle 0xf, FPROM write 0x7, UCTLR cycle 0x1f, UCTLR write 0xf, IGNORE_ECC, DIR_PREMIUM,
// REPLY_GUAR 0xf, and every bank size field at 7 (512 MB). "Bits not used by the MD are used
// by software", so every bit is stored as written.
constexpr std::uint64_t mmc_dir_premium = std::uint64_t{1} << 28;
constexpr std::uint64_t mmc_reset_defaults =
    (std::uint64_t{0x0f} << 49) | (std::uint64_t{0x07} << 44) | (std::uint64_t{0x1f} << 39) |
    (std::uint64_t{0x0f} << 34) | (std::uint64_t{1} << 29) | (std::uint64_t{1} << 28) |
    (std::uint64_t{0x0f} << 24) | 0xff'ffff;

// IIO_WID: the Hub's own XIO widget identification (hubii_wid_t): revision 31:28, part
// number 27:12 = HUB_WIDGET_PART_NUM 0xc101, manufacturer 11:1 = WIDGET_HUB_MFGR_NUM 0x036
// (LINUX sn0/hubio.h, asm/xtalk/xwidget.h). hypothesis: the widget revision equals the Hub
// revision.
constexpr std::uint64_t hub_widget_part = 0xc101;
constexpr std::uint64_t hub_widget_mfgr = 0x036;
// MD_REFRESH_CONTROL reset value, MRC_RESET_DEFAULTS (IRIX-derived hubmd.h).
constexpr std::uint64_t mrc_reset_defaults = 0x400;

// MD_MLAN_CTL, the MicroLAN (1-Wire) master for the node's NIC serial-number chip (LINUX
// sn0/hubmd.h MLAN_*): PHI1 33:27 and PHI0 26:20 timing, PULSE 19:10 low-pulse length,
// SAMPLE 9:2 sample time, DONE 1, RD_DATA 0 (the sampled line).
constexpr std::uint64_t mlan_control = 0x3'ffff'fffc;
constexpr std::uint64_t mlan_done = 0x2;
constexpr std::uint64_t mlan_rd_data = 0x1;
constexpr std::uint64_t mlan_reset_defaults =
    (std::uint64_t{0x31} << 27) | (std::uint64_t{0x31} << 20);

// Block-transfer engines (LINUX sn0/hubio.h IIO_IBLS_0.. at IIO_BASE_BTE0/1; IBLS_*, IBCT_*,
// IBIA_*). Registers, as offsets from each engine's base: length/status, source, destination,
// control/terminate, notification address, interrupt address.
constexpr std::uint64_t bte_base[2] = {0x41'0000, 0x42'0000};
constexpr std::uint64_t bte_registers = 6;
constexpr std::uint64_t ibls = 0;
constexpr std::uint64_t ibsa = 1;
constexpr std::uint64_t ibda = 2;
constexpr std::uint64_t ibct = 3;
constexpr std::uint64_t ibna = 4;
constexpr std::uint64_t ibia = 5;
constexpr std::uint64_t ibls_busy = 1u << 20;
constexpr std::uint64_t ibls_error = 1u << 16;
constexpr std::uint64_t ibls_length = 0xffff;
constexpr std::uint64_t ibct_notify = 1u << 4;
constexpr std::uint64_t ibct_zero_fill = 1u << 0;
// inferred: lengths count 128-byte cache lines (the PROM splits a region between the two
// engines at 0x1000 lines = 512 KB; the Linux SN1 BTE driver shifts by the cache line size).
constexpr std::uint64_t bte_line = 128;
constexpr std::uint64_t physical_mask = (std::uint64_t{1} << 40) - 1;

// The engine and register at a Hub offset, if the offset is a BTE register.
constexpr std::optional<std::pair<unsigned, std::uint64_t>> bte_register(std::uint64_t offset) {
    for (unsigned engine = 0; engine < 2; ++engine) {
        if (offset >= bte_base[engine] && offset < bte_base[engine] + bte_registers * 8 &&
            offset % 8 == 0) {
            return std::pair{engine, (offset - bte_base[engine]) / 8};
        }
    }
    return std::nullopt;
}

// NI_PORT_RESET bits (LINUX): send warm reset on the link; reset the entire Hub.
constexpr std::uint64_t npr_portreset = 1u << 7;
constexpr std::uint64_t npr_localreset = 1u << 0;

// inferred: PI_RT_COUNT is 52 bits wide (LINUX ip27-timer.c registers a 52-bit sched_clock).
constexpr std::uint64_t rt_count_mask = (std::uint64_t{1} << 52) - 1;

} // namespace

Hub::Hub(Scheduler& scheduler, Tracer& tracer, unsigned revision, unsigned slot_id,
         unsigned cpu_count, devices::Pcf8584& i2c, DirectoryMemory& directory,
         AddressSpace& memory, std::function<void()> reset_node, std::string_view event_prefix)
    : scheduler_{scheduler}, tracer_{tracer}, revision_{revision}, slot_id_{slot_id & 7}, i2c_{i2c},
      directory_{directory}, memory_{memory}, reset_node_{std::move(reset_node)},
      cpu_present_{1, cpu_count > 1 ? 1u : 0u},
      local_reset_event_{scheduler.add_event(std::string{event_prefix} + "hub.local_reset",
                                             [this] { local_reset(); })},
      comparator_events_{scheduler.add_event(std::string{event_prefix} + "hub.rt_compare_a",
                                             [this] { comparator_matched(0); }),
                         scheduler.add_event(std::string{event_prefix} + "hub.rt_compare_b",
                                             [this] { comparator_matched(1); }),
                         scheduler.add_event(std::string{event_prefix} + "hub.profile_compare",
                                             [this] { comparator_matched(2); })} {
    for (unsigned w = 0; w < xtalk::widget_count; ++w) {
        io_windows_[w].hub = this;
        io_windows_[w].widget = w;
    }
    for (unsigned i = 0; i < big_windows_.size(); ++i) {
        big_windows_[i].hub = this;
        big_windows_[i].index = i + 1;
    }
    widget_.hub = this;
    for (unsigned slice = 0; slice < ports_.size(); ++slice) {
        ports_[slice].hub = this;
        ports_[slice].slice = slice;
    }
    reset(ResetKind::power_on);
    // Nothing survives power-on.
    err_stack_addr_ = {};
    err_status0_ = {};
}

std::uint64_t Hub::rt_count() const {
    return (rt_base_.load(std::memory_order_relaxed) + cycles_at(scheduler_.now(), rtc_clock) -
            rt_epoch_ticks_.load(std::memory_order_relaxed)) &
           rt_count_mask;
}

std::uint64_t Hub::rt_count_at(VirtualTime time) const {
    std::uint64_t sequence = 0;
    std::uint64_t base = 0;
    std::uint64_t epoch = 0;
    do {
        sequence = rt_sequence_.load(std::memory_order_acquire);
        base = rt_base_.load(std::memory_order_relaxed);
        epoch = rt_epoch_ticks_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
    } while ((sequence & 1) != 0 || rt_sequence_.load(std::memory_order_relaxed) != sequence);
    // A reader behind the last write sees the written value until its time passes it.
    const std::uint64_t ticks = std::max(cycles_at(time, rtc_clock), epoch);
    return (base + ticks - epoch) & rt_count_mask;
}

Hub::RtWindow Hub::rt_window(VirtualTime time) const {
    const auto [base, epoch] = rt_origin();
    const std::uint64_t ticks = std::max(cycles_at(time, rtc_clock), epoch);
    return {.count = (base + ticks - epoch) & rt_count_mask,
            .until = time_at_cycles(ticks + 1, rtc_clock)};
}

void Hub::set_rt_count(std::uint64_t value) {
    const std::uint64_t sequence = rt_sequence_.load(std::memory_order_relaxed);
    rt_sequence_.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    rt_base_.store(value & rt_count_mask, std::memory_order_relaxed);
    // At the writer's own time on host threads (SMP.adoc "S4"), as its reads are: at device
    // time, a count the global master copies to another node started up to a window behind
    // its own, and the PROM disabled that node's CPUs ("Failed RTC distribution").
    rt_epoch_ticks_.store(cycles_at(reader_time_.value_or(scheduler_.now()), rtc_clock),
                          std::memory_order_relaxed);
    rt_sequence_.store(sequence + 2, std::memory_order_release);
    schedule_comparators();
}

// PI_RT_COMPARE_A/B and PI_PROFILE_COMPARE (LINUX sn0/hubpi.h): when PI_RT_COUNT reaches a
// compare value, the comparator latches its pending bit (RT: that CPU's; profile: both), which
// stays set until software writes the pending register. The CPU sees pending and enabled RT
// as HUB_IP_RT (IP4) and profile as HUB_IP_PROF (IP5). inferred: a compare value already
// passed does not fire until the counter comes round again (observed: IRIX writes compare
// values ahead of the counter).
//
// On host threads (SMP.adoc "S4") the writer compares against the count at its own time,
// which can be behind device time: a value it has not reached yet is ahead of the counter as
// the writer saw it, and is due at once if device time has passed it. Measured: compared at
// device time, IRIX's short timeouts were lost and a parallel `reboot2` idled forever.
void Hub::schedule_comparators() {
    const VirtualTime now = reader_time_.value_or(scheduler_.now());
    const std::uint64_t now_ticks = cycles_at(now, rtc_clock);
    const std::uint64_t count = reader_time_ ? rt_count_at(*reader_time_) : rt_count();
    const auto stored_at = [this](std::uint64_t offset) {
        const std::size_t slot = find_stored(offset);
        invariant(slot < stored_register_count, "comparators are stored registers");
        return stored_[slot];
    };
    const std::array<std::uint64_t, 3> compares = {
        stored_at(pi_rt_compare_a), stored_at(pi_rt_compare_b), stored_at(pi_profile_compare)};
    static_assert(compares.size() == std::tuple_size_v<decltype(comparator_events_)>);
    for (std::size_t i = 0; i < compares.size(); ++i) {
        const std::uint64_t compare = compares[i] & rt_count_mask;
        if (compare <= count) {
            scheduler_.cancel(comparator_events_[i]);
            continue;
        }
        scheduler_.schedule_at(
            comparator_events_[i],
            std::max(scheduler_.now(), time_at_cycles(now_ticks + (compare - count), rtc_clock)));
        tracer_.log(TraceCategory::irq, "{}hub comparator {} at count {:#x} (now {:#x})", trace_name_, i, compare,
                    count);
    }
}

// PI_ERR_STATUS0 (LINUX sn0/hubpi.h): VALID (63), ADDR (61:25, address bits 39:3), and the
// error type (2:0), here PI_ERR_RD_PRERR or PI_ERR_WR_PWERR (a PIO read or write error).
// hypothesis: CMD, SUPPL, and the T5 request number read zero. Not modeled: OVERRUN and the
// error stack in memory a second error spools to; the model keeps the first error (observed:
// with OVERRUN set and no stack, IRIX panics "Invalid RRB in error stack"). observed: the PROM clears ERR_STATUS0 and reads an absent
// widget's small window (`0x9200000002400208`, widget 2), and IRIX deems the CPU's firmware
// current only if ERR_STATUS0 records exactly that address ("Some CPUs have old firmware"
// otherwise; slate's IRIX does not print it).
void Hub::record_pio_error(unsigned slice, std::uint64_t address, bool write) {
    constexpr std::uint64_t valid = std::uint64_t{1} << 63;
    constexpr std::uint64_t address_field = (std::uint64_t{1} << 37) - 1;
    constexpr std::uint64_t pio_read_error = 1;  // PI_ERR_RD_PRERR
    constexpr std::uint64_t pio_write_error = 1; // PI_ERR_WR_PWERR
    std::uint64_t& status = err_status0_[slice];
    if ((status & valid) != 0) {
        return;
    }
    status = valid | ((address >> 3) & address_field) << 25 |
             (write ? pio_write_error : pio_read_error);
    tracer_.log(TraceCategory::hub, "{}PIO error at {:#x}: ERR_STATUS0_{} {:#x}", trace_name_,
                address, slice == 0 ? 'A' : 'B', status);
}

void Hub::comparator_matched(unsigned comparator) {
    tracer_.log(TraceCategory::irq, "{}hub comparator {} matched", trace_name_, comparator);
    if (comparator < 2) {
        rt_pending_[comparator] = true;
    } else {
        prof_pending_ = {true, true};
    }
    update_interrupts();
}

void Hub::set_led(unsigned slice, std::uint64_t value) {
    // Eight-bit LEDs, one bank per CPU slice (LINUX MD_LED0/MD_LED1). The PROM reports its
    // boot phase here (PRM 3.1, Table 3-1).
    leds_[slice] = value & 0xff;
    tracer_.log(TraceCategory::firmware, "{}led {} {:#04x}", trace_name_, slice == 0 ? 'A' : 'B', leds_[slice]);
}

void Hub::set_memory_config(std::uint64_t value) {
    // DIR_PREMIUM selects the directory entry format the MD uses. hypothesis: the bank
    // sizes do not yet change how memory decodes (IP27.adoc "Memory and directory").
    memory_config_ = value;
    directory_.set_premium_mode((value & mmc_dir_premium) != 0);
}

void Hub::raise_interrupt(unsigned level) {
    const std::size_t word = level / 64 % 2;
    set_pending(word, interrupt_pending(word) | std::uint64_t{1} << (level % 64));
    update_interrupts();
}

std::uint64_t Hub::read_bte(unsigned engine, std::uint64_t offset) const {
    return bte_[engine][offset];
}

void Hub::write_bte(unsigned engine, std::uint64_t offset, std::uint64_t value) {
    bte_[engine][offset] = value;
    // Writing the control register starts a transfer that software armed by setting BUSY
    // in the length/status register (observed sequence; LINUX SN1 bte.c does the same).
    // hypothesis: without BUSY it starts nothing (INITII zeroes every register).
    if (offset == ibct && (bte_[engine][ibls] & ibls_busy) != 0) {
        run_bte(engine);
    }
}

void Hub::run_bte(unsigned engine) {
    auto& r = bte_[engine];
    const std::uint64_t bytes = (r[ibls] & ibls_length) * bte_line;
    // hypothesis: the engines use physical address bits 39:0; the PROM writes cached
    // xkphys addresses. The transfer completes before the next access (its duration is not
    // modeled).
    const std::uint64_t source = r[ibsa] & physical_mask;
    const std::uint64_t destination = r[ibda] & physical_mask;
    const bool zero_fill = (r[ibct] & ibct_zero_fill) != 0;
    tracer_.log(TraceCategory::hub, "{}bte {} {} {:#x} bytes {:#x} -> {:#x}", trace_name_, engine,
                zero_fill ? "fill" : "copy", bytes, source, destination);
    std::uint64_t status = 0;
    for (std::uint64_t offset = 0; offset < bytes; offset += 8) {
        std::uint64_t value = 0;
        if (!zero_fill) {
            const auto read = memory_.read(PhysicalAddress{source + offset}, AccessWidth::bits64);
            if (!read) {
                status = ibls_error;
                break;
            }
            value = *read;
        }
        if (!memory_.write(PhysicalAddress{destination + offset}, AccessWidth::bits64, value)) {
            status = ibls_error;
            break;
        }
    }
    // Done: BUSY clear, the remaining length 0 (hypothesis on an error as well).
    r[ibls] = status;
    if ((r[ibct] & ibct_notify) != 0 &&
        !memory_.write(PhysicalAddress{r[ibna] & physical_mask}, AccessWidth::bits64, r[ibls])) {
        tracer_.log(TraceCategory::hub, "{}bte {} notification to {:#x} failed", trace_name_, engine, r[ibna]);
    }
    // Completion raises the interrupt level in IBIA at the node it names (observed: the
    // PROM waits for these levels in PI_INT_PEND0). This machine has one node, NASID 0.
    const std::uint64_t node = r[ibia] & 0x1ff;
    if (node == 0) {
        raise_interrupt(static_cast<unsigned>(r[ibia] >> 16 & 0x7f));
    }
}

void Hub::local_reset() {
    // hypothesis: a Hub local reset resets the node's CPUs with a cold reset and reinitializes
    // the Hub's modeled registers, except the error stack addresses, which the PROM relies on
    // to survive it (IP27.adoc "Hub local reset").
    tracer_.log(TraceCategory::hub, "{}local reset", trace_name_);
    reset(ResetKind::cold);
    reset_node_();
}

// IIO_ITTE (the IRIX-derived Linux 2.4 asm-ia64/sn/sn1/hubio.h ii_itte_u, the Bedrock's
// successor of the Hub's; hypothesis: the same fields on the Hub): crosstalk offset bits 4:0 in
// 512 MB units, widget number bits 11:8. observed: IRIX parks unused windows at widget 3 (ITTE
// 0x300) and keeps window 7 at widget 0 (sn0/hubio.h SWIN0_BIGWIN: the top big window stands
// in for small window 0). hypothesis: an ITTE resets to 0 (widget 0, offset 0).
std::pair<unsigned, std::uint64_t> Hub::BigWindow::translate(std::uint64_t offset) const {
    constexpr std::uint64_t first_itte = 0x40'0160;
    constexpr unsigned window_shift = 29;
    const std::uint64_t itte =
        hub->read(not_a_cpu, first_itte + std::uint64_t{8} * (index - 1), AccessWidth::bits64)
            .value_or(0);
    const auto widget = static_cast<unsigned>((itte >> 8) & 0xf);
    return {widget, ((itte & 0x1f) << window_shift) + offset};
}

std::expected<std::uint64_t, AccessFault> Hub::BigWindow::mmio_read(std::uint64_t offset,
                                                                    AccessWidth width) {
    if (hub->xtalk_ == nullptr) {
        return std::unexpected(AccessFault::unmapped);
    }
    const auto [widget, address] = translate(offset);
    hub->tracer_.log(TraceCategory::xtalk, "{}big window {} read {:#x} -> widget {} {:#x}", hub->trace_name_, index,
                     offset, widget, address);
    return hub->xtalk_->xtalk_read(widget, address, width);
}

std::expected<void, AccessFault> Hub::BigWindow::mmio_write(std::uint64_t offset, AccessWidth width,
                                                            std::uint64_t value) {
    if (hub->xtalk_ == nullptr) {
        return std::unexpected(AccessFault::unmapped);
    }
    const auto [widget, address] = translate(offset);
    hub->tracer_.log(TraceCategory::xtalk, "{}big window {} write {:#x} -> widget {} {:#x}", hub->trace_name_, index,
                     offset, widget, address);
    return hub->xtalk_->xtalk_write(widget, address, width, value);
}

std::expected<std::uint64_t, AccessFault> Hub::IoWindow::mmio_read(std::uint64_t offset,
                                                                   AccessWidth width) {
    if (hub->xtalk_ == nullptr) {
        return std::unexpected(AccessFault::unmapped);
    }
    return hub->xtalk_->xtalk_read(widget, offset, width);
}

std::expected<void, AccessFault> Hub::IoWindow::mmio_write(std::uint64_t offset, AccessWidth width,
                                                           std::uint64_t value) {
    if (hub->xtalk_ == nullptr) {
        return std::unexpected(AccessFault::unmapped);
    }
    return hub->xtalk_->xtalk_write(widget, offset, width, value);
}

// inferred: the II registers are laid out as a standard crosstalk widget (IIO_WID 0x400000
// holds WIDGET_ID, IIO_WSTAT 0x400008 WIDGET_STATUS, IIO_WCR 0x400020 WIDGET_CONTROL), so the
// Hub's widget space starts at II offset 0. 32-bit registers sit in the low half of each
// doubleword.
// inferred: the whole local register space (PI, MD, II, NI) answers at crosstalk offset
// 0x1000000 + its local offset. observed: the PROM's xbow_update points a big window at the
// peer Hub's Crossbow port and reads its node ID (bits 16:8) at 0x1600000, NI_STATUS_REV_ID.
constexpr std::uint64_t xtalk_local_base = 0x100'0000;

std::expected<std::uint64_t, AccessFault> Hub::Widget::mmio_read(std::uint64_t offset,
                                                                 AccessWidth width) {
    constexpr std::uint64_t ii_base = 0x40'0000;
    if (offset >= xtalk_local_base && offset < xtalk_local_base + window_size &&
        width == AccessWidth::bits64 && offset % 8 == 0) {
        return hub->read(not_a_cpu, offset - xtalk_local_base, width);
    }
    if (width == AccessWidth::bits64 && offset % 8 == 0) {
        return hub->read(not_a_cpu, ii_base + offset, width);
    }
    if (width == AccessWidth::bits32 && offset % 8 == 4) {
        const auto value = hub->read(not_a_cpu, ii_base + offset - 4, AccessWidth::bits64);
        if (!value) {
            return value;
        }
        return *value & 0xffff'ffff;
    }
    return std::unexpected(AccessFault::unsupported);
}

std::expected<void, AccessFault> Hub::Widget::mmio_write(std::uint64_t offset, AccessWidth width,
                                                         std::uint64_t value) {
    constexpr std::uint64_t ii_base = 0x40'0000;
    if (offset >= xtalk_local_base && offset < xtalk_local_base + window_size &&
        width == AccessWidth::bits64 && offset % 8 == 0) {
        return hub->write(offset - xtalk_local_base, width, value);
    }
    if (width == AccessWidth::bits64 && offset % 8 == 0) {
        return hub->write(ii_base + offset, width, value);
    }
    if (width == AccessWidth::bits32 && offset % 8 == 4) {
        const auto old = hub->read(not_a_cpu, ii_base + offset - 4, AccessWidth::bits64);
        if (!old) {
            return std::unexpected(old.error());
        }
        return hub->write(ii_base + offset - 4, AccessWidth::bits64,
                               (*old & ~std::uint64_t{0xffff'ffff}) | (value & 0xffff'ffff));
    }
    return std::unexpected(AccessFault::unsupported);
}

// Incoming crosstalk DMA reaches node memory. hypothesis: crosstalk address bits 39:0 are the
// node's physical address (this machine has one node, NASID 0), and the transfer is coherent
// and completes at once.
bool Hub::Widget::xtalk_dma_read(std::uint64_t address, std::span<std::byte> bytes) {
    const auto memory =
        hub->memory_.memory_bytes(PhysicalAddress{address & physical_mask}, bytes.size());
    if (memory.size() != bytes.size()) {
        hub->tracer_.log(TraceCategory::hub, "{}DMA read outside memory {:#x} size {:#x}", hub->trace_name_, address,
                         bytes.size());
        return false;
    }
    std::ranges::copy(memory, bytes.begin());
    return true;
}

bool Hub::Widget::xtalk_dma_write(std::uint64_t address, std::span<const std::byte> bytes) {
    const auto memory =
        hub->memory_.writable_memory_bytes(PhysicalAddress{address & physical_mask}, bytes.size());
    if (memory.size() != bytes.size()) {
        hub->tracer_.log(TraceCategory::hub, "{}DMA write outside memory {:#x} size {:#x}", hub->trace_name_, address,
                         bytes.size());
        return false;
    }
    std::ranges::copy(bytes, memory.begin());
    return true;
}

void Hub::Widget::xtalk_interrupt(std::uint64_t address, std::uint8_t vector) {
    // observed (xbow_sanity): an interrupt aimed at PI_INT_PEND_MOD through the Hub's IO
    // window (0x01800090: widget 1, remote-Hub alias, offset 0x90) sets the level given by
    // its vector in PI_INT_PEND. hypothesis: the NASID and alias bits are not checked.
    constexpr std::uint64_t widget_mask = 0xf00'0000;
    constexpr std::uint64_t register_mask = 0x7f'ffff;
    if ((address & widget_mask) == (std::uint64_t{1} << 24) &&
        (address & register_mask) == pi_int_pend_mod) {
        hub->raise_interrupt(vector);
        return;
    }
    hub->tracer_.log(TraceCategory::hub, "{}unmodeled crosstalk interrupt to {:#x} vector {:#x}", hub->trace_name_,
                     address, vector);
}

void Hub::Widget::xtalk_interrupt_clear(std::uint64_t address, std::uint8_t vector) {
    // hypothesis: a clear packet to PI_INT_PEND_MOD clears the level its vector names.
    constexpr std::uint64_t widget_mask = 0xf00'0000;
    constexpr std::uint64_t register_mask = 0x7f'ffff;
    if ((address & widget_mask) == (std::uint64_t{1} << 24) &&
        (address & register_mask) == pi_int_pend_mod) {
        const std::size_t word = vector / 64;
        hub->set_pending(word, hub->interrupt_pending(word) & ~(std::uint64_t{1} << (vector % 64)));
        hub->update_interrupts();
    }
}

void Hub::connect_cpu(unsigned slice, InterruptSink& cpu) {
    // Inputs 0-3 are Cause IP2-IP5: HUB_IP_PEND0, HUB_IP_PEND1_CC, HUB_IP_RT, HUB_IP_PROF.
    for (std::uint32_t input = 0; input < cpu_lines_[slice].size(); ++input) {
        cpu_lines_[slice][input].connect(cpu, input);
    }
}

void Hub::update_interrupts() {
    for (std::size_t slice = 0; slice < 2; ++slice) {
        for (std::size_t word = 0; word < 2; ++word) {
            cpu_lines_[slice][word].set_level(
                (interrupt_pending(word) & interrupt_mask_[slice][word]) != 0);
        }
        cpu_lines_[slice][2].set_level(rt_pending_[slice] && rt_enable_[slice]);
        cpu_lines_[slice][3].set_level(prof_pending_[slice] && prof_enable_[slice]);
    }
}

void Hub::save_state(StateImage& image) const {
    for (std::size_t i = 0; i < stored_register_count; ++i) {
        image.put(std::format("hub.register.{:#08x}", stored_registers[i].offset), stored_[i]);
    }
    image.put("hub.cpu_enable", cpu_enable_);
    image.put("hub.err_stack_addr", err_stack_addr_);
    image.put("hub.err_status0", err_status0_);
    image.put("hub.leds", leds_);
    image.put("hub.ilcsr", ilcsr_);
    image.put("hub.memory_config", memory_config_);
    image.put("hub.refresh_control", refresh_control_);
    image.put("hub.mlan_ctl", mlan_ctl_);
    image.put("hub.bte", bte_);
    image.put("hub.nsri_settings", nsri_settings_);
    image.put("hub.mlan_line", std::uint8_t{mlan_line_});
    image.put("hub.routing_tables", routing_tables_);
    image.put("hub.vector", std::array{vector_status_, return_vector_, vector_read_data_});
    image.put("hub.pending", std::array{interrupt_pending(0), interrupt_pending(1)});
    image.put("hub.interrupt_mask", interrupt_mask_);
    image.put("hub.rt_base", rt_base_.load(std::memory_order_relaxed));
    image.put("hub.rt_epoch_ticks", rt_epoch_ticks_.load(std::memory_order_relaxed));
    const std::uint8_t comparators[] = {rt_pending_[0],   rt_pending_[1], prof_pending_[0],
                                        prof_pending_[1], rt_enable_[0],  rt_enable_[1],
                                        prof_enable_[0],  prof_enable_[1]};
    image.put("hub.comparators", comparators);
}

void Hub::load_state(const StateImage& image) {
    for (std::size_t i = 0; i < stored_register_count; ++i) {
        image.get(std::format("hub.register.{:#08x}", stored_registers[i].offset), stored_[i]);
    }
    image.get("hub.cpu_enable", cpu_enable_);
    image.get("hub.err_stack_addr", err_stack_addr_);
    image.get("hub.err_status0", err_status0_);
    image.get("hub.leds", leds_);
    image.get("hub.ilcsr", ilcsr_);
    image.get("hub.memory_config", memory_config_);
    set_memory_config(memory_config_);
    image.get("hub.refresh_control", refresh_control_);
    image.get("hub.mlan_ctl", mlan_ctl_);
    image.get("hub.bte", bte_);
    image.get("hub.nsri_settings", nsri_settings_);
    std::uint8_t mlan_line = 1;
    image.get("hub.mlan_line", mlan_line);
    mlan_line_ = mlan_line != 0;
    image.get("hub.routing_tables", routing_tables_);
    std::array<std::uint64_t, 3> vector{};
    image.get("hub.vector", vector);
    vector_status_ = vector[0];
    return_vector_ = vector[1];
    vector_read_data_ = vector[2];
    std::array<std::uint64_t, 2> pending{};
    image.get("hub.pending", pending);
    set_pending(0, pending[0]);
    set_pending(1, pending[1]);
    image.get("hub.interrupt_mask", interrupt_mask_);
    std::uint64_t rt_base = 0;
    std::uint64_t rt_epoch_ticks = 0;
    image.get("hub.rt_base", rt_base);
    image.get("hub.rt_epoch_ticks", rt_epoch_ticks);
    rt_base_.store(rt_base, std::memory_order_relaxed);
    rt_epoch_ticks_.store(rt_epoch_ticks, std::memory_order_relaxed);
    rt_sequence_.fetch_add(2, std::memory_order_relaxed);
    std::uint8_t comparators[8]{};
    if (image.get("hub.comparators", comparators)) {
        rt_pending_ = {comparators[0] != 0, comparators[1] != 0};
        prof_pending_ = {comparators[2] != 0, comparators[3] != 0};
        rt_enable_ = {comparators[4] != 0, comparators[5] != 0};
        prof_enable_ = {comparators[6] != 0, comparators[7] != 0};
    }
    // Snapshots from before the comparators were modeled carry no events for them.
    schedule_comparators();
    update_interrupts();
}

void Hub::set_nsri_settings(std::uint64_t value) {
    const unsigned old_id = node_id();
    nsri_settings_ = value;
    if (node_id() != old_id) {
        tracer_.log(TraceCategory::hub, "{}node id {} -> {}", trace_name_, old_id, node_id());
        if (node_id_changed_) {
            node_id_changed_();
        }
    }
}

void Hub::start_vector_pio() {
    const std::uint64_t parms = stored_[find_stored(ni_vector_parms)];
    const std::uint64_t route = stored_[find_stored(ni_vector)];
    const std::uint64_t data = stored_[find_stored(ni_vector_data)];
    const unsigned type = parms & nvp_type;
    const std::uint64_t address = parms & nvp_address;
    tracer_.log(TraceCategory::hub, "{}vector pio type {} route {:#x} address {:#x} data {:#x}",
                trace_name_, type, route, address, data);
    if (neighbor_ == nullptr) {
        // No cable: the request goes nowhere and no response arrives.
        return;
    }
    // hypothesis: with no router, every route ends at the neighbor Hub, and the response
    // arrives before the CPU's next access (link latency is not modeled). TARGET and the
    // return route are 0.
    unsigned result = type;
    std::uint64_t read = 0;
    if (type == piotype_read || type == piotype_write || type == piotype_exchange) {
        if (const auto value = neighbor_->serve_vector_pio(type, address, data)) {
            read = *value;
        } else {
            result = piotype_addr_err;
        }
    } else {
        result = piotype_cmd_err;
    }
    const bool overrun = (vector_status_ & nvs_valid) != 0;
    vector_status_ = nvs_valid | (overrun ? nvs_overrun : 0) | (parms & nvp_echo) | address |
                     result;
    return_vector_ = 0;
    if (result == piotype_read || result == piotype_exchange) {
        vector_read_data_ = read;
    }
    tracer_.log(TraceCategory::hub, "{}vector pio status {:#x} read {:#x}", trace_name_,
                vector_status_, vector_read_data_);
}

std::optional<std::uint64_t> Hub::serve_vector_pio(unsigned type, std::uint64_t address,
                                                   std::uint64_t data) {
    // The address is an offset in this Hub's NI space (observed: the PROM's discovery reads the
    // neighbor's NI_STATUS_REV_ID at 0 and assigns its node ID at 0x108, NI_SCRATCH_REG1).
    const std::uint64_t offset = ni_base | address;
    std::uint64_t value = 0;
    if (type != piotype_write) {
        const auto read_value = read(0, offset, AccessWidth::bits64);
        if (!read_value) {
            return std::nullopt;
        }
        value = *read_value;
    }
    if (type != piotype_read && !write(offset, AccessWidth::bits64, data)) {
        return std::nullopt;
    }
    return value;
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
    rt_pending_ = {};
    prof_pending_ = {};
    rt_enable_ = {};
    prof_enable_ = {};
    for (const EventId event : comparator_events_) {
        scheduler_.cancel(event);
    }
    set_memory_config(mmc_reset_defaults);
    refresh_control_ = mrc_reset_defaults;
    mlan_ctl_ = mlan_reset_defaults;
    mlan_line_ = true;
    routing_tables_ = {};
    vector_status_ = 0;
    return_vector_ = 0;
    vector_read_data_ = 0;
    bte_ = {};
    set_nsri_settings(0);
    set_pending(0, 0);
    set_pending(1, 0);
    interrupt_mask_ = {};
    update_interrupts();
    cpu_enable_ = cpu_present_;
    set_rt_count(0);
}

std::expected<std::uint64_t, AccessFault> Hub::Port::mmio_read(std::uint64_t offset,
                                                               AccessWidth width) {
    return hub->read(slice, offset, width);
}

std::expected<void, AccessFault> Hub::Port::mmio_write(std::uint64_t offset, AccessWidth width,
                                                       std::uint64_t value) {
    // inferred: no register written by the PROM or IRIX depends on the writer's slice; each
    // CPU's registers have their own addresses (_A and _B).
    return hub->write(offset, width, value);
}

std::expected<std::uint64_t, AccessFault> Hub::read(unsigned slice, std::uint64_t offset,
                                                    AccessWidth width) {
    if (width == AccessWidth::bits64) {
        if (const std::size_t slot = find_stored(offset); slot < stored_register_count) {
            return stored_[slot];
        }
        if (const auto bte = bte_register(offset)) {
            return read_bte(bte->first, bte->second);
        }
        switch (offset) {
        case pi_cpu_num:
            // observed: each CPU reads it at the reset vector to find its slice (IP27.adoc "Two
            // CPUs on a Hub").
            return slice;
        case pi_cpu_present_a:
            return cpu_present_[0];
        case pi_cpu_present_b:
            return cpu_present_[1];
        case pi_int_pend0:
            return interrupt_pending(0);
        case pi_int_pend1:
            return interrupt_pending(1);
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
        case pi_err_int_pend:
            // No modeled hardware reports PI errors (parity, SysAD, spool): nothing pending.
            return 0;
        case pi_err_stack_addr_a:
            return err_stack_addr_[0];
        case pi_err_stack_addr_b:
            return err_stack_addr_[1];
        case pi_rt_count:
            return reader_time_ ? rt_count_at(*reader_time_) : rt_count();
        case pi_rt_pend_a:
        case pi_rt_pend_b:
            return rt_pending_[offset == pi_rt_pend_b] ? 1 : 0;
        case pi_prof_pend_a:
        case pi_prof_pend_b:
            return prof_pending_[offset == pi_prof_pend_b] ? 1 : 0;
        case pi_rt_en_a:
        case pi_rt_en_b:
            return rt_enable_[offset == pi_rt_en_b] ? 1 : 0;
        case pi_prof_en_a:
        case pi_prof_en_b:
            return prof_enable_[offset == pi_prof_en_b] ? 1 : 0;
        case md_ureg0_0:
        case md_ureg0_1:
            // hypothesis: the byte-wide junk bus reads zero in bits 63:8.
            return i2c_.read(offset == md_ureg0_1);
        case md_slotid_ustat:
            return msu_fpromrdy | (i2c_.interrupt_asserted() ? msu_i2cintr : 0) | slot_id_;
        case md_memory_config:
            return memory_config_;
        case md_refresh_control:
            return refresh_control_;
        case md_mlan_ctl:
            // hypothesis: each operation completes before the next access (its duration is
            // not modeled). With nothing attached the pulled-up line samples 1.
            return (mlan_ctl_ & mlan_control) | mlan_done | (mlan_line_ ? mlan_rd_data : 0);
        case iio_wid:
            return (std::uint64_t{revision_ & 0xf} << 28) | (hub_widget_part << 12) |
                   (hub_widget_mfgr << 1);
        case md_led0:
            // hypothesis: the LED latches read back their last value.
            return leds_[0];
        case md_led1:
            return leds_[1];
        case iio_ilcsr:
            // The target's Hub is cabled to a Crossbow, so the link reports working.
            return (ilcsr_ & ilcsr_writable) | ilcsr_link_working;
        case ni_port_error:
        case ni_port_error_clear:
            // No CrayLink cable, so no LLP errors (NI_STATUS_REV_ID reports the link down).
            return 0;
        case ni_status_rev_id:
            return (neighbor_ != nullptr ? nsri_link_up : nsri_down_never_reset) |
                   nsri_settings_ | (std::uint64_t{revision_ & 0xf} << nsri_rev_shift);
        case ni_vector_status:
            return vector_status_;
        case ni_return_vector:
            return return_vector_;
        case ni_vector_read_data:
            return vector_read_data_;
        case ni_vector_clear: {
            // Reads the status and clears it (LINUX: "Vector PIO read & clear status").
            const std::uint64_t status = vector_status_;
            vector_status_ = 0;
            return status;
        }
        default:
            if (offset >= pi_err_status_first && offset <= pi_err_status_last && offset % 8 == 0) {
                // ERR_STATUS0_A/B hold what record_pio_write_error recorded, and their
                // clear-on-read forms clear it; ERR_STATUS1: nothing recorded.
                const std::uint64_t index = (offset - pi_err_status_first) / 8;
                if (index % 4 >= 2) {
                    return 0;
                }
                const std::uint64_t status = err_status0_[index / 4];
                if (index % 2 != 0) {
                    err_status0_[index / 4] = 0;
                }
                return status;
            }
            if (offset >= ni_routing_first && offset <= ni_routing_last && offset % 8 == 0) {
                return routing_tables_[(offset - ni_routing_first) / 8];
            }
            if (find_idle(offset) != nullptr) {
                return 0;
            }
            break;
        }
    }
    tracer_.log(TraceCategory::hub, "{}unmodeled read {:#08x} width {}", trace_name_, offset, byte_count(width));
    return std::unexpected(AccessFault::unsupported);
}

std::expected<void, AccessFault> Hub::write(std::uint64_t offset, AccessWidth width,
                                            std::uint64_t value) {
    if (width == AccessWidth::bits64) {
        if (const std::size_t slot = find_stored(offset); slot < stored_register_count) {
            stored_[slot] = value & stored_registers[slot].mask;
            if (offset == iio_wcr) {
                tracer_.log(TraceCategory::hub, "{}widget control {:#x}", trace_name_, value);
            }
            if (offset >= 0x40'0160 && offset <= 0x40'0190) {
                tracer_.log(TraceCategory::hub, "{}ITTE {} = {:#x}", trace_name_, (offset - 0x40'0160) / 8 + 1,
                            value);
            }
            if (offset == pi_rt_compare_a || offset == pi_rt_compare_b ||
                offset == pi_profile_compare) {
                schedule_comparators();
            }
            if (offset == ni_vector_parms) {
                start_vector_pio();
            }
            return {};
        }
        if (const auto bte = bte_register(offset)) {
            write_bte(bte->first, bte->second, value);
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
            const std::size_t word = level / 64;
            const std::uint64_t old = interrupt_pending(word);
            set_pending(word, (value & pend_mod_set) != 0 ? old | bit : old & ~bit);
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
        case pi_err_int_pend:
            // Clearing (PI_ERR_CLEAR_ALL_A/B) has nothing to clear.
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
        case pi_rt_pend_a:
        case pi_rt_pend_b:
            // hypothesis: bit 0 is written as the new pending state (IRIX writes 0 to clear).
            rt_pending_[offset == pi_rt_pend_b] = (value & 1) != 0;
            update_interrupts();
            return {};
        case pi_prof_pend_a:
        case pi_prof_pend_b:
            prof_pending_[offset == pi_prof_pend_b] = (value & 1) != 0;
            update_interrupts();
            return {};
        case pi_rt_en_a:
        case pi_rt_en_b:
            rt_enable_[offset == pi_rt_en_b] = (value & 1) != 0;
            update_interrupts();
            return {};
        case pi_prof_en_a:
        case pi_prof_en_b:
            prof_enable_[offset == pi_prof_en_b] = (value & 1) != 0;
            update_interrupts();
            return {};
        case md_ureg0_0:
        case md_ureg0_1:
            i2c_.write(offset == md_ureg0_1, static_cast<std::uint8_t>(value));
            return {};
        case md_refresh_control:
            // hypothesis: stored; refresh has no observable effect on emulated memory.
            refresh_control_ = value;
            return {};
        case md_mlan_ctl:
            // Starts a 1-Wire pulse and sample with the written timing.
            mlan_ctl_ = value & mlan_control;
            mlan_line_ = mlan_bus_ == nullptr ||
                         mlan_bus_->pulse((value >> 10) & 0x3ff, (value >> 2) & 0xff);
            tracer_.log(TraceCategory::hub, "{}mlan pulse {} sample {}", trace_name_, (value >> 10) & 0x3ff,
                        (value >> 2) & 0xff);
            return {};
        case iio_wid:
            // Read-only.
            return {};
        case md_memory_config:
            set_memory_config(value);
            tracer_.log(TraceCategory::hub, "{}md memory config {:#018x}", trace_name_, value);
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
                tracer_.log(TraceCategory::hub, "{}llp warm reset {}", trace_name_,
                            (value & ilcsr_warm_reset) != 0 ? "asserted" : "released");
            }
            ilcsr_ = value & ilcsr_writable;
            return {};
        case ni_port_error:
        case ni_port_error_clear:
            // Clearing LLP errors: there are none.
            return {};
        case ni_status_rev_id:
            set_nsri_settings(value & nsri_writable);
            return {};
        case ni_vector_status:
        case ni_return_vector:
            // hypothesis: read-only; the status is cleared through NI_VECTOR_CLEAR.
            return {};
        case ni_vector_read_data:
            // inferred: writable (the PROM writes 0xdeadbeefdeadbeef before a read, observed).
            vector_read_data_ = value;
            return {};
        case ni_port_reset:
            // The reset takes effect after the store that requested it completes.
            if ((value & npr_localreset) != 0) {
                scheduler_.schedule_after(local_reset_event_, VirtualDuration{0});
            }
            if ((value & npr_portreset) != 0) {
                // hypothesis: the link's warm reset has no modeled effect on either end.
                tracer_.log(TraceCategory::hub, "{}port reset{}", trace_name_,
                            neighbor_ != nullptr ? "" : " (no link)");
            }
            return {};
        default:
            if (offset >= pi_err_status_first && offset <= pi_err_status_last && offset % 8 == 0) {
                // A write to any of a slice's four status addresses clears its error.
                // observed: the PROM writes zero to ERR_STATUS0_A_RCLR before provoking its
                // own; IRIX, after reading ERR_STATUS0 for the firmware check and after each
                // address probe, writes zero to ERR_STATUS1_A/B_RCLR. hypothesis: the pair
                // (status 0 and 1) is one error, cleared together.
                err_status0_[(offset - pi_err_status_first) / 32] = 0;
                return {};
            }
            if (offset >= ni_routing_first && offset <= ni_routing_last && offset % 8 == 0) {
                routing_tables_[(offset - ni_routing_first) / 8] = value & exit_port_mask;
                return {};
            }
            if (const IdleRange* range = find_idle(offset); range != nullptr && range->writable) {
                return {};
            }
            break;
        }
    }
    tracer_.log(TraceCategory::hub, "{}unmodeled write {:#08x} width {} value {:#x}", trace_name_, offset,
                byte_count(width), value);
    return std::unexpected(AccessFault::unsupported);
}

} // namespace ultraviolent::ip27
