#include <ultraviolent/devices/isp1020.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <span>
#include <utility>

// Register offsets, commands, and completion codes: Linux drivers/scsi/qlogicisp.c (2.6.12).
namespace ultraviolent::devices {

namespace {

// PCI_VENDOR_ID_QLOGIC, PCI_DEVICE_ID_QLOGIC_ISP1020; class 0x0100 (SCSI). observed
// (io_config_space): status 0x0200 (medium DEVSEL) after reset. hypothesis: revision 5, a
// 256-byte I/O BAR and a 4 KB memory BAR, interrupt pin A.
constexpr std::uint16_t isp_status = 0x0200;
constexpr std::uint32_t isp_class_revision = 0x0100'0005;

constexpr std::uint32_t isp_cfg0 = 0x04;
constexpr std::uint32_t isp_cfg1 = 0x06;
constexpr std::uint32_t pci_intf_ctl = 0x08;
constexpr std::uint32_t pci_intf_sts = 0x0a;
constexpr std::uint32_t pci_semaphore = 0x0c;
constexpr std::uint32_t mbox0 = 0x70;
constexpr std::uint32_t mbox7 = 0x7e;
constexpr std::uint32_t host_hccr = 0xc0;

// ISP_CFG0 hardware revision. hypothesis: ISP_CFG0_1040B (slate reports QL1040B,
// OBS-SLATE-0001).
constexpr std::uint16_t cfg0_revision = 0x0005;
constexpr std::uint16_t isp_reset = 0x0001;      // PCI_INTF_CTL ISP_RESET
constexpr std::uint16_t isp_en_int = 0x0002;     // PCI_INTF_CTL ISP_EN_INT
constexpr std::uint16_t isp_en_risc = 0x0004;    // PCI_INTF_CTL ISP_EN_RISC
constexpr std::uint16_t intf_risc_int = 0x0004;  // PCI_INTF_STS: RISC interrupt pending
constexpr std::uint16_t semaphore_lock = 0x0001; // set by the RISC with mailbox status
// HCCR reads. hypothesis (the ISP1040 bit names of Linux qla1280.h): bit 7 host interrupt
// pending, bit 5 RISC paused.
constexpr std::uint16_t hccr_host_int = 0x0080;
constexpr std::uint16_t hccr_paused = 0x0020;

// HCCR commands, bits 15:12.
constexpr std::uint16_t hccr_reset = 0x1;
constexpr std::uint16_t hccr_pause = 0x2;
constexpr std::uint16_t hccr_release = 0x3;
constexpr std::uint16_t hccr_set_host_intr = 0x5;
constexpr std::uint16_t hccr_clear_host_intr = 0x6;
constexpr std::uint16_t hccr_clear_risc_intr = 0x7;

// Mailbox commands.
constexpr std::uint16_t mbox_no_op = 0x00;
constexpr std::uint16_t mbox_load_ram = 0x01;
constexpr std::uint16_t mbox_exec_firmware = 0x02;
constexpr std::uint16_t mbox_dump_ram = 0x03;
constexpr std::uint16_t mbox_write_ram_word = 0x04;
constexpr std::uint16_t mbox_read_ram_word = 0x05;
constexpr std::uint16_t mbox_mailbox_reg_test = 0x06;
constexpr std::uint16_t mbox_verify_checksum = 0x07;

// The downloaded firmware's commands.
constexpr std::uint16_t mbox_about_firmware = 0x08;
constexpr std::uint16_t mbox_init_req_queue = 0x10;
constexpr std::uint16_t mbox_init_res_queue = 0x11;
constexpr std::uint16_t mbox_stop_firmware = 0x14;
constexpr std::uint16_t mbox_get_firmware_status = 0x1f;
constexpr std::uint16_t mbox_get_first = 0x20; // GET_INIT_SCSI_ID
constexpr std::uint16_t mbox_get_last = 0x29;  // GET_DEV_QUEUE_PARAMS
constexpr std::uint16_t mbox_set_first = 0x30; // SET_INIT_SCSI_ID
constexpr std::uint16_t mbox_set_target_params = 0x38;
constexpr std::uint16_t mbox_set_device_queue = 0x39;
// ISP1040 firmware commands (Linux qla1280.h MBC_*).
constexpr std::uint16_t mbox_set_reset_delay = 0x3a;
constexpr std::uint16_t mbox_set_system_parameter = 0x45;
constexpr std::uint16_t mbox_set_firmware_features = 0x4a;
constexpr std::uint16_t mbox_enable_target_mode = 0x55;
constexpr std::uint16_t mbox_set_data_overrun_recovery = 0x5a;
constexpr std::uint16_t mbox_load_ram_64 = 0x09;
constexpr std::uint16_t mbox_dump_ram_64 = 0x0a;
// The 64-bit-address variants (MBOX_CMD_INIT_REQUEST_QUEUE_64 and _RESPONSE_QUEUE_64,
// CONFIG_QL_ISP_A64): address bits 63:48 in mailbox 6 and 47:32 in mailbox 7.
constexpr std::uint16_t mbox_init_req_queue_64 = 0x52;
constexpr std::uint16_t mbox_init_res_queue_64 = 0x53;

// IOCBs (struct Entry_header and friends): 64 bytes, little-endian.
constexpr std::size_t entry_size = 64;
constexpr std::uint8_t entry_command = 1;
constexpr std::uint8_t entry_continuation = 2;
constexpr std::uint8_t entry_status = 3;
constexpr std::uint8_t entry_marker = 4;
// CONFIG_QL_ISP_A64 entries: data segments carry a high address word.
constexpr std::uint8_t entry_command_64 = 9;
constexpr std::uint8_t entry_continuation_64 = 0xa;
constexpr std::uint8_t eflag_bad_header = 4;
constexpr std::size_t command_segments = 4;
constexpr std::size_t continuation_segments = 7;
constexpr std::size_t command_segments_64 = 2;
constexpr std::size_t continuation_segments_64 = 5;
// Status entry completion status and state flags.
constexpr std::uint16_t cs_complete = 0x0000;
constexpr std::uint16_t cs_incomplete = 0x0001;
constexpr std::uint16_t cs_dma_error = 0x0002;
constexpr std::uint16_t cs_data_underrun = 0x0015;
constexpr std::uint16_t sf_got_bus = 0x0100;
constexpr std::uint16_t sf_got_target = 0x0200;
constexpr std::uint16_t sf_sent_cdb = 0x0400;
constexpr std::uint16_t sf_transferred_data = 0x0800;
constexpr std::uint16_t sf_got_status = 0x1000;
constexpr std::uint16_t sf_got_sense = 0x2000;
// Status entry status flags (qlogicisp.c STF_TIMEOUT).
constexpr std::uint16_t stf_timeout = 0x0040;
constexpr std::size_t sense_capacity = 32;

std::string fmt_bytes(std::span<const std::byte> bytes) {
    std::string text;
    for (const std::byte b : bytes) {
        text += std::format("{:02x}", std::to_integer<unsigned>(b));
    }
    return text;
}

std::uint16_t get16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset]) |
                                      std::to_integer<unsigned>(bytes[offset + 1]) << 8);
}

