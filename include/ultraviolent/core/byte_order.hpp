#pragma once

#include <ultraviolent/core/invariant.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace ultraviolent {

enum class ByteOrder : std::uint8_t {
    // Most significant byte at the lowest address.
    big,
    // Least significant byte at the lowest address.
    little,
};

namespace detail {

constexpr bool host_order_is(ByteOrder order) {
    return (order == ByteOrder::big) == (std::endian::native == std::endian::big);
}

// Whole-integer copy for the common widths, used outside constant evaluation.
template <class T> T load_word(const std::byte* bytes, ByteOrder order) {
    T value;
    std::memcpy(&value, bytes, sizeof(T));
    return host_order_is(order) ? value : std::byteswap(value);
}

template <class T> void store_word(std::byte* bytes, T value, ByteOrder order) {
    if (!host_order_is(order)) {
        value = std::byteswap(value);
    }
    std::memcpy(bytes, &value, sizeof(T));
}

} // namespace detail

// Reads the unsigned integer stored in `bytes` (1 to 8 bytes) in `order`.
constexpr std::uint64_t load_unsigned(std::span<const std::byte> bytes, ByteOrder order) {
    invariant(!bytes.empty() && bytes.size() <= 8, "integer width must be 1 to 8 bytes");
    if (!std::is_constant_evaluated()) {
        switch (bytes.size()) {
        case 1:
            return std::to_integer<std::uint64_t>(bytes[0]);
        case 2:
            return detail::load_word<std::uint16_t>(bytes.data(), order);
        case 4:
            return detail::load_word<std::uint32_t>(bytes.data(), order);
        case 8:
            return detail::load_word<std::uint64_t>(bytes.data(), order);
        default:
            break;
        }
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const std::byte b = order == ByteOrder::big ? bytes[i] : bytes[bytes.size() - 1 - i];
        value = (value << 8) | std::to_integer<std::uint64_t>(b);
    }
    return value;
}

// Stores `value` into `bytes` (1 to 8 bytes) in `order`. The value must fit the width.
constexpr void store_unsigned(std::span<std::byte> bytes, std::uint64_t value, ByteOrder order) {
    invariant(!bytes.empty() && bytes.size() <= 8, "integer width must be 1 to 8 bytes");
    invariant(bytes.size() == 8 || (value >> (8 * bytes.size())) == 0,
              "value does not fit the integer width");
    if (!std::is_constant_evaluated()) {
        switch (bytes.size()) {
        case 1:
            bytes[0] = static_cast<std::byte>(value);
            return;
        case 2:
            detail::store_word(bytes.data(), static_cast<std::uint16_t>(value), order);
            return;
        case 4:
            detail::store_word(bytes.data(), static_cast<std::uint32_t>(value), order);
            return;
        case 8:
            detail::store_word(bytes.data(), value, order);
            return;
        default:
            break;
        }
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto b = static_cast<std::byte>(value & 0xff);
        if (order == ByteOrder::big) {
            bytes[bytes.size() - 1 - i] = b;
        } else {
            bytes[i] = b;
        }
        value >>= 8;
    }
}

} // namespace ultraviolent
