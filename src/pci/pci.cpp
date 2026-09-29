#include <ultraviolent/pci/pci.hpp>

// Type-0 header layout and BAR sizing: PCI Local Bus Specification 2.1, section 6.
namespace ultraviolent::pci {

namespace {

constexpr std::uint16_t command_io = 1u << 0;
constexpr std::uint16_t command_memory = 1u << 1;
// I/O space, memory space, bus master, memory write and invalidate, parity, SERR enables.
constexpr std::uint16_t command_writable = 0x0157;

std::uint32_t merge(std::uint32_t old_value, std::uint32_t value, unsigned byte_enable) {
    std::uint32_t mask = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if ((byte_enable & (1u << i)) != 0) {
            mask |= 0xffu << (8 * i);
        }
    }
    return (old_value & ~mask) | (value & mask);
}

} // namespace

ConfigHeader::ConfigHeader(std::uint16_t vendor, std::uint16_t device, std::uint16_t status,
                           std::uint32_t class_revision, std::array<Bar, 6> bars,
                           std::uint8_t interrupt_pin)
    : id_{vendor | (std::uint32_t{device} << 16)}, status_{status}, class_revision_{class_revision},
      bars_{bars}, interrupt_pin_{interrupt_pin} {}

void ConfigHeader::reset() {
    command_ = 0;
    bar_values_ = {};
    interrupt_line_ = 0;
    latency_ = 0;
    cache_line_ = 0;
}

std::uint32_t ConfigHeader::read(std::uint8_t reg) const {
    switch (reg) {
    case 0x00:
        return id_;
    case 0x04:
        // hypothesis: no error bits are ever set.
        return command_ | (std::uint32_t{status_} << 16);
    case 0x08:
        return class_revision_;
    case 0x0c:
        return cache_line_ | (std::uint32_t{latency_} << 8); // header type 0, no BIST
    case 0x3c:
        return interrupt_line_ | (std::uint32_t{interrupt_pin_} << 8);
    default:
        if (reg >= 0x10 && reg <= 0x24) {
            const unsigned index = (reg - 0x10u) / 4;
            const Bar& bar = bars_[index];
            if (bar.size == 0) {
                return 0;
            }
            // Address bits below the size read 0; I/O BARs have bit 0 set.
            return (bar_values_[index] & ~(bar.size - 1)) | (bar.space == Space::io ? 1 : 0);
        }
        return 0;
    }
}

void ConfigHeader::write(std::uint8_t reg, std::uint32_t value, unsigned byte_enable) {
    switch (reg) {
    case 0x04:
        command_ =
            static_cast<std::uint16_t>(merge(command_, value, byte_enable) & command_writable);
        return;
    case 0x0c: {
        const std::uint32_t merged =
            merge(cache_line_ | (std::uint32_t{latency_} << 8), value, byte_enable);
        cache_line_ = static_cast<std::uint8_t>(merged);
        latency_ = static_cast<std::uint8_t>(merged >> 8);
        return;
    }
    case 0x3c:
        interrupt_line_ = static_cast<std::uint8_t>(merge(interrupt_line_, value, byte_enable));
        return;
    default:
        if (reg >= 0x10 && reg <= 0x24) {
            const unsigned index = (reg - 0x10u) / 4;
            bar_values_[index] = merge(bar_values_[index], value, byte_enable);
        }
        return;
    }
}

std::optional<ConfigHeader::Hit> ConfigHeader::decode(Space space, std::uint64_t address) const {
    if ((command_ & (space == Space::io ? command_io : command_memory)) == 0) {
        return std::nullopt;
    }
    for (unsigned i = 0; i < bars_.size(); ++i) {
        const Bar& bar = bars_[i];
        if (bar.size == 0 || bar.space != space) {
            continue;
        }
        const std::uint64_t base = bar_values_[i] & ~(bar.size - 1) & ~std::uint32_t{0xf};
        if (address >= base && address < base + bar.size) {
            return Hit{i, address - base};
        }
    }
    return std::nullopt;
}

void ConfigHeader::save_state(StateImage& image, const std::string& key) const {
    image.put(key + ".command", command_);
    image.put(key + ".bars", bar_values_);
    image.put(key + ".interrupt_line", interrupt_line_);
    image.put(key + ".latency", latency_);
    image.put(key + ".cache_line", cache_line_);
}

void ConfigHeader::load_state(const StateImage& image, const std::string& key) {
    image.get(key + ".command", command_);
    image.get(key + ".bars", bar_values_);
    image.get(key + ".interrupt_line", interrupt_line_);
    image.get(key + ".latency", latency_);
    image.get(key + ".cache_line", cache_line_);
}

} // namespace ultraviolent::pci