std::uint32_t get32(std::span<const std::byte> bytes, std::size_t offset) {
    return get16(bytes, offset) | std::uint32_t{get16(bytes, offset + 2)} << 16;
}

void put16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xff);
    bytes[offset + 1] = static_cast<std::byte>(value >> 8);
}

void put32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    put16(bytes, offset, static_cast<std::uint16_t>(value));
    put16(bytes, offset + 2, static_cast<std::uint16_t>(value >> 16));
}

// A data transfer through an IOCB's segment list (struct dataseg: base, count).
class SegmentTransfer final : public scsi::DataPhase {
  public:
    struct Segment {
        std::uint64_t base;
        std::uint32_t count;
    };
    SegmentTransfer(pci::DmaPort* dma, std::vector<Segment> segments)
        : dma_{dma}, segments_{std::move(segments)} {}

    std::size_t data_in(std::span<const std::byte> bytes) override {
        return transfer(bytes.size(), [&](std::uint64_t address, std::size_t done, std::size_t n) {
            return dma_->dma_write(address, bytes.subspan(done, n));
        });
    }
    std::size_t data_out(std::span<std::byte> bytes) override {
        return transfer(bytes.size(), [&](std::uint64_t address, std::size_t done, std::size_t n) {
            return dma_->dma_read(address, bytes.subspan(done, n));
        });
    }
    [[nodiscard]] std::uint64_t capacity() const {
        std::uint64_t total = 0;
        for (const Segment& s : segments_) {
            total += s.count;
        }
        return total;
    }
    [[nodiscard]] std::uint64_t transferred() const {
        return transferred_;
    }
    [[nodiscard]] bool failed() const {
        return failed_;
    }

