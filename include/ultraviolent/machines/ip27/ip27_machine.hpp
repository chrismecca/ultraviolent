#pragma once

#include <ultraviolent/arch/mips/block_interpreter.hpp>
#include <ultraviolent/arch/mips/cpu.hpp>
#include <ultraviolent/arch/mips/interpreter.hpp>
#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/block_store.hpp>
#include <ultraviolent/core/ethernet_link.hpp>
#include <ultraviolent/core/memory_block.hpp>
#include <ultraviolent/core/reset.hpp>
#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/core/virtual_time.hpp>
#include <ultraviolent/devices/am29f080.hpp>
#include <ultraviolent/devices/bridge.hpp>
#include <ultraviolent/devices/dp83840.hpp>
#include <ultraviolent/devices/elsc.hpp>
#include <ultraviolent/devices/i2c.hpp>
#include <ultraviolent/devices/i2c_eeprom.hpp>
#include <ultraviolent/devices/ioc3.hpp>
#include <ultraviolent/devices/isp1020.hpp>
#include <ultraviolent/devices/m48t35.hpp>
#include <ultraviolent/devices/pcf8584.hpp>
#include <ultraviolent/devices/xbow.hpp>
#include <ultraviolent/machines/ip27/directory_memory.hpp>
#include <ultraviolent/machines/ip27/hub.hpp>
#include <ultraviolent/scsi/cdrom.hpp>
#include <ultraviolent/scsi/disk.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ultraviolent::ip27 {

