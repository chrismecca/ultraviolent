// Command-line entry point. Host integration only: argument parsing, reading asset files, and
// wiring a trace sink. Machine behavior lives in the machine personality.

#include <ultraviolent/backends/console.hpp>
#include <ultraviolent/backends/file_block_store.hpp>
#include <ultraviolent/backends/pcap_link.hpp>
#include <ultraviolent/backends/stream_trace_sink.hpp>
#include <ultraviolent/backends/tap_link.hpp>
#include <ultraviolent/backends/terminal_input.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/devices/am29f080.hpp>
#include <ultraviolent/devices/m48t35.hpp>
#include <ultraviolent/machines/ip27/ip27_machine.hpp>
#include <ultraviolent/machines/ip27/prom_image.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

using namespace ultraviolent;

// Set by SIGINT, SIGUSR1, SIGTERM, or SIGHUP: end the run early but still save the snapshot,
// flash, NVRAM, and disk.
volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int /*signal*/) {
    stop_requested = 1;
}

constexpr int exit_usage = 2;
constexpr int exit_asset = 3;

void usage() {
    std::fputs(
        "usage: ultraviolent --machine ip27 --prom FILE [--memory MIB] [--cycles N]\n"
        "                    [--trace CATEGORY[,CATEGORY...]] [--probe PC]...\n"
        "                    [--load-state FILE] [--save-state FILE] [--flash FILE]\n"
        "                    [--nvram FILE] [--io6prom FILE]\n"
        "                    [--console LINE]... [--cdrom FILE] [--disk FILE]\n"
        "                    [--sample-pc NS] [--date DATE] [--stop-at-prompt]\n"
        "                    [--interactive] [--ethernet tap:IFNAME|pcap:FILE]\n"
        "                    [--engine reference|tier0] [--engine-statistics]\n"
        "                    [--tier0-cache-entries N]\n"
        "\n"
        "Runs the machine for N processor cycles (default 1000000) from power-on, or\n"
        "from a snapshot. Snapshots are for exploration; checkpoints need a cold run.\n"
        "The guest console (IOC3 port A) is standard output; each --console LINE is\n"
        "typed when the guest waits for input after a prompt ending in \"> \", \"] \",\n"
        "\"? \", \": \", \") \", \"# \", \"$ \", or \"% \". --flash keeps the flash PROM (and its\n"
        "log) in FILE, created from the PROM image if missing. --nvram keeps the timekeeper's\n"
        "battery-backed NVRAM (the PROM environment) and clock in FILE, created if missing;\n"
        "the clock resumes from the time FILE holds unless --date is given.\n"
        "--cdrom puts a disc image in the CD-ROM drive (SCSI bus 0, ID 6); a --console line\n"
        "\"@cdrom FILE\" changes the disc when its turn comes, \"@expect A|B\" waits for one of\n"
        "the strings in the output, \"@stop\" ends the run. --disk attaches an\n"
        "existing image file as the system disk (bus 0, ID 1), written in place.\n"
        "--sample-pc NS prints the program counter every NS ns of virtual time.\n"
        "--io6prom FILE loads the BaseIO's own flash PROM (for example io6prom.img); without it\n"
        "the IP27 PROM uses its internal copy of the BASEIO PROM.\n"
        "--ethernet tap:IFNAME plugs the BaseIO Ethernet port into an existing Linux TAP\n"
        "interface you own (sudo ip tuntap add dev IFNAME mode tap user $USER); without it\n"
        "the port is unplugged. --ethernet pcap:FILE records the frames the guest sends in FILE\n"
        "(libpcap format, virtual time) and delivers none; runs stay deterministic.\n"
        "--interactive passes what you type to the console once the --console lines are\n"
        "used up (Ctrl-C and the like go to the guest). Ctrl-] starts a monitor command: q\n"
        "ends the run, cdrom FILE changes the disc, eject empties the drive. Such runs are\n"
        "not deterministic.\n"
        "--stop-at-prompt ends the run when the console script is used up and the guest\n"
        "waits at a prompt (--cycles is then a limit). SIGINT, SIGUSR1, SIGTERM, or SIGHUP\n"
        "ends a run early; the snapshot, flash, NVRAM, and disk are still written.\n"
        "--date YYYY-MM-DD[THH:MM:SS] sets the time-of-day clock at power-on (UTC; default\n"
        "2026-01-01): virtual time runs it from there.\n"
        "--engine reference|tier0 selects the execution engine (default reference, the\n"
        "oracle; tier0 is experimental, doc/IR.adoc). --engine-statistics prints tier 0's\n"
        "block statistics at the end of the run. --tier0-cache-entries N sizes its block\n"
        "cache (a power of two; default 4096).\n"
        "Trace categories: cpu exception tlb memory hub xtalk xbow bridge pci ioc3\n"
        "scsi ethernet irq scheduler firmware machine all\n",
        stderr);
}