  private:
    template <class Move> std::size_t transfer(std::size_t size, Move move) {
        std::size_t done = 0;
        while (done < size && segment_ < segments_.size() && !failed_) {
            const Segment& s = segments_[segment_];
            const std::size_t n = std::min<std::size_t>(size - done, s.count - offset_);
            if (dma_ == nullptr || !move(s.base + offset_, done, n)) {
                failed_ = true;
                break;
            }
            done += n;
            offset_ += static_cast<std::uint32_t>(n);
            if (offset_ == s.count) {
                ++segment_;
                offset_ = 0;
            }
        }
        transferred_ += done;
        return done;
    }

    pci::DmaPort* dma_;
    std::vector<Segment> segments_;
    std::size_t segment_{};
    std::uint32_t offset_{};
    std::uint64_t transferred_{};
    bool failed_{};
};

// A buffer the target fills (auto REQUEST SENSE).
class BufferTransfer final : public scsi::DataPhase {
  public:
    explicit BufferTransfer(std::span<std::byte> buffer) : buffer_{buffer} {}
    std::size_t data_in(std::span<const std::byte> bytes) override {
        const std::size_t n = std::min(bytes.size(), buffer_.size() - used_);
        std::copy_n(bytes.begin(), n, buffer_.begin() + static_cast<std::ptrdiff_t>(used_));
        used_ += n;
        return n;
    }
    std::size_t data_out(std::span<std::byte> /*bytes*/) override {
        return 0;
    }
    [[nodiscard]] std::size_t used() const {
        return used_;
    }

  private:
    std::span<std::byte> buffer_;
    std::size_t used_{};
};

// Completion status in outgoing mailbox 0.
constexpr std::uint16_t command_complete = 0x4000;
constexpr std::uint16_t invalid_command = 0x4001;
constexpr std::uint16_t host_interface_error = 0x4002;
constexpr std::uint16_t test_failed = 0x4003;

} // namespace

Isp1020::Isp1020(Scheduler& scheduler, Tracer& tracer, std::string name)
    : scheduler_{scheduler}, tracer_{tracer}, name_{std::move(name)},
      config_{0x1077,
              0x1020,
              isp_status,
              isp_class_revision,
              {pci::ConfigHeader::Bar{pci::Space::io, 0x100},
               pci::ConfigHeader::Bar{pci::Space::memory, 0x1000}},
              1},
      command_done_{
          scheduler.add_event("isp." + name_ + ".mailbox", [this] { complete_command(); })},
      queue_work_{scheduler.add_event("isp." + name_ + ".queue", [this] { service_requests(); })},
      ram_(ram_words) {}

std::uint32_t Isp1020::config_read(std::uint8_t reg) {
    return config_.read(reg);
}

void Isp1020::config_write(std::uint8_t reg, std::uint32_t value, unsigned byte_enable) {
    config_.write(reg, value, byte_enable);
}

std::optional<std::uint32_t> Isp1020::read(pci::Space space, std::uint64_t address, unsigned size) {
    const auto hit = config_.decode(space, address);
    if (!hit) {
        return std::nullopt;
    }
    // 16-bit little-endian registers; wider and narrower accesses take their byte lanes.
    const auto offset = static_cast<std::uint32_t>(hit->offset);
    const std::uint32_t low = read_register(offset & ~1u);
    std::uint32_t value = low;
    if (size == 1) {
        value = (offset & 1) != 0 ? low >> 8 : low & 0xff;
    } else if (size == 4) {
        value = low | (std::uint32_t{read_register((offset & ~1u) + 2)} << 16);
    }
    tracer_.log(TraceCategory::pci, "{} read {:#06x} size {} = {:#x}", name_, offset, size, value);
    return value;
}

bool Isp1020::write(pci::Space space, std::uint64_t address, unsigned size, std::uint32_t value) {
    const auto hit = config_.decode(space, address);
    if (!hit) {
        return false;
    }
    const auto offset = static_cast<std::uint32_t>(hit->offset);
    tracer_.log(TraceCategory::pci, "{} write {:#06x} size {} = {:#x}", name_, offset, size, value);
    if (size == 4) {
        write_register(offset, static_cast<std::uint16_t>(value));
        write_register(offset + 2, static_cast<std::uint16_t>(value >> 16));
    } else if (size == 2) {
        write_register(offset & ~1u, static_cast<std::uint16_t>(value));
    } else {
        // hypothesis: a byte write stores into its half of the register.
        const std::uint32_t reg = offset & ~1u;
        const std::uint16_t old = read_register(reg);
        const unsigned shift = (offset & 1) * 8;
        write_register(
            reg, static_cast<std::uint16_t>((old & ~(0xffu << shift)) | ((value & 0xff) << shift)));
    }
    return true;
}

