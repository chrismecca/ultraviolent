#include <ultraviolent/devices/bridge.hpp>

#include <algorithm>
#include <bit>
#include <iterator>
#include <vector>

// Offsets and bits: the IRIX-derived Linux arch/mips/include/asm/pci/bridge.h (BRIDGE_*) and
// asm/xtalk/xwidget.h (WIDGET_*). Reset values: the IP27 PROM's bridge_sanity diagnostic
// (observed; IP27.adoc "Bridge").
namespace ultraviolent::devices {

namespace {

constexpr std::uint64_t int_status = 0x104;   // BRIDGE_INT_STATUS
constexpr std::uint64_t int_enable = 0x10c;   // BRIDGE_INT_ENABLE
constexpr std::uint64_t int_rst_stat = 0x114; // BRIDGE_INT_RST_STAT
constexpr std::uint64_t int_mode = 0x11c;     // BRIDGE_INT_MODE
constexpr std::uint64_t int_addr0 = 0x134;    // BRIDGE_INT_ADDR0, then every 8 bytes
constexpr std::uint64_t int_host_err = 0x12c; // BRIDGE_INT_HOST_ERR
constexpr std::uint64_t device0 = 0x204;      // BRIDGE_DEVICE0
constexpr std::uint64_t nic = 0xb4;           // BRIDGE_NIC

// BRIDGE_NIC is a MicroLAN (1-Wire) master with the MCR encoding of the IRIX-derived Linux
// asm-ia64/sn/nic.h (MCR_PACK(pulse, sample) = pulse << 10 | sample << 2; MCR_DONE bit 1,
// MCR_DATA bit 0; observed: the PROM writes 0x82104, a 520 microsecond reset pulse sampled at
// 65, and polls for DONE). hypothesis: an operation completes before the next access.
constexpr std::uint32_t nic_done = 0x2;
constexpr std::uint32_t nic_rd_data = 0x1;

// BRIDGE_STAT_PCI_GIO_N: the Bridge runs its bus as PCI (observed: bridge_sanity reports "PCI
// bit set to 0" otherwise).
constexpr std::uint32_t status_pci = 1u << 5;
// observed (bridge_sanity) reset values: control with the widget ID in bits 3:0; the request
// timeout; each device register (error lock and page check disable set, IO/MEM set, and the
// device window's offset: 0, 2, then 4-9).
constexpr std::uint32_t control_reset = 0x7f00'33f0;
constexpr std::uint32_t req_timeout_reset = 0xf'ffff;
constexpr std::array<std::uint32_t, 8> device_reset = {
    0x1800'1000, 0x1800'1002, 0x1800'1004, 0x1800'1005,
    0x1800'1006, 0x1800'1007, 0x1800'1008, 0x1800'1009,
};

constexpr std::uint32_t isr_invld_addr = 1u << 24;      // BRIDGE_ISR_INVLD_ADDR
constexpr std::uint32_t isr_pci_mst_timeout = 1u << 11; // BRIDGE_ISR_PCI_MST_TIMEOUT

// Configuration space: BRIDGE_TYPE0_CFG_DEV0 + slot * 0x1000 + function * 0x100.
constexpr std::uint64_t config_base = 0x2'0000;
constexpr std::uint64_t config_end = 0x2'8000;
// Device windows (BRIDGE_DEVIO0-7): devices 0 and 1 have 2 MB, the rest 1 MB.
constexpr std::uint64_t window_base = 0x20'0000;
constexpr std::uint64_t window_end = 0xc0'0000;
// External flash PROM 0 (bridge.h BRIDGE_EXTERNAL_FLASH) and the WIDGET_CONTROL bit that lets
// writes reach it (BRIDGE_CTRL_FLASH_WR_EN). observed (the IP27 PROM's flash driver, by
// disassembly): the BaseIO's port is 16 bits wide. Flash address A is the halfword at byte
// offset 2A; commands are halfword stores there, and the identification reads the halfwords
// at 0 and 2, accepting 8-bit parts (AMD 0x01 with 0xd5 or 0xad) as well as 16-bit ones.
// observed (IRIX's flash utility reads the image byte by byte; the PROM accepts the 16-bit AMD
// 0x22da part): the BaseIO's part is a 16-bit part in word mode, so data is byte-linear.
constexpr std::uint64_t external_flash = 0xc0'0000;
constexpr std::uint64_t external_flash_end = external_flash + Am29f080::size;
constexpr std::uint32_t control_flash_write_enable = 0x8000'0000;
constexpr std::uint32_t device_io_mem = 1u << 12;   // BRIDGE_DEV_DEV_IO_MEM: memory space
constexpr std::uint32_t device_swap = 1u << 13;     // BRIDGE_DEV_DEV_SWAP
constexpr std::uint32_t device_offset = 0xfff;      // BRIDGE_DEV_OFF_MASK, 1 MB units
constexpr std::uint32_t device_swap_dir = 1u << 19; // BRIDGE_DEV_SWAP_DIR

constexpr std::uint64_t dir_map_register = 0x84;    // BRIDGE_DIR_MAP
constexpr std::uint32_t dir_map_add512 = 1u << 17;  // BRIDGE_DIRMAP_ADD512
constexpr std::uint32_t dir_map_offset = 0x1'ffff;  // BRIDGE_DIRMAP_OFF
constexpr std::uint64_t direct_base = 0x8000'0000;  // BRIDGE_DMA_DIRECT_BASE
constexpr std::uint64_t direct_end = 0x1'0000'0000; // + BRIDGE_DMA_DIRECT_SIZE
constexpr std::uint64_t pci64_base = 0x1'0000'0000; // beyond 32-bit PCI addresses
constexpr std::uint64_t xtalk_address_mask = (std::uint64_t{1} << 48) - 1;

// Internal ATE RAM (bridge.h b_int_ate_ram): 128 entries written as doublewords at 0x10000;
// a word read at entry + 4 returns the entry's high word, and the same at 0x11000 + 8n the
// low word. External ATE RAM (b_ext_ate_ram, BRIDGE_EXT_SSRAM) is 0x80000-0xfffff.
constexpr std::uint64_t ate_ram = 0x1'0000;
constexpr std::uint64_t ate_ram_low = 0x1'1000;
constexpr std::uint64_t ate_ram_size = 0x400;
constexpr std::uint64_t ext_ssram = 0x8'0000;
constexpr std::uint64_t ext_ssram_end = 0x10'0000;
// ATE fields (bridge.h mkate): crosstalk address 47:12, target widget 11:8, valid bit 0.
constexpr std::uint64_t ate_valid = 0x1;
constexpr std::uint64_t ate_address_mask = 0x0000'ffff'ffff'f000;
// PCI addresses the ATEs translate (BRIDGE_DMA_MAPPED_BASE/SIZE), by 4 KB or, with
// BRIDGE_CTRL_PAGE_SIZE, 16 KB pages.
constexpr std::uint64_t mapped_base = 0x4000'0000;
constexpr std::uint64_t mapped_end = 0x8000'0000;
constexpr std::uint32_t control_page_size = 1u << 21;

std::uint64_t access_mask(unsigned size) {
    return size >= 8 ? ~std::uint64_t{0} : (std::uint64_t{1} << (8 * size)) - 1;
}

unsigned size_of(AccessWidth width) {
    return static_cast<unsigned>(byte_count(width));
}
constexpr std::uint32_t isr_errors = 0xffff'ff00; // everything but the 8 device interrupts

// BRIDGE_INT_RST_STAT group bits and the INT_STATUS bits each clears (BRIDGE_IRR_*).
struct ResetGroup {
    std::uint32_t select;
    std::uint32_t clears;
};
constexpr ResetGroup reset_groups[] = {
    {1u << 6, 1u << 31},                                                       // MULTI_CLR
    {1u << 5, (1u << 29) | (1u << 22)},                                        // CRP_GRP
    {1u << 4, (1u << 28) | (1u << 26) | (1u << 9)},                            // RESP_BUF_GRP
    {1u << 3, (1u << 23) | (1u << 27) | (1u << 25) | (1u << 24)},              // REQ_DSP_GRP
    {1u << 2, (1u << 21) | (1u << 20) | (1u << 19) | (1u << 18) | (1u << 17)}, // LLP_GRP
    {1u << 1, (1u << 16) | (1u << 30)},                                        // SSRAM_GRP
    {1u << 0, (1u << 15) | (1u << 14) | (1u << 13) | (1u << 12) | (1u << 11) | (1u << 10) |
                  (1u << 8)}, // PCI_GRP (and GIO)
};

// Register offsets bridge.h defines (each the 32-bit register at offset 4 mod 8).
constexpr bool defined_register(std::uint64_t offset) {
    if (offset <= 0x74) {
        return true; // widget registers through BRIDGE_WID_TST_PIN_CTRL
    }
    switch (offset) {
    case 0x84: // DIR_MAP
    case 0x94: // RAM_PERR
    case 0xa4: // ARB
    case 0xb4: // NIC
    case 0xc4: // BUS_TIMEOUT
    case 0xcc: // PCI_CFG
    case 0xd4: // PCI_ERR_UPPER
    case 0xdc: // PCI_ERR_LOWER
        return true;
    default:
        break;
    }
    return (offset >= 0x104 && offset <= 0x16c) || // interrupt registers, INT_ADDR0-7
           (offset >= 0x204 && offset <= 0x27c) || // DEVICE0-7, WR_REQ_BUF0-7
           (offset >= 0x284 && offset <= 0x29c);   // response buffers, status, clear
}

} // namespace

Bridge::Bridge(Tracer& tracer, unsigned port) : tracer_{tracer}, port_{port} {
    for (unsigned slot = 0; slot < dma_ports_.size(); ++slot) {
        dma_ports_[slot].bridge = this;
        dma_ports_[slot].slot = slot;
    }
    reset();
}

bool Bridge::dma(unsigned slot, std::uint64_t address, std::span<std::byte> read,
                 std::span<const std::byte> write) {
    const bool writing = !write.empty();
    const std::uint64_t size = writing ? write.size() : read.size();
    unsigned target = 0;
    std::uint64_t xtalk = 0;
    const std::uint32_t dir_map = registers_[dir_map_register / 8];
    if (address >= pci64_base) {
        // A 64-bit PCI address names its target widget in bits 63:60 and attributes in 59:48
        // (bridge.h PCI64_ATTR_*); bits 47:0 are the crosstalk address (IP27 Linux DMA
        // addresses are masterwid << 60 | PCI64_ATTR_BAR | physical). hypothesis: the
        // prefetch, precise, virtual-channel, barrier, and RMF attributes do not change the data
        // (observed: IRIX's queue addresses carry VIRTUAL and BAR).
        target = static_cast<unsigned>(address >> 60);
        xtalk = address & xtalk_address_mask;
        if (size > xtalk_address_mask + 1 - xtalk) {
            target = xtalk::widget_count;
        }
    } else if (address >= mapped_base && address < mapped_end) {
        // hypothesis: a transfer stays within one page; the internal ATEs cover the first 128
        // pages, and there is no external ATE RAM (see mmio_read).
        const std::uint64_t page_size =
            (registers_[xtalk::widget_control / 8] & control_page_size) != 0 ? 0x4000 : 0x1000;
        const std::uint64_t index = (address - mapped_base) / page_size;
        const std::uint64_t ate = index < ates_.size() ? ates_[index] : 0;
        const std::uint64_t within = (address - mapped_base) % page_size;
        target = xtalk::widget_count;
        if ((ate & ate_valid) != 0 && size <= page_size - within) {
            target = static_cast<unsigned>((ate >> 8) & 0xf);
            xtalk = (ate & ate_address_mask & ~(page_size - 1)) + within;
        }
    } else if (address >= direct_base && size <= direct_end - address &&
               (dir_map & dir_map_add512) == 0) {
        // BRIDGE_DMA_DIRECT_BASE/SIZE: PCI 2 GB up is the direct map, to the widget and 2 GB
        // crosstalk block DIR_MAP names (BRIDGE_DIRMAP_W_ID, BRIDGE_DIRMAP_OFF at address bit
        // 31).
        target = (dir_map >> 20) & 0xf;
        xtalk = (std::uint64_t{dir_map & dir_map_offset} << 31) + (address - direct_base);
    } else {
        target = xtalk::widget_count;
    }
    if (target >= xtalk::widget_count || fabric_ == nullptr) {
        tracer_.log(TraceCategory::bridge, "unmodeled DMA slot {} {:#x} size {:#x}", slot, address,
                    size);
        return false;
    }
    tracer_.log(TraceCategory::bridge, "DMA {} slot {} {:#x} -> widget {:#x} {:#x} size {:#x}",
                writing ? "write" : "read", slot, address, target, xtalk, size);
    // With the device's SWAP_DIR set, PCI bytes keep their addresses. hypothesis: without it,
    // DMA preserves byte lanes like PIO, so PCI byte X is crosstalk byte X ^ 3 (IP27.adoc
    // "Bridge").
    if ((registers_[(device0 + std::uint64_t{8} * slot) / 8] & device_swap_dir) != 0) {
        return writing ? fabric_->dma_write(target, xtalk, write)
                       : fabric_->dma_read(target, xtalk, read);
    }
    const std::uint64_t first = xtalk & ~std::uint64_t{3};
    std::vector<std::byte> lanes((xtalk + size + 3 - first) & ~std::uint64_t{3});
    if (!fabric_->dma_read(target, first, lanes)) {
        return false;
    }
    for (std::uint64_t i = 0; i < size; ++i) {
        std::byte& lane = lanes[((xtalk + i) ^ 3) - first];
        if (writing) {
            lane = write[i];
        } else {
            read[i] = lane;
        }
    }
    return !writing || fabric_->dma_write(target, first, lanes);
}

void Bridge::reset() {
    // hypothesis: registers the diagnostic does not check reset to zero.
    registers_ = {};
    registers_[xtalk::widget_control / 8] = control_reset | (port_ & 0xf);
    registers_[xtalk::widget_req_timeout / 8] = req_timeout_reset;
    for (std::size_t i = 0; i < device_reset.size(); ++i) {
        registers_[(device0 + 8 * i) / 8] = device_reset[i];
    }
    int_status_ = 0;
    pins_ = 0;
    ates_ = {};
    error_interrupt_sent_ = false;
    for (pci::Device* device : slots_) {
        if (device != nullptr) {
            device->pci_reset();
        }
    }
}

// PCI interrupts (Linux pci-xtalk-bridge.c bridge_domain_activate): an enabled pin sends an
// interrupt to the widget interrupt destination with the vector in its INT_ADDR register's
// field bits 7:0 when it rises, and, with its INT_MODE bit, a clear packet when it falls.
// hypothesis: the destination address is INTDEST's (INT_ADDR's host bits name the node, and
// this machine has one); a pin enabled while high sends at once.
void Bridge::set_interrupt_level(std::uint32_t pin, bool asserted) {
    const std::uint32_t bit = 1u << (pin & 7);
    if (((pins_ & bit) != 0) == asserted) {
        return;
    }
    pins_ = asserted ? pins_ | bit : pins_ & ~bit;
    tracer_.log(TraceCategory::bridge, "INT{} {}", pin, asserted ? "asserted" : "deasserted");
    if ((registers_[int_enable / 8] & bit) == 0) {
        return;
    }
    if (asserted) {
        send_pin_interrupt(pin & 7, true);
    } else if ((registers_[int_mode / 8] & bit) != 0) {
        send_pin_interrupt(pin & 7, false);
    }
}

void Bridge::send_pin_interrupt(unsigned pin, bool raise) {
    if (fabric_ == nullptr) {
        return;
    }
    const std::uint32_t upper = registers_[xtalk::widget_intdest_upper / 8];
    const std::uint32_t lower = registers_[xtalk::widget_intdest_lower / 8];
    const std::uint64_t address = (std::uint64_t{upper & 0xffff} << 32) | lower;
    const auto vector =
        static_cast<std::uint8_t>(registers_[(int_addr0 + std::uint64_t{8} * pin) / 8]);
    if (raise) {
        fabric_->send_interrupt((upper >> 16) & 0xf, address, vector);
    } else {
        fabric_->send_interrupt_clear((upper >> 16) & 0xf, address, vector);
    }
}

void Bridge::update_error_interrupt() {
    const std::uint32_t pending = int_status_ & registers_[int_enable / 8] & isr_errors;
    if (pending == 0) {
        error_interrupt_sent_ = false;
        return;
    }
    if (error_interrupt_sent_ || fabric_ == nullptr) {
        return;
    }
    // The error interrupt goes to the widget interrupt destination with the host error
    // field's vector (bridge.h BRIDGE_INT_HOST_ERR; observed: bridge_sanity expects it in
    // PI_INT_PEND0 when INT_HOST_ERR is 0).
    const std::uint32_t upper = registers_[xtalk::widget_intdest_upper / 8];
    const std::uint32_t lower = registers_[xtalk::widget_intdest_lower / 8];
    const std::uint64_t address = (std::uint64_t{upper & 0xffff} << 32) | lower;
    fabric_->send_interrupt((upper >> 16) & 0xf, address,
                            static_cast<std::uint8_t>(registers_[int_host_err / 8]));
    error_interrupt_sent_ = true;
}

void Bridge::raise_error(std::uint32_t bits, std::uint64_t offset) {
    tracer_.log(TraceCategory::bridge, "error {:#010x} at {:#08x}", bits, offset);
    int_status_ |= bits;
    // hypothesis: the error address registers capture the offending offset.
    registers_[xtalk::widget_err_upper / 8] = static_cast<std::uint32_t>(offset >> 32);
    registers_[xtalk::widget_err_lower / 8] = static_cast<std::uint32_t>(offset);
    update_error_interrupt();
}

std::uint32_t Bridge::read_register(std::uint64_t offset) {
    switch (offset) {
    case xtalk::widget_id:
        return id;
    case xtalk::widget_status:
        return status_pci;
    case int_status:
        return int_status_ | pins_;
    case nic:
        return registers_[offset / 8] | nic_done;
    default:
        return registers_[offset / 8];
    }
}

void Bridge::write_register(std::uint64_t offset, std::uint32_t value) {
    switch (offset) {
    case xtalk::widget_id:
    case xtalk::widget_status:
    case int_status:
        // Read-only. hypothesis: the write is ignored without an error.
        return;
    case int_rst_stat:
        for (const ResetGroup& group : reset_groups) {
            if ((value & group.select) != 0) {
                int_status_ &= ~group.clears;
            }
        }
        update_error_interrupt();
        return;
    case nic: {
        // Run the pulse and sample on the NIC bus; with no bus the line stays pulled up.
        const bool line =
            nic_bus_ == nullptr || nic_bus_->pulse((value >> 10) & 0x3ff, (value >> 2) & 0xff);
        registers_[offset / 8] = (value & ~(nic_done | nic_rd_data)) | (line ? nic_rd_data : 0);
        return;
    }
    default: {
        const std::uint32_t old = registers_[offset / 8];
        registers_[offset / 8] = value;
        if (offset == int_enable) {
            update_error_interrupt();
            // Pins enabled while asserted.
            const std::uint32_t newly = value & ~old & pins_ & 0xff;
            for (unsigned pin = 0; pin < 8; ++pin) {
                if ((newly & (1u << pin)) != 0) {
                    send_pin_interrupt(pin, true);
                }
            }
        }
        return;
    }
    }
}

// PCI is little-endian and crosstalk big-endian: the Bridge swaps byte lanes, so a 32-bit
// access sees the PCI dword's value and narrower accesses address byte (offset ^ (4 - size))
// (Linux arch/mips/pci/ops-bridge.c, "where ^ (4 - size)"). A doubleword is two dwords, the
// lower address in the high half. hypothesis: DEV_SWAP is not modeled.
std::expected<std::uint64_t, AccessFault> Bridge::config_access(std::uint64_t offset, unsigned size,
                                                                const std::uint64_t* write) {
    if (size == 8) {
        const std::uint64_t high = write != nullptr ? *write >> 32 : 0;
        const std::uint64_t low = write != nullptr ? *write & 0xffff'ffff : 0;
        auto first = config_access(offset, 4, write != nullptr ? &high : nullptr);
        if (!first) {
            return first;
        }
        auto second = config_access(offset + 4, 4, write != nullptr ? &low : nullptr);
        if (!second) {
            return second;
        }
        return (*first << 32) | *second;
    }
    const unsigned slot = static_cast<unsigned>((offset - config_base) >> 12) & 7;
    const unsigned function = static_cast<unsigned>(offset >> 8) & 7;
    pci::Device* device = slots_[slot];
    if (device == nullptr || function != 0) {
        // An empty slot does not answer: the read fails (Linux ops-bridge.c treats the bus
        // error as "device not found"). hypothesis: no Bridge error bits are set.
        tracer_.log(TraceCategory::pci, "config slot {} function {} empty", slot, function);
        return std::unexpected(AccessFault::unmapped);
    }
    const std::uint64_t pci_address = size == 4 ? offset & 0xff : (offset & 0xff) ^ (4 - size);
    const auto reg = static_cast<std::uint8_t>(pci_address & 0xfc);
    const unsigned shift = 8 * static_cast<unsigned>(pci_address & 3);
    if (write != nullptr) {
        tracer_.log(TraceCategory::pci, "config write slot {} {:#04x} size {} = {:#x}", slot,
                    pci_address, size, *write);
        device->config_write(reg, static_cast<std::uint32_t>(*write << shift),
                             ((1u << size) - 1) << (pci_address & 3));
        return 0;
    }
    const std::uint64_t value = (device->config_read(reg) >> shift) & access_mask(size);
    tracer_.log(TraceCategory::pci, "config read slot {} {:#04x} size {} = {:#x}", slot,
                pci_address, size, value);
    return value;
}

std::expected<std::uint64_t, AccessFault> Bridge::window_access(std::uint64_t offset, unsigned size,
                                                                const std::uint64_t* write) {
    if (size == 8) {
        const std::uint64_t high = write != nullptr ? *write >> 32 : 0;
        const std::uint64_t low = write != nullptr ? *write & 0xffff'ffff : 0;
        auto first = window_access(offset, 4, write != nullptr ? &high : nullptr);
        if (!first) {
            return first;
        }
        auto second = window_access(offset + 4, 4, write != nullptr ? &low : nullptr);
        if (!second) {
            return second;
        }
        return (*first << 32) | *second;
    }
    // BRIDGE_DEVIO0/1 are 2 MB at 0x200000 and 0x400000; DEVIO2-7 1 MB from 0x600000.
    const unsigned device = offset < 0x60'0000 ? static_cast<unsigned>((offset >> 21) - 1)
                                               : static_cast<unsigned>((offset >> 20) - 4);
    const std::uint64_t base = device < 2 ? window_base + (std::uint64_t{device} << 21)
                                          : 0x60'0000 + (std::uint64_t{device - 2} << 20);
    const std::uint32_t reg = registers_[(device0 + std::uint64_t{8} * device) / 8];
    const pci::Space space = (reg & device_io_mem) != 0 ? pci::Space::memory : pci::Space::io;
    // Without DEV_SWAP the window preserves byte lanes, like configuration space: a narrower
    // access at byte A reaches PCI byte A ^ (4 - size) and values pass unchanged. With it,
    // bytes keep their addresses and wider values arrive byte-reversed. inferred: IP27 Linux
    // swizzles I/O byte and halfword addresses by 3 and 2 with data unchanged
    // (asm/mach-ip27/mangle-port.h); the PROM, with DEV_SWAP clear, writes 16550 register n of
    // the IOC3 at window byte base + n, which is PCI byte base + (n ^ 3) where ioc3.h puts it
    // (iu_lcr first), and reads the ISP1020's semaphore (PCI 0x0c) at window byte 0x0e.
    const bool swap = (reg & device_swap) != 0;
    const std::uint64_t byte = !swap && size < 4 ? offset ^ (4 - size) : offset;
    const std::uint64_t pci_address = (std::uint64_t{reg & device_offset} << 20) + (byte - base);
    const auto to_pci = [&](std::uint64_t value) {
        const auto v = static_cast<std::uint32_t>(value);
        if (!swap || size == 1) {
            return v;
        }
        return size == 2 ? static_cast<std::uint32_t>(std::byteswap(static_cast<std::uint16_t>(v)))
                         : std::byteswap(v);
    };
    for (pci::Device* target : slots_) {
        if (target == nullptr) {
            continue;
        }
        if (write != nullptr) {
            if (target->write(space, pci_address, size, to_pci(*write))) {
                return 0;
            }
        } else if (const auto value = target->read(space, pci_address, size)) {
            return to_pci(*value); // the swap is its own inverse
        }
    }
    // Nobody claimed it: a master abort.
    raise_error(isr_pci_mst_timeout, offset);
    return std::unexpected(AccessFault::unmapped);
}

std::expected<std::uint64_t, AccessFault> Bridge::mmio_read(std::uint64_t offset,
                                                            AccessWidth width) {
    if (offset >= config_base && offset < config_end) {
        return config_access(offset, size_of(width), nullptr);
    }
    if (offset >= window_base && offset < window_end) {
        return window_access(offset, size_of(width), nullptr);
    }
    if (offset >= ate_ram && offset < ate_ram + ate_ram_size && width == AccessWidth::bits32 &&
        offset % 8 == 4) {
        return ates_[(offset - ate_ram) / 8] >> 32;
    }
    if (offset >= ate_ram_low && offset < ate_ram_low + ate_ram_size &&
        width == AccessWidth::bits32 && offset % 8 == 4) {
        return ates_[(offset - ate_ram_low) / 8] & 0xffff'ffff;
    }
    if (flash_ != nullptr && offset >= external_flash && offset < external_flash_end) {
        // Byte-linear: byte b is the high (even b) or low (odd b) byte of word b / 2.
        const unsigned size = size_of(width);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i) {
            const std::uint64_t byte = offset - external_flash + i;
            const std::uint16_t word = flash_->read_word(static_cast<std::uint32_t>(byte >> 1));
            value = (value << 8) | ((byte & 1) != 0 ? word & 0xff : word >> 8);
        }
        tracer_.log(TraceCategory::bridge, "flash read {:#x} width {} = {:#x}",
                    offset - external_flash, size, value);
        return value;
    }
    if (offset >= ext_ssram && offset < ext_ssram_end) {
        // hypothesis: the BaseIO Bridge has no external ATE SSRAM; the access completes and
        // reads 0 (observed: IRIX sizes the SSRAM by writing a probe value at the last entry
        // for each size and reading it back, and treats a mismatch as absence).
        return 0;
    }
    if (offset < register_space && ((width == AccessWidth::bits32 && offset % 8 == 4) ||
                                    (width == AccessWidth::bits64 && offset % 8 == 0))) {
        const std::uint64_t reg = offset | 4;
        if (!defined_register(reg)) {
            raise_error(isr_invld_addr, reg);
            return std::unexpected(AccessFault::unsupported);
        }
        const std::uint32_t value = read_register(reg);
        tracer_.log(TraceCategory::bridge, "read {:#07x} = {:#x}", reg, value);
        return value;
    }
    tracer_.log(TraceCategory::bridge, "unmodeled read {:#08x} width {}", offset,
                byte_count(width));
    return std::unexpected(AccessFault::unsupported);
}

