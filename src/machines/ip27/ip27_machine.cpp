#include <ultraviolent/machines/ip27/ip27_machine.hpp>

#include <ultraviolent/core/invariant.hpp>
#include <ultraviolent/machines/ip27/prom_image.hpp>

#include <algorithm>
#include <format>

namespace ultraviolent::ip27 {

namespace {

// IP27.adoc "Node address space".
constexpr std::uint64_t node_space = std::uint64_t{1} << 32;
constexpr std::uint64_t ualias_size = 0x1000'0000;
constexpr std::uint64_t lboot_base = 0x1000'0000;
// The reset vector's physical address: LBOOT offset 0x0fc00000 (IP27.adoc "Boot PROM").
constexpr std::uint64_t flash_base = 0x1fc0'0000;
constexpr unsigned hspec_attribute = 0;
constexpr unsigned io_attribute = 1;
constexpr unsigned uncac_attribute = 3;
// IALIAS: the local Hub's registers at the start of widget 1's 16 MB I/O window.
constexpr std::uint64_t hub_widget = 1;
constexpr std::uint64_t small_window_shift = 24;

void must_map(std::expected<void, MapError> result, const char* what) {
    invariant(result.has_value(), what);
}

} // namespace

std::uint32_t reset_config_word(const Ip27Config& config, std::span<const std::byte> flash) {
    // Config bits 31:29 (IC) and 28:26 (DC) are hardwired to 32 KB; bits 24:0 are the mode
    // bits latched at reset (UM 14.14, 8.5).
    constexpr std::uint32_t hardwired_caches = (3u << 29) | (3u << 26);
    if (const auto mode = prom_mode_bits(flash)) {
        return hardwired_caches | (*mode & 0x01ff'ffffu);
    }
    return config.cpu.config;
}

std::expected<void, std::string> validate(const Ip27Config& config,
                                          std::span<const std::byte> prom) {
    if (prom.empty() || prom.size() > prom_code_size) {
        return std::unexpected("PROM image does not fit the flash code area");
    }
    if (config.memory_bytes == 0 || config.memory_bytes % (1u << 20) != 0 ||
        config.memory_bytes > node_space) {
        return std::unexpected(std::format(
            "memory size must be a nonzero multiple of 1 MiB, at most 4096 MiB (got {} bytes)",
            config.memory_bytes));
    }
    if (config.cpu_clock.hertz == 0 || config.cpu_clock.hertz > maximum_frequency_hertz) {
        return std::unexpected("processor clock out of range");
    }
    if ((reset_config_word(config, prom) & mips::config_big_endian) == 0) {
        return std::unexpected("IP27 memory is big-endian: the reset mode bits must select it");
    }
    return {};
}

namespace {

mips::CpuConfig cpu_config(const Ip27Config& config, std::span<const std::byte> prom) {
    mips::CpuConfig cpu = config.cpu;
    cpu.config = reset_config_word(config, prom);
    return cpu;
}

} // namespace

Ip27Machine::Ip27Machine(const Ip27Config& config, std::span<const std::byte> prom)
    : config_{config}, memory_{config.memory_bytes}, flash_{flash_size},
      cpu_{bus_, tracer_, cpu_config(config, prom)} {
    hub_.connect_cpu(0, cpu_);
    invariant(validate(config, prom).has_value(),
              "Ip27Machine built from an invalid configuration");
    // Unprogrammed flash reads as 0xff.
    std::ranges::fill(flash_.bytes(), std::byte{0xff});
    std::ranges::copy(prom, flash_.bytes().begin());
    map_system_windows();
}

void Ip27Machine::map_system_windows() {
    using mips::system_address::uncached_window;
    const std::uint64_t memory = config_.memory_bytes;

    // CAC: cached requests reach memory.
    must_map(bus_.map_memory({PhysicalAddress{0}, memory}, memory_, 0, MemoryAccess::read_write),
             "CAC memory window");
    // UNCAC: uncached attribute 3 reaches memory.
    must_map(bus_.map_memory({PhysicalAddress{uncached_window(uncac_attribute)}, memory}, memory_,
                             0, MemoryAccess::read_write),
             "UNCAC memory window");
    // HSPEC UALIAS: uncached attribute 0, the first 256 MB of node space, reaches memory.
    must_map(bus_.map_memory(
                 {PhysicalAddress{uncached_window(hspec_attribute)}, std::min(memory, ualias_size)},
                 memory_, 0, MemoryAccess::read_write),
             "UALIAS window");
    // HSPEC LBOOT: the boot PROM, at the base of LBOOT (observed: the PROM reads its
    // ip27conf block there) and where the reset vector points. Flash programming is not
    // modeled in M2.
    for (const std::uint64_t base : {lboot_base, flash_base}) {
        must_map(
            bus_.map_memory({PhysicalAddress{uncached_window(hspec_attribute) + base}, flash_size},
                            flash_, 0, MemoryAccess::read_only),
            "LBOOT flash window");
    }
    // IO: the local Hub's registers, at IALIAS and at the remote-Hub alias for this node
    // (widget 1 window + 0x800000, LINUX REMOTE_HUB_ADDR; observed: the PROM uses it for its
    // own NASID 0).
    const std::uint64_t hub_window =
        uncached_window(io_attribute) + (hub_widget << small_window_shift);
    for (const std::uint64_t base : {hub_window, hub_window + Hub::window_size}) {
        must_map(bus_.map_mmio({PhysicalAddress{base}, Hub::window_size}, hub_), "Hub window");
    }
}

void Ip27Machine::reset(ResetKind kind) {
    hub_.reset(kind);
    cpu_.reset(kind);
}

void Ip27Machine::step() {
    const mips::IntegerState& state = cpu_.state();
    for (const std::uint64_t probe : pc_probes_) {
        if (probe == state.pc) {
            tracer_.log(TraceCategory::cpu,
                        "probe {:#018x} a0 {:#x} a1 {:#x} a2 {:#x} a3 {:#x} ra {:#x} v0 {:#x}",
                        state.pc, state.gpr[4], state.gpr[5], state.gpr[6], state.gpr[7],
                        state.gpr[31], state.gpr[2]);
        }
    }
    if (tracer_.enabled(TraceCategory::cpu) && pc_probes_.empty()) {
        tracer_.log(TraceCategory::cpu, "pc {:#018x}", state.pc);
    }
    interpreter_.step();
    scheduler_.advance_to(time_at_cycles(cpu_.cycles(), config_.cpu_clock));
}

void Ip27Machine::run(std::uint64_t cycles) {
    for (std::uint64_t i = 0; i < cycles; ++i) {
        step();
    }
}

} // namespace ultraviolent::ip27