std::uint16_t Isp1020::read_register(std::uint32_t offset) {
    switch (offset) {
    case 0x00:
        return 0x1077; // PCI_ID_LOW
    case 0x02:
        return 0x1020; // PCI_ID_HIGH
    case isp_cfg0:
        return cfg0_revision;
    case isp_cfg1:
        return cfg1_;
    case pci_intf_ctl:
        return interface_control_;
    case pci_intf_sts:
        return risc_interrupt_ ? intf_risc_int : 0;
    case pci_semaphore:
        return semaphore_;
    case host_hccr:
        return static_cast<std::uint16_t>((host_interrupt_ ? hccr_host_int : 0) |
                                          (paused_ ? hccr_paused : 0));
    default:
        break;
    }
    if (offset >= mbox0 && offset <= mbox7) {
        return mailbox_out_[(offset - mbox0) / 2];
    }
    const auto found = storage_.find(offset);
    const std::uint16_t value = found != storage_.end() ? found->second : 0;
    tracer_.log(TraceCategory::scsi, "{} read {:#06x} = {:#x}", name_, offset, value);
    return value;
}

void Isp1020::write_register(std::uint32_t offset, std::uint16_t value) {
    store_register(offset, value);
    update_interrupt();
}

void Isp1020::update_interrupt() {
    // hypothesis: INTA follows the RISC interrupt when the host enabled both interrupts and
    // RISC interrupts (Linux isp1020_enable_irqs writes ISP_EN_INT | ISP_EN_RISC).
    constexpr std::uint16_t enables = isp_en_int | isp_en_risc;
    interrupt_.set_level(risc_interrupt_ && (interface_control_ & enables) == enables);
}

void Isp1020::store_register(std::uint32_t offset, std::uint16_t value) {
    switch (offset) {
    case isp_cfg1:
        cfg1_ = value;
        return;
    case pci_intf_ctl:
        if ((value & isp_reset) != 0) {
            tracer_.log(TraceCategory::scsi, "{} chip reset", name_);
            cfg1_ = 0;
            reset_risc();
        }
        interface_control_ = static_cast<std::uint16_t>(value & ~isp_reset); // self-clearing
        return;
    case pci_semaphore:
        semaphore_ = value & semaphore_lock;
        return;
    case host_hccr:
        host_command(value);
        return;
    default:
        break;
    }
    if (offset >= mbox0 && offset <= mbox7) {
        mailbox_in_[(offset - mbox0) / 2] = value;
        // Mailbox 4 is also the request queue's in-pointer: the running firmware picks up new
        // entries (qlogicisp.c isp1020_queuecommand).
        if (offset == mbox0 + 8 && firmware_running_ && requests_.length != 0 &&
            !scheduler_.is_pending(queue_work_)) {
            scheduler_.schedule_after(queue_work_, command_time);
        }
        return;
    }
    tracer_.log(TraceCategory::scsi, "{} write {:#06x} = {:#x}", name_, offset, value);
    storage_[offset] = value;
}

// hypothesis: RST# resets the registers and the RISC; RISC RAM keeps its contents (static RAM).
void Isp1020::pci_reset() {
    config_.reset();
    cfg1_ = 0;
    interface_control_ = 0;
    storage_.clear();
    reset_risc();
    paused_ = false;
    update_interrupt();
}

void Isp1020::reset_risc() {
    scheduler_.cancel(command_done_);
    scheduler_.cancel(queue_work_);
    risc_interrupt_ = false;
    host_interrupt_ = false;
    paused_ = true;
    semaphore_ = 0;
    mailbox_out_ = {};
    firmware_running_ = false;
    // The queues live in the RISC's state: a reset discards them.
    requests_ = {};
    responses_ = {};
    parameters_.clear();
}

void Isp1020::host_command(std::uint16_t command) {
    switch (command >> 12) {
    case hccr_reset:
        tracer_.log(TraceCategory::scsi, "{} RISC reset", name_);
        reset_risc();
        return;
    case hccr_pause:
        paused_ = true;
        scheduler_.cancel(command_done_);
        return;
    case hccr_release:
        paused_ = false;
        if (host_interrupt_) {
            start_command();
        }
        return;
    case hccr_set_host_intr:
        host_interrupt_ = true;
        if (!paused_) {
            start_command();
        }
        return;
    case hccr_clear_host_intr:
        host_interrupt_ = false;
        scheduler_.cancel(command_done_);
        return;
    case hccr_clear_risc_intr:
        risc_interrupt_ = false;
        return;
    default:
        // NOP, single step, breakpoint enable, BIOS disable, test mode: no modeled effect.
        tracer_.log(TraceCategory::scsi, "{} HCCR {:#06x}", name_, command);
        return;
    }
}

