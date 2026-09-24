// Command-line entry point. Host integration only: argument parsing, reading asset files, and
// wiring a trace sink. Machine behavior lives in the machine personality.

#include <ultraviolent/backends/stream_trace_sink.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/machines/ip27/ip27_machine.hpp>
#include <ultraviolent/machines/ip27/prom_image.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ultraviolent;

constexpr int exit_usage = 2;
constexpr int exit_asset = 3;

void usage() {
    std::fputs("usage: ultraviolent --machine ip27 --prom FILE [--memory MIB] [--cycles N]\n"
               "                    [--trace CATEGORY[,CATEGORY...]] [--probe PC]...\n"
               "\n"
               "Runs the machine for N processor cycles (default 1000000) from power-on.\n"
               "Trace categories: cpu exception tlb memory hub xtalk xbow bridge pci ioc3\n"
               "scsi ethernet irq scheduler firmware machine all\n",
               stderr);
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

std::optional<std::vector<std::byte>> read_file(const std::string& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    std::vector<char> bytes{std::istreambuf_iterator<char>{stream},
                            std::istreambuf_iterator<char>{}};
    if (stream.bad()) {
        return std::nullopt;
    }
    std::vector<std::byte> result(bytes.size());
    std::ranges::transform(bytes, result.begin(), [](char c) { return static_cast<std::byte>(c); });
    return result;
}

struct Options {
    std::string machine;
    std::string prom;
    std::uint64_t memory_mib{512};
    std::uint64_t cycles{1'000'000};
    std::vector<TraceCategory> trace;
    std::vector<std::uint64_t> probes;
};

std::optional<Options> parse(std::span<char*> arguments) {
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view flag = arguments[i];
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
    backends::StreamTraceSink sink{stderr};
    machine.tracer().set_sink(&sink);
    for (const TraceCategory category : options->trace) {
        machine.tracer().enable(category);
    }
    for (const std::uint64_t pc : options->probes) {
        machine.add_pc_probe(pc);
    }
    if (!options->probes.empty()) {
        machine.tracer().enable(TraceCategory::cpu);
    }

    machine.run(options->cycles);

    const auto& state = machine.cpu().state();
    std::fprintf(stderr, "stopped after %llu cycles at virtual %llu ns, pc %#018llx\n",
                 static_cast<unsigned long long>(machine.cpu().cycles()),
                 static_cast<unsigned long long>(machine.now().nanoseconds),
                 static_cast<unsigned long long>(state.pc));
    return 0;
}
