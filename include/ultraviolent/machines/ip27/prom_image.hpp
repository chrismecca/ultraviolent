#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ultraviolent::ip27 {

// The IP27 Flash PROM: 1 MB, of which pages 0-13 (896 KB) hold PROM code and pages 14-15 the
// PROM log (IP27.adoc "Boot PROM").
inline constexpr std::size_t flash_size = 0x10'0000;
inline constexpr std::size_t prom_code_size = std::size_t{14} * 0x1'0000;

enum class PromImageError : std::uint8_t {
    // Empty, or larger than the code pages of the flash.
    bad_size,
    // Carries the promgen tags but its header fields are inconsistent.
    bad_container,
};

std::string_view describe(PromImageError error);

// Extracts the bytes that belong at the start of the flash from a user-supplied image file.
// Accepts a promgen-format image (IP27.adoc) or a raw payload. The bytes themselves are never
// modified.
std::expected<std::vector<std::byte>, PromImageError>
decode_prom_image(std::span<const std::byte> file);

// The R10000 reset mode word from the flash's "ip27conf" block, if the image has one
// (IP27.adoc "Reset mode bits").
std::optional<std::uint32_t> prom_mode_bits(std::span<const std::byte> flash);

} // namespace ultraviolent::ip27