void Isp1020::start_command() {
    if (!scheduler_.is_pending(command_done_)) {
        scheduler_.schedule_after(command_done_, command_time);
    }
}

void Isp1020::complete_command() {
    if (!host_interrupt_ || paused_) {
        return;
    }
    host_interrupt_ = false;
    execute();
    risc_interrupt_ = true;
    semaphore_ |= semaphore_lock;
    update_interrupt();
}

void Isp1020::execute() {
    const auto& in = mailbox_in_;
    auto& out = mailbox_out_;
    const std::uint16_t command = in[0];
    std::uint16_t status = command_complete;
    switch (command) {
    case mbox_no_op:
        break;
    case mbox_load_ram:
        if (!dma_read_words(std::uint32_t{in[2]} << 16 | in[3], in[1], in[4])) {
            status = host_interface_error;
        }
        break;
    case mbox_dump_ram:
        if (!dma_write_words(std::uint32_t{in[2]} << 16 | in[3], in[1], in[4])) {
            status = host_interface_error;
        }
        break;
    case mbox_load_ram_64:
    case mbox_dump_ram_64: {
        // MBC_LOAD_RAM_A64_ROM, MBC_DUMP_RAM_A64_ROM: address bits 63:48 in mailbox 6, 47:32
        // in 7 (Linux qla1280.c).
        const std::uint64_t address = std::uint64_t{in[6]} << 48 | std::uint64_t{in[7]} << 32 |
                                      std::uint64_t{in[2]} << 16 | in[3];
        const bool ok = command == mbox_load_ram_64 ? dma_read_words(address, in[1], in[4])
                                                    : dma_write_words(address, in[1], in[4]);
        if (!ok) {
            status = host_interface_error;
        }
        break;
    }
    case mbox_exec_firmware:
        firmware_running_ = true;
        firmware_start_ = in[1];
        break;
    case mbox_write_ram_word:
        ram_[in[1]] = in[2];
        break;
    case mbox_read_ram_word:
        out[2] = ram_[in[1]];
        break;
    case mbox_mailbox_reg_test:
        for (std::size_t i = 1; i < out.size(); ++i) {
            out[i] = in[i];
        }
        break;
    case mbox_verify_checksum: {
        // inferred: a firmware image gives its length in words in its fourth word, and that
        // many words sum to zero (Linux qlogicisp_asm.c risc_code01 and risc_code_length01).
        const std::uint16_t start = in[1];
        const std::uint16_t length = ram_[static_cast<std::uint16_t>(start + 3)];
        std::uint16_t sum = 0;
        for (std::uint32_t i = 0; i < length; ++i) {
            sum = static_cast<std::uint16_t>(sum + ram_[static_cast<std::uint16_t>(start + i)]);
        }
        out[2] = sum;
        if (sum != 0) {
            status = test_failed;
        }
        break;
    }
    default:
        if (!firmware_running_ || !execute_firmware_command()) {
            status = invalid_command;
        }
        break;
    }
    out[0] = status;
    tracer_.log(TraceCategory::scsi,
                "{} mailbox {:#06x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} -> {:#06x} {:#x} "
                "{:#x} {:#x}",
                name_, command, in[1], in[2], in[3], in[4], in[5], in[6], in[7], out[0], out[1],
                out[2], out[3]);
}