// Configuration of the one-node, one-CPU IP27 (IP27-TARGET.adoc). Values marked hypothesis in
// IP27.adoc are defaults, not constants.
struct Ip27Config {
    // Installed memory, filling 512 MB banks from bank 0; a final partial bank must be a power
    // of two of at least 8 MB (memory_bank_sizes).
    std::uint64_t memory_bytes{512ull << 20};
    // hypothesis: standard directory memory, the configuration that needs no directory DIMMs
    // (PRM 1.4; IP27.adoc "Memory and directory").
    DirectoryDimms directory{DirectoryDimms::standard};
    // observed: the user's PROM describes a 180 MHz processor (IP27.adoc "Reset mode bits").
    Frequency cpu_clock{180'000'000};
    // Identity registers. `cpu.config` is used only when the PROM has no "ip27conf" block;
    // otherwise the reset Config comes from the flash (reset_config_word).
    // hypothesis: R10000 revision 3.4, a late production part, for the CPU and FPU (observed:
    // IRIX warns "CPU 0 ... is downrev (900)" for revision 0.0; slate's R12000s report the
    // same revision for CPU and FPU).
    mips::CpuConfig cpu{.processor_id = 0x0934, .config = 0x6c01'a603, .fpu_id = 0x0934};
    // hypothesis: Hub 2.4 (IP27.adoc "Hub").
    unsigned hub_revision{Hub::default_revision};
    // The node board's module slot, n1-n4 (IP27.adoc "Machine type"); n1 like slate's first
    // node board.
    unsigned node_slot{1};
    // The module number in the system controller's NVRAM (1-255; 0 means unassigned). slate
    // is module 1.
    std::uint8_t module_number{1};
    // The BaseIO Ethernet address, stored in the IOC3's NIC. hypothesis: SGI's OUI 08:00:69 and
    // an arbitrary suffix; not slate's address.
    std::array<std::uint8_t, 6> ethernet_address{0x08, 0x00, 0x69, 0x0e, 0x17, 0x01};
    // The time-of-day clock's reading at virtual time zero, in Unix seconds. Fixed so runs
    // are deterministic: 2026-01-01 00:00:00 UTC.
    std::int64_t clock_epoch{1'767'225'600};
};

// The IOC3 NIC's memory for an Ethernet address (IP27.adoc "NICs").
std::array<std::uint8_t, devices::Nic::memory_size>
ethernet_nic_memory(const std::array<std::uint8_t, 6>& address);

// A board's manufacturing record, as SGI programs it into the board's NIC.
struct BoardIdentity {
    std::string_view serial;   // the barcode, up to 10 characters
    std::string_view part;     // up to 25 characters, such as "030-0734-003"
    std::string_view revision; // up to 4 characters
    std::string_view name;     // up to 14 characters, such as "BASEIO"
    std::uint8_t group{0xff};
    std::uint32_t capability{0xffff'ffff};
    std::uint8_t variety{0xff};
};

// The NIC memory holding `board`'s manufacturing record (IP27.adoc "NICs"): pages 0 and 1.
std::array<std::uint8_t, devices::Nic::memory_size> board_nic_memory(const BoardIdentity& board);

// The BaseIO board's Crossbow port: io1 of a rack module, the PROM's slot table for the node
// slots this machine offers (IP27.adoc "Crossbow"). Slate, a deskside, has it on port 15.
inline constexpr unsigned baseio_port = 0x8;

// The Crossbow port the Hub of node slot n`slot` is cabled to (IP27.adoc "Crossbow").
unsigned hub_xbow_port(unsigned slot);

// The system controller NVRAM's contents at first power-on (IP27.adoc "System controller").
std::array<std::byte, devices::I2cEeprom::size> initial_controller_nvram(const Ip27Config& config);

// MD_SLOTID_USTAT slot ID of node slot n`slot` (IP27.adoc "Machine type").
unsigned node_slot_id(unsigned slot);

// The installed size of each memory bank for `memory_bytes`, or nothing when it cannot be
// populated that way.
std::optional<std::array<std::uint64_t, memory_banks>>
memory_bank_sizes(std::uint64_t memory_bytes);

// The CPU's Config register at reset: the flash mode word when present (bits 24:0), with the
// hardwired 32 KB primary cache fields (UM 14.14, 8.5); otherwise `config.cpu.config`.
std::uint32_t reset_config_word(const Ip27Config& config, std::span<const std::byte> flash);

// Host configuration errors, reported before a machine is built (ENGINEERING "Errors").
std::expected<void, std::string> validate(const Ip27Config& config,
                                          std::span<const std::byte> prom);

// The IP27 machine personality: owns the node's components and their wiring (ARCHITECTURE
// "Machine personality"). Decoding follows IP27.adoc.
class Ip27Machine {
  public:
    // validate(config, prom) must hold. `prom` is the flash content from decode_prom_image.
    Ip27Machine(const Ip27Config& config, std::span<const std::byte> prom);
    Ip27Machine(const Ip27Machine&) = delete;
    Ip27Machine& operator=(const Ip27Machine&) = delete;

    void reset(ResetKind kind);

    // Snapshots (IP27.adoc "Snapshots"): the whole machine state at a cycle boundary. Loading
    // requires a machine built from the same configuration and PROM that has not run yet.
    [[nodiscard]] StateImage save_state();
    std::expected<void, std::string> load_state(const StateImage& image);

    // Diagnostic: trace a0-a3, ra, and v0 under `cpu` whenever execution reaches `pc`. Pure
    // observation; it never changes behavior.
    void add_pc_probe(std::uint64_t pc) {
        pc_probes_.push_back(pc);
    }

    // One CPU cycle. Virtual time catches up with the CPU when an event is due or a device is
    // accessed (IP27.adoc "Machine loop"); run() leaves it current.
    void step();
    void run(std::uint64_t cycles);
    // The engine run() uses (IR.adoc "Stages"): a host choice, not guest configuration, and
    // not part of snapshots. Tracing and PC probes always step the reference interpreter.
    void set_execution_engine(mips::ExecutionEngine engine) {
        engine_ = engine;
    }
    [[nodiscard]] const mips::BlockStatistics& block_statistics() const {
        return block_interpreter_.statistics();
    }
    // Tier 0's block cache size, a power of two (IR.adoc "Keeping blocks valid").
    void set_block_cache_entries(std::size_t entries) {
        block_interpreter_.set_cache_entries(entries);
    }
    // 4 KiB frames of memory and flash now marked as holding decoded code.
    [[nodiscard]] std::size_t marked_code_frames() const {
        return memory_.marked_frames() + flash_.marked_frames();
    }
    // Ends the current run() at the next cycle boundary (for host backends, such as a console
    // script that has run out).
    void request_stop() {
        run_end_cycle_ = cpu_.cycles();
        stop_cycle_ = run_end_cycle_;
    }

    [[nodiscard]] mips::Cpu& cpu() {
        return cpu_;
    }
    // The flash PROM's contents, for host persistence of the PROM log (IP27.adoc "Boot PROM").
    // load_flash replaces them before the machine runs.
    [[nodiscard]] std::span<const std::byte> flash_contents() const {
        return flash_.bytes();
    }
    [[nodiscard]] bool flash_dirty() const {
        return flash_device_.dirty();
    }
    void load_flash(std::span<const std::byte> contents);
    // The BaseIO's own flash PROM (the IO6 PROM, behind the Bridge's external flash port),
    // loaded with a promgen image such as io6prom.img (at most 1 MB). Without it the port is
    // empty and the IP27 PROM uses its internal copy of the BASEIO PROM.
    void load_baseio_flash(std::span<const std::byte> image);
    // The BaseIO Ethernet port's cable: frames go to and come from `link`, and the PHY sees a
    // link partner. Without one the port is unplugged. The link must outlive the machine.
    void connect_ethernet(EthernetLink* link);
    // The timekeeper's battery-backed NVRAM (the PROM environment and the running clock), for
    // host persistence. load_nvram replaces it before the machine runs; with `resume_clock`
    // the clock continues from the time the contents hold.
    [[nodiscard]] std::span<const std::uint8_t> nvram_contents();
    void load_nvram(std::span<const std::uint8_t> contents, bool resume_clock);

    // The console serial port (the BaseIO IOC3's port A) sends its output to `sink`.
    void connect_console(devices::SerialSink& sink) {
        ioc3_.uart_a().connect(sink);
    }
    void connect_console_input(devices::SerialSource& source) {
        ioc3_.uart_a().connect(source);
    }
    // The disc in the CD-ROM drive (SCSI bus 0, ID 6), or none. The store must outlive the
    // machine or be removed first.
    void insert_cdrom(BlockStore* disc) {
        cdrom_.insert(disc);
    }
    // The system disk (SCSI bus 0, ID 1) on `store`, which must outlive the machine. Call
    // before running.
    void attach_disk(BlockStore& store);

    [[nodiscard]] Tracer& tracer() {
        return tracer_;
    }
    [[nodiscard]] Scheduler& scheduler() {
        return scheduler_;
    }
    [[nodiscard]] AddressSpace& bus() {
        return bus_;
    }
    // The virtual clock, current within device events (it lags the CPU between them).
    [[nodiscard]] const VirtualClock& clock() const {
        return clock_;
    }
    [[nodiscard]] VirtualTime now() const {
        return clock_.now();
    }

  private:
    // A device window whose accesses first bring virtual time up to date, so the device sees
    // the time at the start of the accessing instruction (IP27.adoc "Machine loop").
    class SynchronizedTarget final : public MmioTarget {
      public:
        SynchronizedTarget(Ip27Machine& machine, MmioTarget& target)
            : machine_{machine}, target_{target} {}
        std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                            AccessWidth width) override;
        std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) override;

      private:
        Ip27Machine& machine_;
        MmioTarget& target_;
    };

    // Advances the scheduler to the CPU's completed cycles and recomputes when the next event
    // is due.
    void synchronize_time();
    void update_next_event();
    void map_system_windows();
    void flash_mode_changed();

    // The Hub's flash PROM interface (IP27.adoc "Boot PROM").
    class FlashInterface final : public MmioTarget {
      public:
        FlashInterface(devices::Am29f080& device, Tracer& trace) : flash{device}, tracer{trace} {}
        std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                            AccessWidth width) override;
        std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) override;
        devices::Am29f080& flash;
        Tracer& tracer;
    };
    // The part of the command range above the first megabyte: writes only.
    class FlashCommands final : public MmioTarget {
      public:
        explicit FlashCommands(FlashInterface& target) : interface{target} {}
        std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                            AccessWidth width) override;
        std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) override;
        FlashInterface& interface;
    };

    Ip27Config config_;
    // FNV-1a hash of the flash contents, to match snapshots with their PROM.
    std::uint64_t flash_hash_{};
    std::vector<std::uint64_t> pc_probes_;
    VirtualClock clock_;
    Tracer tracer_{clock_};
    Scheduler scheduler_{clock_, tracer_};
    AddressSpace bus_{ByteOrder::big};
    MemoryBlock memory_;
    MemoryBlock flash_;
    devices::Am29f080 flash_device_{scheduler_, tracer_, flash_.bytes(),
                                    [this] { flash_mode_changed(); }};
    FlashInterface flash_interface_{flash_device_, tracer_};
    FlashCommands flash_commands_{flash_interface_};
    // The BaseIO board's flash PROM (IP27.adoc "Bridge"): erased until an image is loaded.
    MemoryBlock baseio_flash_{devices::Am29f080::size};
    devices::Am29f080 baseio_flash_device_{
        scheduler_, tracer_, baseio_flash_.bytes(), {}, "baseio.flash"};
    // The junk-bus I2C bus: the Hub's PCF8584, the system controller, and its NVRAM.
    devices::I2cBus i2c_bus_;
    devices::Pcf8584 i2c_{scheduler_, tracer_, i2c_bus_};
    devices::I2cEeprom controller_nvram_;
    devices::Elsc elsc_{scheduler_, tracer_, i2c_bus_};
    DirectoryMemory directory_;
    Hub hub_{scheduler_,
             tracer_,
             config_.hub_revision,
             node_slot_id(config_.node_slot),
             i2c_,
             directory_,
             bus_,
             [this] { cpu_.reset(ResetKind::cold); }};
    mips::Cpu cpu_;
    mips::Interpreter interpreter_{cpu_};
    mips::BlockInterpreter block_interpreter_{cpu_};
    mips::ExecutionEngine engine_{mips::ExecutionEngine::reference};
    SynchronizedTarget synchronized_hub_{*this, hub_};
    SynchronizedTarget synchronized_flash_{*this, flash_interface_};
    SynchronizedTarget synchronized_flash_commands_{*this, flash_commands_};
    // The module's crossbow and the Hub's crosstalk windows (IP27.adoc "Xtalk").
    devices::Xbow xbow_{tracer_};
    // The BaseIO board's Bridge, on Crossbow port `baseio_port`.
    devices::Bridge baseio_bridge_{tracer_, baseio_port};
    // The BaseIO board's NICs on the Bridge's NIC bus: the MIO daughterboard and the BASEIO
    // board, DS2502 parts (family 0x09) with synthetic laser serial numbers and slate's
    // manufacturing records (IP27.adoc "NICs", OBS-SLATE-0004).
    devices::OneWireBus baseio_nic_bus_;
    devices::Nic mio_nic_{0x09, 0x1};
    devices::Nic baseio_nic_{0x09, 0x2};
    // BaseIO PCI devices, in slate's slots (OBS-SLATE-0001): SCSI controllers 0 and 1, IOC3.
    // The BaseIO's two SCSI buses and their QLogic controllers (slate: the CD-ROM is ID 6 on
    // bus 0, OBS-SLATE-0001).
    scsi::Bus scsi_bus0_;
    scsi::Bus scsi_bus1_;
    scsi::CdRom cdrom_{tracer_, "cdrom"};
    std::optional<scsi::Disk> system_disk_;
    devices::Isp1020 scsi0_{scheduler_, tracer_, "scsi0"};
    devices::Isp1020 scsi1_{scheduler_, tracer_, "scsi1"};
    // The IP27's software counts the timekeeper's year from 1970 (Linux
    // arch/mips/sgi-ip27/ip27-timer.c, drivers/rtc/rtc-m48t35.c; IRIX wrote 0x36 for 2006).
    devices::M48t35 timekeeper_{scheduler_, config_.clock_epoch, 1970};
    devices::Ioc3 ioc3_{scheduler_, tracer_};
    // The BaseIO's Ethernet PHY on the IOC3's MII bus (IP27.adoc "IOC3 Ethernet").
    devices::Dp83840 ethernet_phy_{scheduler_, tracer_, "phy"};
    devices::OneWireBus ioc3_nic_bus_;
    // hypothesis: the NIC's serial number is the Ethernet address's low 24 bits.
    devices::Nic ethernet_nic_{0x91, std::uint64_t{config_.ethernet_address[3]} << 16 |
                                         std::uint64_t{config_.ethernet_address[4]} << 8 |
                                         config_.ethernet_address[5]};
    std::array<std::optional<SynchronizedTarget>, xtalk::widget_count> xtalk_windows_;
    std::array<std::optional<SynchronizedTarget>, 7> big_windows_;
    // First CPU cycle count at which a pending event is due.
    std::uint64_t next_event_cycle_{};
    // Where run() stops; the interpreter runs uninterrupted up to the lesser of the two.
    std::uint64_t run_end_cycle_{};
    std::uint64_t stop_cycle_{};
    // The last event deadline converted to a cycle count, and the result.
    std::optional<VirtualTime> converted_deadline_;
    std::uint64_t converted_deadline_cycle_{};
    // Scheduler invariant (IR.adoc): events run between calls into the execution engine or
    // inside a synchronized device access, never otherwise while an instruction executes.
    bool executing_{};
    unsigned synchronized_accesses_{};
};

} // namespace ultraviolent::ip27