// Tier 0's block statistics (IR.adoc "Stages" E, F), for benchmark review.
void print_block_statistics(const mips::BlockStatistics& s, std::size_t marked_frames,
                            std::uint64_t code_frame_stores) {
    const auto share = [](std::uint64_t part, std::uint64_t whole) {
        return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    };
    const auto ratio = [](std::uint64_t a, std::uint64_t b) {
        return b == 0 ? 0.0 : static_cast<double>(a) / static_cast<double>(b);
    };
    const auto u = [](std::uint64_t n) { return static_cast<unsigned long long>(n); };
    std::uint64_t fallbacks = 0;
    for (const std::uint64_t n : s.fallbacks) {
        fallbacks += n;
    }
    std::uint64_t built_operations = 0;
    for (std::size_t n = 0; n < s.built_lengths.size(); ++n) {
        built_operations += n * s.built_lengths[n];
    }
    std::fprintf(stderr,
                 "tier0: %llu block entries, %llu decoded operations (%.2f per entry, %.2f per "
                 "lookup; %llu from cache hits), %llu reference steps, %llu interrupts at entry\n",
                 u(s.entries), u(s.operations), ratio(s.operations, s.entries),
                 ratio(s.operations, s.lookups), u(s.cached_operations), u(fallbacks),
                 u(s.pending_at_entry));
    std::fprintf(stderr,
                 "tier0 cache: %llu lookups, %llu hits (%.2f%%), %llu blocks built (average "
                 "%.2f operations), reuse mean %.1f max %llu, %zu marked code frames, %llu "
                 "stores to code frames\n",
                 u(s.lookups), u(s.hits), share(s.hits, s.lookups), u(s.blocks_built),
                 ratio(built_operations, s.blocks_built), ratio(s.hits, s.blocks_built),
                 u(s.max_reuse), marked_frames, u(code_frame_stores));
    const auto rows = [&](const char* title, const auto& counts, const auto& names,
                          std::uint64_t whole) {
        for (std::size_t i = 0; i < counts.size(); ++i) {
            std::fprintf(stderr, "tier0 %-14s %-18s %12llu (%5.1f%%)\n", title, names[i],
                         u(counts[i]), share(counts[i], whole));
        }
    };
    constexpr const char* misses[] = {"cold", "tag", "epoch", "code frame", "tlb entry"};
    rows("cache miss:", s.misses, misses, s.lookups);
    constexpr const char* no_block[] = {"not host-backed", "unsupported", "delay slot"};
    rows("reference:", s.fallbacks, no_block, fallbacks);
    constexpr const char* ends[] = {"control flow", "state change",  "unsupported",
                                    "delay slot",   "page boundary", "length limit"};
    rows("block end:", s.built_ends, ends, s.blocks_built);
    constexpr const char* exits[] = {"completed", "run limit", "pending exception",
                                     "exception", "host path", "left block"};
    rows("block exit:", s.exits, exits, s.entries);
    constexpr const char* barriers[] = {"memory fill", "device", "uncached memory", "code store",
                                        "other"};
    rows("barrier:", s.barriers, barriers, s.entries);
    const auto histogram = [&](const char* name, const auto& lengths) {
        std::fprintf(stderr, "tier0 %s lengths:", name);
        for (std::size_t n = 0; n < lengths.size(); ++n) {
            if (lengths[n] != 0) {
                std::fprintf(stderr, " %zu:%llu", n, u(lengths[n]));
            }
        }
        std::fprintf(stderr, "\n");
    };
    histogram("built", s.built_lengths);
    histogram("run", s.run_lengths);
}