bool Isp1020::execute_firmware_command() {
    const auto& in = mailbox_in_;
    auto& out = mailbox_out_;
    const std::uint16_t command = in[0];
    if (command == mbox_about_firmware) {
        // hypothesis: the firmware reports the version its banner names ("Version 02.55"):
        // major, minor, and subminor 0.
        std::string text;
        for (std::uint16_t i = 0; i < 64; ++i) {
            const std::uint16_t word = ram_[static_cast<std::uint16_t>(firmware_start_ + i)];
            text.push_back(static_cast<char>(word >> 8));
            text.push_back(static_cast<char>(word & 0xff));
        }
        const std::size_t at = text.find("Version ");
        unsigned major = 0;
        unsigned minor = 0;
        if (at != std::string::npos) {
            (void)std::sscanf(text.c_str() + at + 8, "%u.%u", &major, &minor);
        }
        out[1] = static_cast<std::uint16_t>(major);
        out[2] = static_cast<std::uint16_t>(minor);
        out[3] = 0;
        return true;
    }
    const std::uint64_t address = std::uint64_t{in[2]} << 16 | in[3];
    const std::uint64_t address_64 =
        std::uint64_t{in[6]} << 48 | std::uint64_t{in[7]} << 32 | address;
    if (command == mbox_init_req_queue || command == mbox_init_req_queue_64) {
        requests_ = {in[1], command == mbox_init_req_queue ? address : address_64, in[4]};
        out[4] = requests_.pointer;
        return true;
    }
    if (command == mbox_init_res_queue || command == mbox_init_res_queue_64) {
        responses_ = {in[1], command == mbox_init_res_queue ? address : address_64, 0};
        out[5] = 0;
        return true;
    }
    if (command == mbox_stop_firmware) {
        firmware_running_ = false;
        return true;
    }
    if (command == mbox_get_firmware_status) {
        out[1] = 0; // no commands outstanding
        out[2] = 0;
        return true;
    }
    // Parameters, keyed by their SET command; per-target and per-device ones also by mailbox 1
    // (target << 8 | lun). GET command n reads what SET command n + 0x10 stored.
    const auto key = [&](unsigned set_command) {
        const bool indexed =
            set_command == mbox_set_target_params || set_command == mbox_set_device_queue;
        return set_command << 16 | (indexed ? in[1] : 0u);
    };
    if ((command >= mbox_set_first && command <= mbox_set_reset_delay) ||
        command == mbox_set_system_parameter || command == mbox_set_firmware_features ||
        command == mbox_set_data_overrun_recovery || command == mbox_enable_target_mode) {
        parameters_[key(command)] = in;
        return true;
    }
    if (command >= mbox_get_first && command <= mbox_get_last) {
        if (const auto found = parameters_.find(key(command + 0x10u)); found != parameters_.end()) {
            for (std::size_t i = 1; i < out.size(); ++i) {
                out[i] = found->second[i];
            }
        } else {
            for (std::size_t i = 1; i < out.size(); ++i) {
                out[i] = 0;
            }
        }
        return true;
    }
    // Queue control, aborts, bus reset: accepted with nothing to do (no command is ever
    // outstanding, since IOCBs complete at once).
    if (command >= 0x13 && command <= 0x1c) {
        return true;
    }
    return false;
}

void Isp1020::service_requests() {
    if (!firmware_running_ || requests_.length == 0 || responses_.length == 0) {
        return;
    }
    const std::uint16_t in = mailbox_in_[4];
    std::array<std::byte, entry_size> entry{};
    // At most one pass over the ring: an in-pointer past its end is never reached.
    for (unsigned n = 0; requests_.pointer != in && n < requests_.length; ++n) {
        const std::uint64_t address = requests_.address + requests_.pointer * entry_size;
        std::uint16_t pointer =
            static_cast<std::uint16_t>((requests_.pointer + 1) % requests_.length);
        if (dma_ == nullptr || !dma_->dma_read(address, entry)) {
            tracer_.log(TraceCategory::scsi, "{} request DMA failed at {:#x}", name_, address);
            requests_.pointer = pointer;
            continue;
        }
        execute_iocb(entry, pointer);
        requests_.pointer = pointer;
    }
    mailbox_out_[4] = requests_.pointer;
    update_interrupt();
}

