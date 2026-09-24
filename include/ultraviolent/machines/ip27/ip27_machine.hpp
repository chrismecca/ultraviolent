#pragma once

#include <ultraviolent/arch/mips/cpu.hpp>
#include <ultraviolent/arch/mips/interpreter.hpp>
#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/memory_block.hpp>
#include <ultraviolent/core/reset.hpp>
#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/core/virtual_time.hpp>
#include <ultraviolent/devices/pcf8584.hpp>
#include <ultraviolent/machines/ip27/hub.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace ultraviolent::ip27 {

// Configuration of the one-node, one-CPU IP27 (IP27-TARGET.adoc). Values marked hypothesis in
// IP27.adoc are defaults, not constants.
struct Ip27Config {
    std::uint64_t memory_bytes{512ull << 20};
    // observed: the user's PROM describes a 180 MHz processor (IP27.adoc "Reset mode bits").
    Frequency cpu_clock{180'000'000};
    // Identity registers. `cpu.config` is used only when the PROM has no "ip27conf" block;
    // otherwise the reset Config comes from the flash (reset_config_word).
    mips::CpuConfig cpu{.processor_id = 0x0900, .config = 0x6c01'a603, .fpu_id = 0x0900};
    // hypothesis: Hub 2.4 (IP27.adoc "Hub").
    unsigned hub_revision{Hub::default_revision};
};

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

    // Diagnostic: trace a0-a3, ra, and v0 under `cpu` whenever execution reaches `pc`. Pure
    // observation; it never changes behavior.
    void add_pc_probe(std::uint64_t pc) {
        pc_probes_.push_back(pc);
    }

    // One CPU cycle, then virtual time catches up with the CPU (IP27.adoc "Machine loop").
    void step();
    void run(std::uint64_t cycles);

    [[nodiscard]] mips::Cpu& cpu() {
        return cpu_;
    }
    [[nodiscard]] Tracer& tracer() {
        return tracer_;
    }
    [[nodiscard]] Scheduler& scheduler() {
        return scheduler_;
    }
    [[nodiscard]] AddressSpace& bus() {
        return bus_;
    }
    [[nodiscard]] VirtualTime now() const {
        return clock_.now();
    }

  private:
    void map_system_windows();

    Ip27Config config_;
    std::vector<std::uint64_t> pc_probes_;
    VirtualClock clock_;
    Tracer tracer_{clock_};
    Scheduler scheduler_{clock_, tracer_};
    AddressSpace bus_{ByteOrder::big};
    MemoryBlock memory_;
    MemoryBlock flash_;
    devices::Pcf8584 i2c_{scheduler_, tracer_};
    Hub hub_{scheduler_, tracer_, config_.hub_revision, i2c_,
             [this] { cpu_.reset(ResetKind::cold); }};
    mips::Cpu cpu_;
    mips::Interpreter interpreter_{cpu_};
};

} // namespace ultraviolent::ip27