std::optional<std::uint64_t> parse_number(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<TraceCategory> parse_category(std::string_view name) {
    for (std::size_t i = 0; i < trace_category_count; ++i) {
        const auto category = static_cast<TraceCategory>(i);
        if (trace_category_name(category) == name) {
            return category;
        }
    }
    return std::nullopt;
}

bool write_file(const std::string& path, std::span<const std::byte> bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

std::optional<std::vector<std::byte>> read_file(const std::string& path) {
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream) {
        return std::nullopt;
    }
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(result.data()), size);
    if (!stream) {
        return std::nullopt;
    }
    return result;
}

struct Options {
    std::string machine;
    std::string prom;
    std::uint64_t memory_mib{512};
    std::uint64_t cycles{1'000'000};
    std::vector<TraceCategory> trace;
    std::vector<std::uint64_t> probes;
    std::string load_state;
    std::string save_state;
    std::vector<std::string> console_lines;
    std::string flash;
    std::string nvram;
    std::string io6prom;
    std::string ethernet;
    std::string cdrom;
    std::string disk;
    std::uint64_t sample_pc_ns{};
    std::optional<std::int64_t> date;
    bool stop_at_prompt{};
    bool interactive{};
    mips::ExecutionEngine engine{mips::ExecutionEngine::reference};
    bool engine_statistics{};
    std::uint64_t block_cache_entries{mips::BlockInterpreter::default_cache_entries};
};

// "YYYY-MM-DD" or "YYYY-MM-DDTHH:MM:SS" (UTC) as Unix seconds.
std::optional<std::int64_t> parse_date(std::string_view text) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    const std::string copy{text};
    const int fields = std::sscanf(copy.c_str(), "%d-%u-%uT%u:%u:%u", &year, &month, &day, &hour,
                                   &minute, &second);
    if (fields != 3 && fields != 6) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{month},
                                           std::chrono::day{day}};
    if (!date.ok() || hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date}.time_since_epoch().count() * std::int64_t{86'400} +
           std::int64_t{hour} * 3600 + std::int64_t{minute} * 60 + second;
}

