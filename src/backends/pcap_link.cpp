#include <ultraviolent/backends/pcap_link.hpp>

#include <array>
#include <cstdint>

namespace ultraviolent::backends {

namespace {

// libpcap file format: a global header, then per frame a record header and the bytes, all
// little-endian here (the magic number tells readers the byte order).
constexpr std::uint32_t pcap_magic = 0xa1b2'c3d4;
constexpr std::uint16_t version_major = 2;
constexpr std::uint16_t version_minor = 4;
constexpr std::uint32_t snapshot_length = 65'535;
constexpr std::uint32_t link_type_ethernet = 1;

void put(std::FILE* file, std::uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        std::fputc(static_cast<int>((value >> (8 * i)) & 0xff), file);
    }
}

} // namespace

std::unique_ptr<PcapLink> PcapLink::open(const std::string& path, const VirtualClock& clock) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return nullptr;
    }
    put(file, pcap_magic, 4);
    put(file, version_major, 2);
    put(file, version_minor, 2);
    put(file, 0, 4); // time zone
    put(file, 0, 4); // timestamp accuracy
    put(file, snapshot_length, 4);
    put(file, link_type_ethernet, 4);
    return std::unique_ptr<PcapLink>{new PcapLink{file, clock}};
}

PcapLink::~PcapLink() {
    std::fclose(file_);
}

void PcapLink::send(std::span<const std::byte> frame) {
    const std::uint64_t now = clock_.now().nanoseconds;
    const auto length = static_cast<std::uint32_t>(frame.size());
    put(file_, static_cast<std::uint32_t>(now / 1'000'000'000), 4);
    put(file_, static_cast<std::uint32_t>(now % 1'000'000'000 / 1'000), 4);
    put(file_, length, 4);
    put(file_, length, 4);
    std::fwrite(frame.data(), 1, frame.size(), file_);
    std::fflush(file_);
}

std::optional<std::vector<std::byte>> PcapLink::receive() {
    return std::nullopt;
}

} // namespace ultraviolent::backends
