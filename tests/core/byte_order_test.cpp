#include "support/test.hpp"

#include <ultraviolent/core/byte_order.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using namespace ultraviolent;

constexpr std::array<std::byte, 8> sample{std::byte{0x01}, std::byte{0x23}, std::byte{0x45},
                                          std::byte{0x67}, std::byte{0x89}, std::byte{0xab},
                                          std::byte{0xcd}, std::byte{0xef}};

const test::Registration load_big_endian{
    "byte_order.load_big_endian", [](test::Context& t) {
        const std::span<const std::byte> bytes{sample};
        t.check_equal(load_unsigned(bytes.first(1), ByteOrder::big), std::uint64_t{0x01});
        t.check_equal(load_unsigned(bytes.first(2), ByteOrder::big), std::uint64_t{0x0123});
        t.check_equal(load_unsigned(bytes.first(4), ByteOrder::big), std::uint64_t{0x01234567});
        t.check_equal(load_unsigned(bytes, ByteOrder::big), std::uint64_t{0x0123456789abcdef});
    }};

const test::Registration load_little_endian{
    "byte_order.load_little_endian", [](test::Context& t) {
        const std::span<const std::byte> bytes{sample};
        t.check_equal(load_unsigned(bytes.first(1), ByteOrder::little), std::uint64_t{0x01});
        t.check_equal(load_unsigned(bytes.first(2), ByteOrder::little), std::uint64_t{0x2301});
        t.check_equal(load_unsigned(bytes.first(4), ByteOrder::little), std::uint64_t{0x67452301});
        t.check_equal(load_unsigned(bytes, ByteOrder::little), std::uint64_t{0xefcdab8967452301});
    }};

const test::Registration store_round_trips{
    "byte_order.store_round_trips", [](test::Context& t) {
        for (const ByteOrder order : {ByteOrder::big, ByteOrder::little}) {
            for (const std::size_t width : {1u, 2u, 4u, 8u}) {
                std::array<std::byte, 8> buffer{};
                const std::uint64_t value = width == 8 ? 0x8877665544332211
                                                       : (std::uint64_t{0x8877665544332211} &
                                                          ((std::uint64_t{1} << (8 * width)) - 1));
                store_unsigned(std::span{buffer}.first(width), value, order);
                t.check_equal(load_unsigned(std::span{buffer}.first(width), order), value);
            }
        }
        std::array<std::byte, 4> buffer{};
        store_unsigned(buffer, 0xdeadbeef, ByteOrder::big);
        t.check(buffer[0] == std::byte{0xde} && buffer[3] == std::byte{0xef},
                "big-endian store puts the most significant byte first");
    }};

} // namespace
