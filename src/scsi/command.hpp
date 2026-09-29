#pragma once

#include <ultraviolent/scsi/scsi.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Helpers shared by the SCSI target models: CDB fields, big-endian parameter data, and sense.
namespace ultraviolent::scsi::detail {

// Operation codes (SCSI-2).
inline constexpr std::uint8_t test_unit_ready = 0x00;
inline constexpr std::uint8_t rezero_unit = 0x01;
inline constexpr std::uint8_t request_sense = 0x03;
inline constexpr std::uint8_t format_unit = 0x04;
inline constexpr std::uint8_t read_6 = 0x08;
inline constexpr std::uint8_t write_6 = 0x0a;
inline constexpr std::uint8_t seek_6 = 0x0b;
inline constexpr std::uint8_t inquiry = 0x12;
inline constexpr std::uint8_t mode_select_6 = 0x15;
inline constexpr std::uint8_t reserve_unit = 0x16;
inline constexpr std::uint8_t release_unit = 0x17;
inline constexpr std::uint8_t mode_sense_6 = 0x1a;
inline constexpr std::uint8_t start_stop_unit = 0x1b;
inline constexpr std::uint8_t send_diagnostic = 0x1d;
inline constexpr std::uint8_t prevent_allow = 0x1e;
inline constexpr std::uint8_t read_capacity = 0x25;
inline constexpr std::uint8_t read_10 = 0x28;
inline constexpr std::uint8_t write_10 = 0x2a;
inline constexpr std::uint8_t seek_10 = 0x2b;
inline constexpr std::uint8_t verify_10 = 0x2f;
inline constexpr std::uint8_t synchronize_cache = 0x35;

// Sense keys and additional sense codes.
inline constexpr std::uint8_t not_ready = 0x02;
inline constexpr std::uint8_t medium_error = 0x03;
inline constexpr std::uint8_t illegal_request = 0x05;
inline constexpr std::uint8_t unit_attention = 0x06;
inline constexpr std::uint8_t data_protect = 0x07;
inline constexpr std::uint8_t medium_may_have_changed = 0x28;
inline constexpr std::uint8_t unrecovered_read_error = 0x11;
inline constexpr std::uint8_t invalid_opcode = 0x20;
inline constexpr std::uint8_t lba_out_of_range = 0x21;
inline constexpr std::uint8_t lun_not_supported = 0x25;
inline constexpr std::uint8_t invalid_field_in_parameters = 0x26;
inline constexpr std::uint8_t write_protected = 0x27;
inline constexpr std::uint8_t medium_not_present = 0x3a;
inline constexpr std::uint8_t write_error = 0x0c;

inline std::uint8_t at(std::span<const std::byte> bytes, std::size_t i) {
    return i < bytes.size() ? std::to_integer<std::uint8_t>(bytes[i]) : 0;
}

inline std::uint32_t big_endian(std::span<const std::byte> bytes, std::size_t first,
                                std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        value = value << 8 | at(bytes, first + i);
    }
    return value;
}

inline void put_big_endian(std::span<std::byte> bytes, std::size_t first, std::size_t count,
                           std::uint32_t value) {
    for (std::size_t i = 0; i < count; ++i) {
        bytes[first + count - 1 - i] = static_cast<std::byte>(value >> (8 * i));
    }
}

inline void put_text(std::span<std::byte> bytes, std::size_t first, std::size_t width,
                     std::string_view text) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes[first + i] = static_cast<std::byte>(i < text.size() ? text[i] : ' ');
    }
}

// Sends at most the allocation length.
inline void send(DataPhase& data, std::span<const std::byte> bytes, std::size_t allocation) {
    data.data_in(bytes.first(std::min(bytes.size(), allocation)));
}

// The standard INQUIRY data (SCSI-2 8.2.5): peripheral type, removable medium, ANSI version,
// capability flags (byte 7), and identification; peripheral qualifier 3 for other units.
struct Identity {
    std::uint8_t device_type;
    bool removable;
    std::uint8_t ansi_version;
    std::uint8_t flags;
    std::string_view vendor;
    std::string_view product;
    std::string_view revision;
};

inline void send_inquiry(DataPhase& data, const Identity& identity, unsigned lun,
                         std::size_t allocation) {
    std::array<std::byte, 36> reply{};
    if (lun != 0) {
        reply[0] = std::byte{0x7f};
    } else {
        reply[0] = std::byte{identity.device_type};
        reply[1] = identity.removable ? std::byte{0x80} : std::byte{0};
        reply[2] = std::byte{identity.ansi_version};
        reply[3] = std::byte{0x02}; // response data format 2
        reply[4] = std::byte{reply.size() - 5};
        reply[7] = std::byte{identity.flags};
        put_text(reply, 8, 8, identity.vendor);
        put_text(reply, 16, 16, identity.product);
        put_text(reply, 32, 4, identity.revision);
    }
    send(data, reply, allocation);
}

// Pending sense data: key, additional sense code and qualifier.
using SenseData = std::array<std::uint8_t, 3>;

inline Status fail(SenseData& sense, std::uint8_t key, std::uint8_t asc, std::uint8_t ascq = 0) {
    sense = {key, asc, ascq};
    return Status::check_condition;
}

// REQUEST SENSE: fixed-format sense data (SCSI-2 8.2.14); clears the pending sense.
inline void send_sense(DataPhase& data, SenseData& pending, std::size_t allocation) {
    std::array<std::byte, 18> sense{};
    sense[0] = std::byte{0x70};
    sense[2] = std::byte{pending[0]};
    sense[7] = std::byte{10};
    sense[12] = std::byte{pending[1]};
    sense[13] = std::byte{pending[2]};
    pending = {};
    send(data, sense, allocation);
}

} // namespace ultraviolent::scsi::detail