void Isp1020::execute_iocb(std::span<const std::byte> entry, std::uint16_t& next) {
    const auto type = std::to_integer<std::uint8_t>(entry[0]);
    if (type == entry_marker) {
        tracer_.log(TraceCategory::scsi, "{} marker", name_);
        return;
    }
    std::array<std::byte, entry_size> status{};
    status[0] = std::byte{entry_status};
    status[1] = std::byte{1};
    std::copy_n(entry.begin() + 4, 4, status.begin() + 4); // handle
    const bool wide = type == entry_command_64;
    if (type != entry_command && !wide) {
        tracer_.log(TraceCategory::scsi, "{} unmodeled entry type {:#x}", name_, type);
        status[3] = std::byte{eflag_bad_header};
        post_response(status);
        return;
    }
    const unsigned lun = std::to_integer<unsigned>(entry[8]);
    const unsigned target_id = std::to_integer<unsigned>(entry[9]);
    const std::uint16_t cdb_length = std::min<std::uint16_t>(get16(entry, 10), 12);
    const std::uint16_t segment_count = get16(entry, 18);
    const unsigned entry_count = std::to_integer<unsigned>(entry[1]);
    // struct dataseg: base, count; A64: base, base_hi, count.
    std::vector<SegmentTransfer::Segment> segments;
    const auto add_segments = [&](std::span<const std::byte> bytes, std::size_t first,
                                  std::size_t count, bool high) {
        const std::size_t stride = high ? 12 : 8;
        for (std::size_t i = 0; i < count && segments.size() < segment_count; ++i) {
            const std::size_t at = first + stride * i;
            const std::uint64_t base =
                get32(bytes, at) | (high ? std::uint64_t{get32(bytes, at + 4)} << 32 : 0);
            segments.push_back({base, get32(bytes, at + (high ? 8 : 4))});
        }
    };
    if (wide) {
        add_segments(entry, 40, command_segments_64, true);
    } else {
        add_segments(entry, 32, command_segments, false);
    }
    // Continuation entries follow in the ring.
    std::array<std::byte, entry_size> continuation{};
    for (unsigned c = 1; c < entry_count; ++c) {
        const std::uint64_t address = requests_.address + next * entry_size;
        next = static_cast<std::uint16_t>((next + 1) % requests_.length);
        if (dma_ == nullptr || !dma_->dma_read(address, continuation)) {
            continue;
        }
        const auto kind = std::to_integer<std::uint8_t>(continuation[0]);
        if (kind == entry_continuation_64) {
            add_segments(continuation, 4, continuation_segments_64, true);
        } else if (kind == entry_continuation) {
            add_segments(continuation, 8, continuation_segments, false);
        }
    }
    const auto cdb = entry.subspan(20, cdb_length);
    tracer_.log(TraceCategory::scsi, "{} command target {} lun {} cdb {} segments {}", name_,
                target_id, lun, fmt_bytes(cdb), segments.size());

    scsi::Target* target = bus_ != nullptr ? bus_->target(target_id) : nullptr;
    std::uint16_t completion = cs_complete;
    std::uint16_t state = sf_got_bus;
    std::uint16_t scsi_status = 0;
    std::uint32_t residual = 0;
    std::uint16_t status_flags = 0;
    if (target == nullptr) {
        // Selection timeout. hypothesis: reported as INCOMPLETE with GOT_BUS in the state
        // flags and TIMEOUT in the status flags (IRIX reports INCOMPLETE without the timeout
        // flag as a hard error, where slate's boot shows none).
        completion = cs_incomplete;
        status_flags = stf_timeout;
    } else {
        SegmentTransfer data{dma_, std::move(segments)};
        const scsi::Status result = target->execute(lun, cdb, data);
        state |= sf_got_target | sf_sent_cdb | sf_got_status;
        scsi_status = static_cast<std::uint16_t>(result);
        if (data.transferred() != 0) {
            state |= sf_transferred_data;
        }
        residual = static_cast<std::uint32_t>(data.capacity() - data.transferred());
        if (data.failed()) {
            completion = cs_dma_error;
        } else if (residual != 0 && result == scsi::Status::good) {
            completion = cs_data_underrun;
        }
        if (result == scsi::Status::check_condition) {
            // Automatic REQUEST SENSE.
            const std::array<std::byte, 6> request_sense{std::byte{0x03},           {}, {}, {},
                                                         std::byte{sense_capacity}, {}};
            std::array<std::byte, sense_capacity> sense{};
            BufferTransfer sense_data{sense};
            if (target->execute(lun, request_sense, sense_data) == scsi::Status::good) {
                state |= sf_got_sense;
                put16(status, 18, static_cast<std::uint16_t>(sense_data.used()));
                std::ranges::copy(sense, status.begin() + 32);
            }
        }
    }
    put16(status, 8, scsi_status);
    put16(status, 10, completion);
    put16(status, 12, state);
    put16(status, 14, status_flags);
    put32(status, 20, residual);
    tracer_.log(TraceCategory::scsi, "{} status {:#x} completion {:#x} state {:#x} residual {}",
                name_, scsi_status, completion, state, residual);
    post_response(status);
}

void Isp1020::post_response(std::span<const std::byte> entry) {
    const std::uint64_t address = responses_.address + responses_.pointer * entry_size;
    if (dma_ == nullptr || !dma_->dma_write(address, entry)) {
        tracer_.log(TraceCategory::scsi, "{} response DMA failed at {:#x}", name_, address);
        return;
    }
    // hypothesis: a full response queue (the host's out-pointer in mailbox 5) is not modeled.
    responses_.pointer = static_cast<std::uint16_t>((responses_.pointer + 1) % responses_.length);
    mailbox_out_[5] = responses_.pointer;
    risc_interrupt_ = true;
}

