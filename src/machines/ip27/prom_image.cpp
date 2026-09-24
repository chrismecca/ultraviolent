#include <ultraviolent/machines/ip27/prom_image.hpp>

#include <ultraviolent/core/byte_order.hpp>

#include <algorithm>

namespace ultraviolent::ip27 {

namespace {

// promgen container layout, observed in the user's image (IP27.adoc "Boot PROM").
constexpr std::size_t container_tag_offset = 0x40;
constexpr std::size_t total_size_offset = 0x50;
constexpr std::size_t image_tag_offset = 0x80;
constexpr std::size_t header_size_offset = 0x98;
constexpr std::size_t payload_size_offset = 0xb0;
constexpr std::size_t minimum_header = 0xb8;

bool has_tag(std::span<const std::byte> file, std::size_t offset, std::string_view tag) {
    if (file.size() < offset + tag.size()) {
        return false;
    }
    return std::ranges::equal(file.subspan(offset, tag.size()), tag, {}, {},
                              [](char c) { return static_cast<std::byte>(c); });
}

std::uint64_t field(std::span<const std::byte> file, std::size_t offset) {
    return load_unsigned(file.subspan(offset, 8), ByteOrder::big);
}

} // namespace

std::optional<std::uint32_t> prom_mode_bits(std::span<const std::byte> flash) {
    // observed: flash offset 0x68 holds "ip27conf" and 0x64 the mode word (IP27.adoc).
    constexpr std::size_t mode_offset = 0x64;
    constexpr std::size_t tag_offset = 0x68;
    if (!has_tag(flash, tag_offset, "ip27conf")) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(load_unsigned(flash.subspan(mode_offset, 4), ByteOrder::big));
}

std::string_view describe(PromImageError error) {
    switch (error) {
    case PromImageError::bad_size:
        return "image is empty or larger than the IP27 flash code area";
    case PromImageError::bad_container:
        return "promgen header fields are inconsistent with the file";
    }
    return "unknown PROM image error";
}

std::expected<std::vector<std::byte>, PromImageError>
decode_prom_image(std::span<const std::byte> file) {
    std::span<const std::byte> payload = file;
    if (has_tag(file, container_tag_offset, "JFKSWCSM") &&
        has_tag(file, image_tag_offset, "ip27prom")) {
        if (file.size() < minimum_header) {
            return std::unexpected(PromImageError::bad_container);
        }
        const std::uint64_t total = field(file, total_size_offset);
        const std::uint64_t header = field(file, header_size_offset);
        const std::uint64_t size = field(file, payload_size_offset);
        if (total != file.size() || header < minimum_header || header > file.size() ||
            size != file.size() - header) {
            return std::unexpected(PromImageError::bad_container);
        }
        payload = file.subspan(header);
    }
    if (payload.empty() || payload.size() > prom_code_size) {
        return std::unexpected(PromImageError::bad_size);
    }
    return std::vector<std::byte>(payload.begin(), payload.end());
}

} // namespace ultraviolent::ip27
