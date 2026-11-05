#pragma once

#include <ultraviolent/arch/mips/block_interpreter.hpp>
#include <ultraviolent/arch/mips/coherence.hpp>
#include <ultraviolent/arch/mips/cpu.hpp>
#include <ultraviolent/arch/mips/interpreter.hpp>
#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/block_store.hpp>
#include <ultraviolent/core/ethernet_link.hpp>
#include <ultraviolent/core/invariant.hpp>
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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ultraviolent::ip27 {

// The module's chassis (IP27.adoc "Machine type"): the slot IDs its node slots give the node
// boards, which select the PROM's Crossbow tables.
enum class ModuleType : std::uint8_t {
    // A rack module: node slots n1-n4 (slot IDs 7-4) on two Crossbows; the BaseIO on port 8.
    rack,
    // A deskside module, as slate: node slots n1 and n2 (slot IDs 3 and 2) on one Crossbow,
    // ports 0xa and 9; the BaseIO on port 15.
    deskside,
};

// Configuration of the IP27 (IP27-TARGET.adoc; SMP.adoc). Values marked hypothesis in IP27.adoc
// are defaults, not constants.
// The node board's processors (SMP.adoc "S6: IP31 / R12000").
enum class ProcessorType : std::uint8_t {
    r10000,
    // slate's: R12000 revision 3.5 with an R12010 FPU of the same revision (OBS-SLATE-0004).
    r12000,
};

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
    // Which processor `cpu` identifies (r12000_identity gives slate's).
    ProcessorType processor{ProcessorType::r10000};
    // hypothesis: Hub 2.4 (IP27.adoc "Hub").
    unsigned hub_revision{Hub::default_revision};
    // The node board's module slot, n1-n4 (IP27.adoc "Machine type"); n1 like slate's first
    // node board. With two nodes they are n1 and n2.
    unsigned node_slot{1};
    // Deskside, slate's chassis (ADR-021, the target); a rack module is the other layout.
    ModuleType module{ModuleType::deskside};
    // Node boards: 1, or 2 in a deskside module (SMP.adoc "S5: the second node"). Each has
    // `memory_bytes` and `cpus`.
    unsigned nodes{1};
    // CPUs on the node board, from slice A: 1 or 2 (SMP.adoc; slate's node boards hold two).
    unsigned cpus{1};
    // With two CPUs, how far (in processor cycles) the one running may get ahead of the other
    // before they swap (ADR-023): 1 alternates every cycle; the default is 1 µs at 180 MHz.
    std::uint64_t cpu_slice{180};
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

// PRId and FPU implementation of slate's R12000s, revision 3.5 (hinv, OBS-SLATE-0004).
inline constexpr std::uint32_t r12000_identity = 0x0e35;

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

// The BaseIO board's Crossbow port, the module's io1 (IP27.adoc "Crossbow"): 8 in a rack
// module, 15 in a deskside (slate).
unsigned baseio_port(ModuleType module);

// The Crossbow port the Hub of node slot n`slot` is cabled to (IP27.adoc "Crossbow").
unsigned hub_xbow_port(ModuleType module, unsigned slot);

// The system controller NVRAM's contents at first power-on (IP27.adoc "System controller").
std::array<std::byte, devices::I2cEeprom::size> initial_controller_nvram(const Ip27Config& config);

// MD_SLOTID_USTAT slot ID of node slot n`slot` (IP27.adoc "Machine type").
unsigned node_slot_id(ModuleType module, unsigned slot);

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

    // One cycle of CPU A. Virtual time catches up with the CPU when an event is due or a device
    // is accessed (IP27.adoc "Machine loop"); run() leaves it current.
    void step();
    // Runs until every CPU has completed `cycles` more cycles. With two CPUs, they take turns
    // in slices (SMP.adoc "Time and scheduling"), and the run ends between slices: one CPU may
    // have run up to a slice further.
    void run(std::uint64_t cycles);
    // The engine run() uses (IR.adoc "Stages"; tier 0 by default since stage G; tier 1,
    // JIT.adoc): a host choice, not guest configuration, and not part of snapshots. Tracing and
    // PC probes always step the reference interpreter. Returns false, changing nothing, when
    // this host has no tier-1 backend.
    bool set_execution_engine(mips::ExecutionEngine engine) {
        for (Processor* p : processors_) {
            if (p == nullptr) {
                continue;
            }
            if (engine == mips::ExecutionEngine::tier1 && p->native == nullptr) {
                p->native = mips::make_native_code(p->cpu);
                if (p->native == nullptr) {
                    return false;
                }
            }
            p->blocks.set_native_code(engine == mips::ExecutionEngine::tier1 ? p->native.get()
                                                                             : nullptr);
        }
        engine_ = engine;
        return true;
    }
    [[nodiscard]] mips::ExecutionEngine execution_engine() const {
        return engine_;
    }
    // The first CPU's engine statistics.
    [[nodiscard]] const mips::BlockStatistics& block_statistics() const {
        return nodes_.front()->cpu_a.blocks.statistics();
    }
    // Tier 1's statistics for the first CPU, when it has been selected.
    [[nodiscard]] const mips::NativeCode* native_code() const {
        return nodes_.front()->cpu_a.native.get();
    }
    // Host-parallel CPUs (SMP.adoc "S4"): with several CPUs, run() runs each on its own host
    // thread, at most `window` cycles ahead of the slowest. Not deterministic; accepted
    // against reference mode by the workloads' results. A host choice, like the engine, and
    // not part of snapshots. PC probes and instruction tracing need reference mode: with
    // them run() takes turns as before.
    static constexpr std::uint64_t default_parallel_window = 10'000;
    void set_parallel(bool parallel, std::uint64_t window = default_parallel_window) {
        parallel_ = parallel;
        parallel_window_ = window;
    }
    // Tier 0's block cache size, a power of two (IR.adoc "Keeping blocks valid").
    void set_block_cache_entries(std::size_t entries) {
        for (Processor* p : processors_) {
            if (p != nullptr) {
                p->blocks.set_cache_entries(entries);
            }
        }
    }
    // Tier 1's translation threshold (BlockInterpreter::set_native_threshold).
    void set_native_threshold(std::uint64_t hits) {
        for (Processor* p : processors_) {
            if (p != nullptr) {
                p->blocks.set_native_threshold(hits);
            }
        }
    }
    // 4 KiB frames of memory and flash now marked as holding decoded code.
    [[nodiscard]] std::size_t marked_code_frames() const {
        std::size_t frames = 0;
        for (const auto& node : nodes_) {
            frames += node->memory.marked_frames() + node->flash.marked_frames();
        }
        return frames;
    }
    // Ends the current run() at the next cycle boundary (for host backends, such as a console
    // script that has run out).
    void request_stop() {
        if (parallel_running_) {
            parallel_stop_.store(true, std::memory_order_relaxed);
            return;
        }
        run_end_cycle_ = installed_[running_]->cpu.cycles();
        stop_cycle_ = run_end_cycle_;
        stop_requested_ = true;
    }

    // The module's keyswitch turned to OFF, as an operator stops the machine: the system
    // controller tells IRIX, which shuts down and has the controller remove power; the run
    // then ends (IP27.adoc "System controller"). Call it from an event.
    void turn_keyswitch_off() {
        elsc_.turn_keyswitch_off();
    }
    // Whether the system controller has removed power.
    [[nodiscard]] bool powered_off() const {
        return elsc_.powered_off();
    }

    // Host pacing (for interactive use, main's --realtime): the run's host threads wait until
    // host time `until` before they go on, each where it holds no lock (a CPU thread at the top
    // of its loop, parked). Usable from an event. Nothing the guest sees changes.
    void hold_host_until(std::chrono::steady_clock::time_point until) {
        host_hold_.store(until.time_since_epoch().count(), std::memory_order_relaxed);
    }

    // CPU `index` (which must be installed): node index * 2 + slice, so 0 is the first node's
    // slice A and 1 its slice B.
    [[nodiscard]] mips::Cpu& cpu(unsigned index = 0) {
        invariant(index < processors_.size() && processors_[index] != nullptr, "no such CPU");
        return processors_[index]->cpu;
    }
    [[nodiscard]] unsigned node_count() const {
        return static_cast<unsigned>(nodes_.size());
    }
    // A node's flash PROM contents, for host persistence of the PROM log (IP27.adoc "Boot
    // PROM"). load_flash replaces them before the machine runs.
    [[nodiscard]] std::span<const std::byte> flash_contents(unsigned node = 0) const {
        return nodes_.at(node)->flash.bytes();
    }
    [[nodiscard]] bool flash_dirty(unsigned node = 0) const {
        return nodes_.at(node)->flash_device.dirty();
    }
    void load_flash(std::span<const std::byte> contents, unsigned node = 0);
    // The BaseIO's own flash PROM (the IO6 PROM, behind the Bridge's external flash port),
    // loaded with a promgen image such as io6prom.img (at most 1 MB). Without it the port is
    // empty and the IP27 PROM uses its internal copy of the BASEIO PROM.
    void load_baseio_flash(std::span<const std::byte> image);
    // The BaseIO Ethernet port's cable: frames go to and come from `link`, and the PHY sees a
    // link partner. Without one the port is unplugged. The link must outlive the machine.
    void connect_ethernet(EthernetLink* link);
    // The timekeeper's battery-backed NVRAM (the PROM environment and the running clock), for
    // host persistence. load_nvram replaces it before the machine runs; with `resume_clock`
    // the clock continues from the time the contents hold, plus `seconds_off` (time on the
    // battery while the machine was off; devices::M48t35::load_contents).
    [[nodiscard]] std::span<const std::uint8_t> nvram_contents();
    void load_nvram(std::span<const std::uint8_t> contents, bool resume_clock,
                    std::int64_t seconds_off = 0);

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
    // Called when the guest ejects the disc (IRIX's eject); the drive is then empty.
    void on_cdrom_eject(std::function<void()> ejected) {
        cdrom_.on_eject(std::move(ejected));
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
    // The first node's view of physical addresses.
    [[nodiscard]] AddressSpace& bus() {
        return nodes_.front()->bus;
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
        // `hub`: the Hub whose register window this is; on host threads its PI_RT_COUNT and
        // PI_INT_PEND0/1 are read without the device lock.
        SynchronizedTarget(Ip27Machine& machine, MmioTarget& target, Hub* hub = nullptr)
            : machine_{machine}, target_{target}, hub_{hub} {}
        std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                            AccessWidth width) override;
        std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) override;

      private:
        Ip27Machine& machine_;
        MmioTarget& target_;
        Hub* hub_;
        // One CPU's PI_RT_COUNT reads (JIT.adoc "Device time"): the count, remembered for the
        // cycles before it changes, while the counter's base and epoch and device time are
        // those it was computed from. Reads at those cycles return the same count; the
        // delay loops read many times per tick.
        struct RtReads {
            std::uint64_t from{1};
            std::uint64_t until{0};
            std::uint64_t base{};
            std::uint64_t epoch{};
            VirtualTime now{};
            std::uint64_t count{};
        };
        RtReads rt_reads_;
    };

    // A crosstalk small window as one CPU slice of a node sees it: an access that no widget
    // answers is a PIO error, which the requesting CPU's own Hub records whichever node the
    // address reaches (Hub::record_pio_error; IP27.adoc "PIO errors").
    class PioErrorWindow final : public MmioTarget {
      public:
        PioErrorWindow(MmioTarget& target, Hub& requester, unsigned slice, std::uint64_t base)
            : target_{target}, requester_{requester}, slice_{slice}, base_{base} {}
        std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                            AccessWidth width) override;
        std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) override;

      private:
        MmioTarget& target_;
        Hub& requester_;
        unsigned slice_;
        std::uint64_t base_;
    };

    struct Processor;
    // The wires from a Hub's interrupt outputs to its CPU. The machine owns the wiring: in
    // reference mode they reach the CPU directly; with CPUs on host threads, the levels are
    // posted to the CPU's mailbox (mips::Cpu::post_interrupt_levels), and set at once too when
    // the CPU is the caller's own.
    class CpuPins final : public InterruptSink {
      public:
        CpuPins(Ip27Machine& machine, Processor& processor)
            : machine_{machine}, processor_{processor} {}
        void set_interrupt_level(std::uint32_t input, bool asserted) override;
        // Before CPUs run on threads: the levels the CPU has now.
        void set_levels(std::uint32_t levels) {
            levels_ = levels;
        }

      private:
        Ip27Machine& machine_;
        Processor& processor_;
        std::uint32_t levels_{};
    };

    // A CPU in a slice of the Hub, and its execution engines.
    struct Processor {
        Processor(Ip27Machine& machine, AddressSpace& bus, Tracer& tracer,
                  const mips::ProcessorModel& model, unsigned slice)
            : cpu{bus, tracer, model, slice}, pins{machine, *this} {}
        mips::Cpu cpu;
        mips::Interpreter interpreter{cpu};
        mips::BlockInterpreter blocks{cpu};
        std::unique_ptr<mips::NativeCode> native;
        CpuPins pins;
        // On host threads: the cycle count where its engine last returned, for the others'
        // window.
        std::atomic<std::uint64_t> published{};
    };

    // A lock for very short critical sections between host threads: device accesses.
    class SpinLock {
      public:
        void lock() {
            while (locked_.exchange(true, std::memory_order_acquire)) {
                while (locked_.load(std::memory_order_relaxed)) {
#if defined(__x86_64__)
                    __builtin_ia32_pause();
#endif
                }
            }
        }
        void unlock() {
            locked_.store(false, std::memory_order_release);
        }

      private:
        std::atomic<bool> locked_{};
    };

    // Advances the scheduler to virtual time, the time of the CPU furthest behind (SMP.adoc
    // "Time and scheduling"), and recomputes when the next event is due.
    void synchronize_time();
    void update_next_event();
    // Completed cycles of the CPU furthest behind: virtual time in CPU cycles.
    [[nodiscard]] std::uint64_t lowest_cycles() const {
        // Runs on every device access: kept small enough to inline for one and two CPUs.
        if (cpu_count_ == 1) [[likely]] {
            return installed_[0]->cpu.cycles();
        }
        if (cpu_count_ == 2) {
            return std::min(installed_[0]->cpu.cycles(), installed_[1]->cpu.cycles());
        }
        return lowest_of_several();
    }
    [[nodiscard]] std::uint64_t lowest_of_several() const;
    // Completed cycles of the CPU furthest behind other than the running one.
    [[nodiscard, gnu::always_inline]] std::uint64_t lowest_other_cycles() const {
        if (cpu_count_ == 2) {
            return installed_[1 - running_]->cpu.cycles();
        }
        std::uint64_t lowest = std::numeric_limits<std::uint64_t>::max();
        for (unsigned i = 0; i < cpu_count_; ++i) {
            if (i != running_) {
                lowest = std::min(lowest, installed_[i]->cpu.cycles());
            }
        }
        return lowest;
    }
    // Host-parallel CPUs (SMP.adoc "S4"): runs every CPU on its own thread until each has
    // reached `end` or a stop.
    void run_parallel(std::uint64_t cycles);
    void run_thread(Processor& processor, unsigned index, std::uint64_t end);
    // The lowest cycle count the CPUs other than installed_[index] have published.
    [[nodiscard]] std::uint64_t lowest_published_other(unsigned index) const;
    // On host threads, under the device lock: device time moves to the CPU's time if that is
    // later, and the events due by then run; then the next event's cycle is published.
    void advance_device_time(std::uint64_t cycles);
    // hold_host_until's time while it is still ahead of the host's clock.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> host_hold() const;
    void publish_next_event();
    // A device access by the calling thread's CPU, under the device lock.
    template <typename Access> auto locked_access(Access access);
    // Runs installed_[running_] up to stop_cycle_.
    void run_processor(bool observed);
    void step(Processor& processor);
    void run_many(std::uint64_t cycles);
    // Rebuilds every node's view of physical addresses (Node::map_system_windows).
    void map_views();
    // How many CPUs are installed.
    [[nodiscard]] unsigned cpu_count() const {
        return cpu_count_;
    }

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

    // Host threads: where a node's CPUs meet to leave a reset together (mips::ResetRendezvous).
    class NodeReset final : public mips::ResetRendezvous {
      public:
        explicit NodeReset(Ip27Machine& machine) : machine_{machine} {}
        // Adds one of the node's CPUs, which every reset of the node resets.
        void add(mips::Cpu& cpu);
        // Under the device lock: posts a `kind` reset to the node's CPUs. A reset asked for
        // while a round is open (posted, not every CPU arrived) joins it, so every CPU takes
        // the same rounds (observed: a second request reached one CPU after the other had taken
        // the first, and it then waited for a round that never came).
        void post(ResetKind kind);
        std::uint64_t arrive(mips::Cpu& cpu, std::uint64_t cycles) override;
        // After the threads, when nothing waits: forgets a round a stop left open.
        void clear();

      private:
        Ip27Machine& machine_;
        std::array<mips::Cpu*, 2> cpus_{};
        unsigned participants_{};
        std::atomic<bool> open_{};
        std::atomic<unsigned> arrived_{};
        std::atomic<std::uint64_t> latest_{};
        std::atomic<std::uint64_t> round_{};
        std::atomic<std::uint64_t> released_{};
    };

    // A node board: its Hub, memory, boot PROM, and CPUs, and the node's view of physical
    // addresses (SMP.adoc "S5: the second node").
    struct Node {
        Node(Ip27Machine& machine, unsigned index, unsigned slot, std::span<const std::byte> prom);
        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;

        // Rebuilds the node's view from the node IDs.
        void map_system_windows();
        void map_aliases();
        // `node`'s memory, PROM, directory, Hub registers, and crosstalk windows at `base`.
        void map_resources(Node& node, std::uint64_t base);
        void map_flash(std::uint64_t address);
        void flash_mode_changed();
        void reset_cpus(ResetKind kind);
        // The node's own components, under their usual field names.
        void save_state(StateImage& image) const;
        [[nodiscard]] bool load_state(const StateImage& image);

        Ip27Machine& machine;
        unsigned index;
        AddressSpace bus{ByteOrder::big};
        MemoryBlock memory;
        MemoryBlock flash;
        devices::Am29f080 flash_device;
        FlashInterface flash_interface;
        FlashCommands flash_commands;
        // The Hub's PCF8584 on the module's I2C bus (IP27.adoc "Junk bus").
        devices::Pcf8584 i2c;
        DirectoryMemory directory;
        // The node board's MicroLAN (Hub MD_MLAN_CTL) and the NICs on it (IP27.adoc "NICs"):
        // the node board's own, and in n1 of a deskside module the module's.
        devices::OneWireBus mlan;
        devices::Nic board_nic;
        Hub hub;
        NodeReset reset_rendezvous;
        Processor cpu_a;
        std::optional<Processor> cpu_b;
        SynchronizedTarget synchronized_hub;
        SynchronizedTarget synchronized_hub_b;
        SynchronizedTarget synchronized_flash;
        SynchronizedTarget synchronized_flash_commands;
        std::array<std::optional<SynchronizedTarget>, 7> big_windows;
        // Every node's crosstalk small windows as this node's CPUs see them, by target node and
        // window base, per slice. Kept once made: an address map's older versions may still
        // point at them (AddressSpace::set_shared).
        struct PioWindows {
            std::array<std::unique_ptr<PioErrorWindow>, 2> windows;
            std::array<std::unique_ptr<SynchronizedTarget>, 2> synchronized;
        };
        std::map<std::pair<unsigned, std::uint64_t>, PioWindows> pio_windows;
        AddressSpace::Ports pio_ports(Node& node, unsigned widget, std::uint64_t base);
    };

    Ip27Config config_;
    // Cycle counts to virtual time for the CPU clock, on every synchronized access.
    CycleTime cpu_time_{config_.cpu_clock};
    // FNV-1a hash of the flash contents, to match snapshots with their PROM.
    std::uint64_t flash_hash_{};
    std::vector<std::uint64_t> pc_probes_;
    VirtualClock clock_;
    Tracer tracer_{clock_};
    Scheduler scheduler_{clock_, tracer_};
    // The BaseIO board's flash PROM (IP27.adoc "Bridge"): erased until an image is loaded.
    MemoryBlock baseio_flash_{devices::Am29f080::size};
    devices::Am29f080 baseio_flash_device_{
        scheduler_, tracer_, baseio_flash_.bytes(), {}, "baseio.flash"};
    // The module's I2C bus: each node's PCF8584, the system controller, and its NVRAM.
    devices::I2cBus i2c_bus_;
    devices::I2cEeprom controller_nvram_;
    devices::Elsc elsc_{scheduler_, tracer_, i2c_bus_};
    // LL/SC and host-page coherence between the CPUs (unused with one).
    mips::Coherence coherence_;
    std::vector<std::unique_ptr<Node>> nodes_;
    // Indexed node * 2 + slice; null where no CPU is installed.
    std::array<Processor*, 4> processors_{};
    // The installed CPUs in index order, the first cpu_count_ entries.
    std::array<Processor*, 4> installed_{};
    unsigned cpu_count_{};
    // The CPU run() is running, an index into installed_.
    unsigned running_{};
    // request_stop() was called during this run.
    bool stop_requested_{};
    // Where the running CPU must yield to the other (cpu_slice ahead of it).
    std::uint64_t slice_end_cycle_{std::numeric_limits<std::uint64_t>::max()};
    mips::ExecutionEngine engine_{mips::ExecutionEngine::tier0};
    // The module's crossbow and the Hub's crosstalk windows (IP27.adoc "Xtalk").
    devices::Xbow xbow_{tracer_};
    // The BaseIO board's Bridge, on Crossbow port `baseio_port`.
    devices::Bridge baseio_bridge_{tracer_, baseio_port(config_.module)};
    // The BaseIO board's NICs on the Bridge's NIC bus: the MIO daughterboard and the BASEIO
    // board, DS2502 parts (family 0x09) with synthetic laser serial numbers and slate's
    // manufacturing records (IP27.adoc "NICs", OBS-SLATE-0004).
    devices::OneWireBus baseio_nic_bus_;
    devices::Nic mio_nic_{0x09, 0x1};
    devices::Nic baseio_nic_{0x09, 0x2};
    // A deskside module's MODULEID and midplane NICs, on n1's MicroLAN (as slate's).
    devices::Nic module_nic_{0x09, 0x3};
    devices::Nic midplane_nic_{0x09, 0x4};
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
    // Host-parallel CPUs (SMP.adoc "S4"). parallel_running_ changes only while no CPU thread
    // runs.
    bool parallel_{};
    std::uint64_t parallel_window_{default_parallel_window};
    bool parallel_running_{};
    SpinLock device_lock_;
    std::atomic<std::uint64_t> parallel_next_event_{};
    std::atomic<bool> parallel_stop_{};
    // hold_host_until's time, in steady_clock ticks; 0 when the run was never held.
    std::atomic<std::chrono::steady_clock::rep> host_hold_{};
    // CPU threads that have finished their run.
    std::atomic<unsigned> parallel_finished_{};
    // The CPU the calling host thread runs, while CPUs run on threads.
    static thread_local Processor* current_;
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