bool Isp1020::dma_read_words(std::uint64_t pci_address, std::uint16_t ram_address,
                             std::uint32_t count) {
    std::vector<std::byte> bytes(std::size_t{count} * 2);
    if (dma_ == nullptr || !dma_->dma_read(pci_address, bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        ram_[static_cast<std::uint16_t>(ram_address + i)] = static_cast<std::uint16_t>(
            std::to_integer<unsigned>(bytes[std::size_t{2} * i]) |
            std::to_integer<unsigned>(bytes[std::size_t{2} * i + 1]) << 8);
    }
    return true;
}

bool Isp1020::dma_write_words(std::uint64_t pci_address, std::uint16_t ram_address,
                              std::uint32_t count) {
    std::vector<std::byte> bytes(std::size_t{count} * 2);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint16_t word = ram_[static_cast<std::uint16_t>(ram_address + i)];
        bytes[std::size_t{2} * i] = static_cast<std::byte>(word & 0xff);
        bytes[std::size_t{2} * i + 1] = static_cast<std::byte>(word >> 8);
    }
    return dma_ != nullptr && dma_->dma_write(pci_address, bytes);
}

void Isp1020::save_state(StateImage& image) const {
    const std::string& key = name_;
    config_.save_state(image, key + ".config");
    const std::uint16_t registers[] = {
        cfg1_,   interface_control_, risc_interrupt_,  host_interrupt_,
        paused_, semaphore_,         firmware_running_};
    image.put(key + ".registers", registers);
    image.put(key + ".mailbox_in", mailbox_in_);
    image.put(key + ".mailbox_out", mailbox_out_);
    const std::uint64_t queues[] = {firmware_start_,   requests_.length,  requests_.address,
                                    requests_.pointer, responses_.length, responses_.address,
                                    responses_.pointer};
    image.put(key + ".queues", queues);
    std::vector<std::uint32_t> parameters;
    for (const auto& [which, mailboxes] : parameters_) {
        parameters.push_back(which);
        parameters.insert(parameters.end(), mailboxes.begin(), mailboxes.end());
    }
    image.put_bytes(key + ".parameters", std::as_bytes(std::span{parameters}));
    image.put_sparse(key + ".ram", std::as_bytes(std::span{ram_}));
    std::vector<std::uint32_t> flat;
    for (const auto& [offset, value] : storage_) {
        flat.push_back(offset);
        flat.push_back(value);
    }
    image.put_bytes(key + ".storage", std::as_bytes(std::span{flat}));
}

void Isp1020::load_state(const StateImage& image) {
    const std::string& key = name_;
    config_.load_state(image, key + ".config");
    std::uint16_t registers[7]{};
    if (image.get(key + ".registers", registers)) {
        cfg1_ = registers[0];
        interface_control_ = registers[1];
        risc_interrupt_ = registers[2] != 0;
        host_interrupt_ = registers[3] != 0;
        paused_ = registers[4] != 0;
        semaphore_ = registers[5];
        firmware_running_ = registers[6] != 0;
    }
    image.get(key + ".mailbox_in", mailbox_in_);
    image.get(key + ".mailbox_out", mailbox_out_);
    std::uint64_t queues[7]{};
    if (image.get(key + ".queues", queues)) {
        firmware_start_ = static_cast<std::uint16_t>(queues[0]);
        requests_ = {static_cast<std::uint16_t>(queues[1]), queues[2],
                     static_cast<std::uint16_t>(queues[3])};
        responses_ = {static_cast<std::uint16_t>(queues[4]), queues[5],
                      static_cast<std::uint16_t>(queues[6])};
    }
    parameters_.clear();
    if (const auto bytes = image.get_bytes(key + ".parameters")) {
        std::vector<std::uint32_t> flat(bytes->size() / sizeof(std::uint32_t));
        std::memcpy(flat.data(), bytes->data(), flat.size() * sizeof(std::uint32_t));
        for (std::size_t i = 0; i + 9 <= flat.size(); i += 9) {
            auto& mailboxes = parameters_[flat[i]];
            for (std::size_t j = 0; j < 8; ++j) {
                mailboxes[j] = static_cast<std::uint16_t>(flat[i + 1 + j]);
            }
        }
    }
    (void)image.get_sparse(key + ".ram", std::as_writable_bytes(std::span{ram_}));
    if (const auto bytes = image.get_bytes(key + ".storage")) {
        std::vector<std::uint32_t> flat(bytes->size() / sizeof(std::uint32_t));
        std::memcpy(flat.data(), bytes->data(), flat.size() * sizeof(std::uint32_t));
        storage_.clear();
        for (std::size_t i = 0; i + 1 < flat.size(); i += 2) {
            storage_[flat[i]] = static_cast<std::uint16_t>(flat[i + 1]);
        }
    }
    update_interrupt();
}

} // namespace ultraviolent::devices