std::expected<void, AccessFault> Bridge::mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) {
    if (offset >= config_base && offset < config_end) {
        if (auto result = config_access(offset, size_of(width), &value); !result) {
            return std::unexpected(result.error());
        }
        return {};
    }
    if (offset >= ate_ram && offset < ate_ram + ate_ram_size && width == AccessWidth::bits64) {
        ates_[(offset - ate_ram) / 8] = value;
        return {};
    }
    if (offset >= ext_ssram && offset < ext_ssram_end) {
        return {}; // no SSRAM fitted (see mmio_read)
    }
    if (flash_ != nullptr && offset >= external_flash && offset < external_flash_end) {
        if ((registers_[xtalk::widget_control / 8] & control_flash_write_enable) == 0) {
            // hypothesis: without FLASH_WR_EN the write completes and is dropped.
            tracer_.log(TraceCategory::bridge, "flash write {:#x} without FLASH_WR_EN", offset);
            return {};
        }
        // Halfword stores are word cycles (hypothesis: wider stores are successive words;
        // byte stores are not used and are dropped).
        const unsigned size = size_of(width);
        for (unsigned i = 0; i + 1 < size; i += 2) {
            const std::uint64_t byte = offset - external_flash + i;
            flash_->write_word(static_cast<std::uint32_t>(byte >> 1),
                               static_cast<std::uint16_t>(value >> (8 * (size - 2 - i))));
        }
        return {};
    }
    if (offset >= window_base && offset < window_end) {
        if (auto result = window_access(offset, size_of(width), &value); !result) {
            return std::unexpected(result.error());
        }
        return {};
    }
    if (offset < register_space && ((width == AccessWidth::bits32 && offset % 8 == 4) ||
                                    (width == AccessWidth::bits64 && offset % 8 == 0))) {
        const std::uint64_t reg = offset | 4;
        tracer_.log(TraceCategory::bridge, "write {:#07x} = {:#x}", reg, value);
        if (!defined_register(reg)) {
            // observed (bridge_sanity): a write to an undefined register sets INVLD_ADDR.
            raise_error(isr_invld_addr, reg);
            return {};
        }
        write_register(reg, static_cast<std::uint32_t>(value));
        return {};
    }
    tracer_.log(TraceCategory::bridge, "unmodeled write {:#08x} width {} value {:#x}", offset,
                byte_count(width), value);
    return std::unexpected(AccessFault::unsupported);
}

void Bridge::xtalk_interrupt(std::uint64_t address, std::uint8_t vector) {
    tracer_.log(TraceCategory::bridge, "unexpected interrupt to {:#x} vector {:#x}", address,
                vector);
}

void Bridge::save_state(StateImage& image) const {
    image.put("bridge.registers", registers_);
    image.put("bridge.ates", ates_);
    image.put("bridge.pins", pins_);
    image.put("bridge.int_status", int_status_);
    image.put("bridge.error_interrupt_sent", error_interrupt_sent_);
}

void Bridge::load_state(const StateImage& image) {
    image.get("bridge.registers", registers_);
    image.get("bridge.ates", ates_);
    image.get("bridge.pins", pins_);
    image.get("bridge.int_status", int_status_);
    image.get("bridge.error_interrupt_sent", error_interrupt_sent_);
}

} // namespace ultraviolent::devices