std::optional<Options> parse(std::span<char*> arguments) {
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view flag = arguments[i];
        if (flag == "--stop-at-prompt") {
            options.stop_at_prompt = true;
            continue;
        }
        if (flag == "--interactive") {
            options.interactive = true;
            continue;
        }
        if (flag == "--engine-statistics") {
            options.engine_statistics = true;
            continue;
        }
        if (i + 1 >= arguments.size()) {
            std::fprintf(stderr, "missing value for %.*s\n", static_cast<int>(flag.size()),
                         flag.data());
            return std::nullopt;
        }
        const std::string_view value = arguments[++i];
        if (flag == "--machine") {
            options.machine = value;
        } else if (flag == "--prom") {
            options.prom = value;
        } else if (flag == "--load-state") {
            options.load_state = value;
        } else if (flag == "--save-state") {
            options.save_state = value;
        } else if (flag == "--flash") {
            options.flash = value;
        } else if (flag == "--nvram") {
            options.nvram = value;
        } else if (flag == "--io6prom") {
            options.io6prom = value;
        } else if (flag == "--ethernet") {
            if (!value.starts_with("tap:") && !value.starts_with("pcap:")) {
                std::fprintf(stderr, "--ethernet takes tap:IFNAME or pcap:FILE\n");
                return std::nullopt;
            }
            options.ethernet = value;
        } else if (flag == "--engine") {
            if (value == "reference") {
                options.engine = mips::ExecutionEngine::reference;
            } else if (value == "tier0") {
                options.engine = mips::ExecutionEngine::tier0;
            } else {
                std::fprintf(stderr, "--engine takes reference or tier0\n");
                return std::nullopt;
            }
        } else if (flag == "--cdrom") {
            options.cdrom = value;
        } else if (flag == "--disk") {
            options.disk = value;
        } else if (flag == "--console") {
            options.console_lines.emplace_back(value);
        } else if (flag == "--date") {
            options.date = parse_date(value);
            if (!options.date) {
                std::fprintf(stderr, "not a date (YYYY-MM-DD[THH:MM:SS]): %.*s\n",
                             static_cast<int>(value.size()), value.data());
                return std::nullopt;
            }
        } else if (flag == "--sample-pc") {
            const auto number = parse_number(value);
            if (!number || *number == 0) {
                std::fprintf(stderr, "not a sampling interval: %.*s\n",
                             static_cast<int>(value.size()), value.data());
                return std::nullopt;
            }
            options.sample_pc_ns = *number;
        } else if (flag == "--tier0-cache-entries") {
            const auto number = parse_number(value);
            if (!number || !std::has_single_bit(*number)) {
                std::fprintf(stderr, "--tier0-cache-entries takes a power of two\n");
                return std::nullopt;
            }
            options.block_cache_entries = *number;
        } else if (flag == "--memory" || flag == "--cycles") {
            const auto number = parse_number(value);
            if (!number) {
                std::fprintf(stderr, "not a number: %.*s\n", static_cast<int>(value.size()),
                             value.data());
                return std::nullopt;
            }
            (flag == "--memory" ? options.memory_mib : options.cycles) = *number;
        } else if (flag == "--probe") {
            std::uint64_t pc = 0;
            const std::string_view digits = value.starts_with("0x") ? value.substr(2) : value;
            const auto [end, error] =
                std::from_chars(digits.data(), digits.data() + digits.size(), pc, 16);
            if (error != std::errc{} || end != digits.data() + digits.size()) {
                std::fprintf(stderr, "not a hexadecimal address: %.*s\n",
                             static_cast<int>(value.size()), value.data());
                return std::nullopt;
            }
            options.probes.push_back(pc);
        } else if (flag == "--trace") {
            std::string_view rest = value;
            while (!rest.empty()) {
                const std::size_t comma = rest.find(',');
                const std::string_view name = rest.substr(0, comma);
                rest =
                    comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                if (name == "all") {
                    for (std::size_t c = 0; c < trace_category_count; ++c) {
                        options.trace.push_back(static_cast<TraceCategory>(c));
                    }
                } else if (const auto category = parse_category(name)) {
                    options.trace.push_back(*category);
                } else {
                    std::fprintf(stderr, "unknown trace category: %.*s\n",
                                 static_cast<int>(name.size()), name.data());
                    return std::nullopt;
                }
            }
        } else {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(flag.size()),
                         flag.data());
            return std::nullopt;
        }
    }
    if (options.machine != "ip27" || options.prom.empty()) {
        return std::nullopt;
    }
    if (options.interactive && options.stop_at_prompt) {
        std::fprintf(stderr, "--interactive and --stop-at-prompt do not combine\n");
        return std::nullopt;
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse(std::span{argv + 1, static_cast<std::size_t>(argc - 1)});
    if (!options) {
        usage();
        return exit_usage;
    }

    ip27::Ip27Config config;
    config.memory_bytes = options->memory_mib << 20;
    if (options->date) {
        config.clock_epoch = *options->date;
    }

    const auto file = read_file(options->prom);
    if (!file) {
        std::fprintf(stderr, "cannot read PROM image %s\n", options->prom.c_str());
        return exit_asset;
    }
    const auto prom = ip27::decode_prom_image(*file);
    if (!prom) {
        const std::string_view reason = ip27::describe(prom.error());
        std::fprintf(stderr, "invalid PROM image %s: %.*s\n", options->prom.c_str(),
                     static_cast<int>(reason.size()), reason.data());
        return exit_asset;
    }

    if (auto valid = ip27::validate(config, *prom); !valid) {
        std::fprintf(stderr, "configuration error: %s\n", valid.error().c_str());
        return exit_usage;
    }

    ip27::Ip27Machine machine{config, *prom};
    machine.set_execution_engine(options->engine);
    machine.set_block_cache_entries(options->block_cache_entries);
    backends::StreamTraceSink sink{stderr};
    machine.tracer().set_sink(&sink);
    // The guest console: the IOC3's first serial port, on standard output, with scripted input.
    backends::ScriptedConsole console{stdout, options->console_lines, !options->load_state.empty()};
    machine.connect_console(console);
    machine.connect_console_input(console);
    if (options->stop_at_prompt) {
        console.on_script_done([&machine] { machine.request_stop(); });
    }
    console.on_stop([&machine] { machine.request_stop(); });
    // After the script, an interactive run reads the user's terminal (standard input).
    std::optional<backends::TerminalInput> terminal;
    if (options->interactive) {
        terminal.emplace(STDIN_FILENO);
        console.then_read(*terminal);
        std::fprintf(stderr,
                     "interactive console: Ctrl-] then q ends the run (Ctrl-] then ? for help)\n");
    }
    for (const TraceCategory category : options->trace) {
        machine.tracer().enable(category);
    }
    for (const std::uint64_t pc : options->probes) {
        machine.add_pc_probe(pc);
    }
    if (!options->probes.empty()) {
        machine.tracer().enable(TraceCategory::cpu);
    }

    std::unique_ptr<backends::FileBlockStore> cdrom;
    if (!options->cdrom.empty()) {
        cdrom = backends::FileBlockStore::open(options->cdrom, false);
        if (!cdrom) {
            std::fprintf(stderr, "cannot open CD-ROM image %s\n", options->cdrom.c_str());
            return exit_asset;
        }
        machine.insert_cdrom(cdrom.get());
    }
    // Disc changes the console script asks for ("@cdrom PATH"). Replaced images stay open
    // until the machine is done with them.
    std::vector<std::unique_ptr<backends::FileBlockStore>> changed_discs;
    const auto change_disc = [&](const std::string& path) {
        std::unique_ptr<backends::FileBlockStore> disc;
        if (!path.empty()) {
            disc = backends::FileBlockStore::open(path, false);
            if (!disc) {
                std::fprintf(stderr, "cannot open CD-ROM image %s; drive left empty\n",
                             path.c_str());
            }
        }
        std::fprintf(stderr, "disc change to %s\n", path.empty() ? "(none)" : path.c_str());
        machine.insert_cdrom(disc.get());
        changed_discs.push_back(std::move(disc));
    };
    console.on_disc_change(change_disc);
    // Monitor commands after Ctrl-] in an interactive run.
    if (terminal) {
        terminal->on_command([&](std::string_view line) {
            if (line == "q" || line == "quit") {
                machine.request_stop();
            } else if (line.starts_with("cdrom ")) {
                change_disc(std::string{line.substr(6)});
            } else if (line == "eject") {
                change_disc("");
            } else {
                std::fprintf(stderr, "monitor commands: q (end the run; the snapshot, flash, "
                                     "NVRAM, and disk are still written), cdrom FILE, eject\n");
            }
        });
    }

    std::unique_ptr<backends::FileBlockStore> disk;
    if (!options->disk.empty()) {
        disk = backends::FileBlockStore::open(options->disk, true);
        if (!disk) {
            std::fprintf(stderr, "cannot open disk image %s\n", options->disk.c_str());
            return exit_asset;
        }
        machine.attach_disk(*disk);
    }

    // The flash persists in a host file: its PROM log survives runs (IP27.adoc "Boot PROM").
    // A snapshot's flash contents take precedence over the file.
    if (!options->flash.empty()) {
        if (const auto contents = read_file(options->flash)) {
            if (contents->size() != machine.flash_contents().size()) {
                std::fprintf(stderr, "flash file %s is not %zu bytes\n", options->flash.c_str(),
                             machine.flash_contents().size());
                return exit_asset;
            }
            machine.load_flash(*contents);
        }
    }

    // The BaseIO's own flash PROM. A snapshot's contents take precedence (loaded below).
    if (!options->io6prom.empty()) {
        const auto image = read_file(options->io6prom);
        if (!image || image->size() > devices::Am29f080::size) {
            std::fprintf(stderr, "cannot use BaseIO PROM image %s\n", options->io6prom.c_str());
            return exit_asset;
        }
        machine.load_baseio_flash(*image);
    }

    // The timekeeper's NVRAM persists in a host file, as its battery keeps it: the PROM
    // environment survives, and the clock resumes from the time the file holds unless --date
    // sets it. A snapshot's timekeeper takes precedence over the file.
    if (!options->nvram.empty()) {
        if (const auto contents = read_file(options->nvram)) {
            if (contents->size() != devices::M48t35::size) {
                std::fprintf(stderr, "NVRAM file %s is not %zu bytes\n", options->nvram.c_str(),
                             devices::M48t35::size);
                return exit_asset;
            }
            std::vector<std::uint8_t> bytes(contents->size());
            std::ranges::transform(*contents, bytes.begin(),
                                   [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
            machine.load_nvram(bytes, !options->date);
        }
    }

    if (!options->load_state.empty()) {
        const auto data = read_file(options->load_state);
        const auto image = data ? StateImage::deserialize(*data) : std::nullopt;
        if (!image) {
            std::fprintf(stderr, "cannot read snapshot %s\n", options->load_state.c_str());
            return exit_asset;
        }
        if (auto loaded = machine.load_state(*image); !loaded) {
            std::fprintf(stderr, "cannot load snapshot %s: %s\n", options->load_state.c_str(),
                         loaded.error().c_str());
            return exit_asset;
        }
    }

    // The Ethernet cable, after any snapshot: this run's configuration decides whether the
    // port is plugged in.
    std::unique_ptr<EthernetLink> cable;
    if (options->ethernet.starts_with("tap:")) {
        std::string error;
        cable = backends::TapLink::open(options->ethernet.substr(4), error);
        if (!cable) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return exit_asset;
        }
    } else if (options->ethernet.starts_with("pcap:")) {
        const std::string path = options->ethernet.substr(5);
        cable = backends::PcapLink::open(path, machine.clock());
        if (!cable) {
            std::fprintf(stderr, "cannot write capture %s\n", path.c_str());
            return exit_asset;
        }
    }
    machine.connect_ethernet(cable.get());

    // Diagnostic: the program counter every N ns of virtual time, on standard error. Pure
    // observation (an event that only reads the PC).
    const VirtualDuration interval{options->sample_pc_ns};
    auto& scheduler = machine.scheduler();
    EventId sample{};
    if (options->sample_pc_ns != 0) {
        sample = scheduler.add_event("diagnostic.sample_pc", [&] {
            std::fprintf(stderr, "sample %llu pc %#018llx\n",
                         static_cast<unsigned long long>(machine.now().nanoseconds),
                         static_cast<unsigned long long>(machine.cpu().state().pc));
            scheduler.schedule_after(sample, interval);
        });
        scheduler.schedule_after(sample, interval);
    }

    // A host signal ends the run at the next check; the check only reads a host flag.
    const EventId stop_check = scheduler.add_event("host.stop_check", [&] {
        if (stop_requested != 0) {
            machine.request_stop();
            return;
        }
        scheduler.schedule_after(stop_check, VirtualDuration{10'000'000});
    });
    scheduler.schedule_after(stop_check, VirtualDuration{10'000'000});
    std::signal(SIGINT, request_stop);
    std::signal(SIGUSR1, request_stop);
    std::signal(SIGTERM, request_stop);
    std::signal(SIGHUP, request_stop);

    machine.run(options->cycles);
    scheduler.cancel(stop_check); // snapshots carry no host events
    terminal.reset();             // the user's terminal back to normal before any messages
    if (options->sample_pc_ns != 0) {
        scheduler.cancel(sample);
    }

    if (!options->flash.empty() && machine.flash_dirty() &&
        !write_file(options->flash, machine.flash_contents())) {
        std::fprintf(stderr, "cannot write flash %s\n", options->flash.c_str());
        return exit_asset;
    }

    if (!options->nvram.empty() &&
        !write_file(options->nvram, std::as_bytes(machine.nvram_contents()))) {
        std::fprintf(stderr, "cannot write NVRAM %s\n", options->nvram.c_str());
        return exit_asset;
    }

    if (!options->save_state.empty() &&
        !write_file(options->save_state, machine.save_state().serialize())) {
        std::fprintf(stderr, "cannot write snapshot %s\n", options->save_state.c_str());
        return exit_asset;
    }

    machine.insert_cdrom(nullptr);
    if (options->engine_statistics) {
        print_block_statistics(machine.block_statistics(), machine.marked_code_frames(),
                               machine.cpu().code_frame_stores());
    }
    const auto& state = machine.cpu().state();
    std::fprintf(stderr, "stopped after %llu cycles at virtual %llu ns, pc %#018llx\n",
                 static_cast<unsigned long long>(machine.cpu().cycles()),
                 static_cast<unsigned long long>(machine.now().nanoseconds),
                 static_cast<unsigned long long>(state.pc));
    return 0;
}
