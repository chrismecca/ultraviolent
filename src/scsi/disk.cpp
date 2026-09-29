#include <ultraviolent/scsi/disk.hpp>

#include "command.hpp"

#include <vector>

namespace ultraviolent::scsi {

using namespace detail;

namespace {

// OBS-SLATE-0002: ANSI version 4; wide (WBus16), Sync, Linked, CmdQue.
constexpr Identity identity{0x00,  false, 4, 0x20 | 0x10 | 0x08 | 0x02, "IBM", "HUS103036FL3800",
                            "RPQR"};
constexpr unsigned heads = 16;
constexpr unsigned sectors_per_track = 64;

} // namespace

Status Disk::execute(unsigned lun, std::span<const std::byte> cdb, DataPhase& data) {
    const std::uint8_t opcode = at(cdb, 0);
    tracer_.log(TraceCategory::scsi, "{} opcode {:#04x} lun {}", name_, opcode, lun);
    if (opcode == request_sense) {
        send_sense(data, sense_, at(cdb, 4));
        return Status::good;
    }
    if (opcode == inquiry) {
        send_inquiry(data, identity, lun, at(cdb, 4));
        return Status::good;
    }
    if (lun != 0) {
        return fail(sense_, illegal_request, lun_not_supported);
    }
    switch (opcode) {
    case test_unit_ready:
    case rezero_unit:
    case format_unit:
    case seek_6:
    case seek_10:
    case reserve_unit:
    case release_unit:
    case start_stop_unit:
    case send_diagnostic:
    case prevent_allow:
    case verify_10:
    case synchronize_cache:
        return Status::good;
    case read_capacity: {
        std::array<std::byte, 8> reply{};
        const std::uint64_t blocks = block_count();
        put_big_endian(reply, 0, 4, static_cast<std::uint32_t>(blocks == 0 ? 0 : blocks - 1));
        put_big_endian(reply, 4, 4, block_length);
        send(data, reply, reply.size());
        return Status::good;
    }
    case read_6:
    case write_6: {
        const std::uint32_t block = big_endian(cdb, 1, 3) & 0x1f'ffff;
        const std::uint32_t count = at(cdb, 4);
        return transfer(block, count == 0 ? 256 : count, opcode == write_6, data);
    }
    case read_10:
    case write_10:
        return transfer(big_endian(cdb, 2, 4), big_endian(cdb, 7, 2), opcode == write_10, data);
    case mode_sense_6:
        return mode_sense(cdb, data);
    case mode_select_6: {
        // Parameters are accepted and ignored: nothing the disk models is changeable.
        std::vector<std::byte> parameters(at(cdb, 4));
        data.data_out(parameters);
        return Status::good;
    }
    default:
        tracer_.log(TraceCategory::scsi, "{} unmodeled opcode {:#04x}", name_, opcode);
        return fail(sense_, illegal_request, invalid_opcode);
    }
}

Status Disk::transfer(std::uint64_t block, std::uint32_t count, bool write, DataPhase& data) {
    if (block + count > block_count()) {
        return fail(sense_, illegal_request, lba_out_of_range);
    }
    if (write && !store_.writable()) {
        return fail(sense_, data_protect, write_protected);
    }
    tracer_.log(TraceCategory::scsi, "{} {} block {} count {}", name_, write ? "write" : "read",
                block, count);
    std::vector<std::byte> buffer(block_length);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t offset = (block + i) * block_length;
        if (write) {
            if (data.data_out(buffer) != buffer.size()) {
                break;
            }
            if (!store_.write(offset, buffer)) {
                return fail(sense_, medium_error, write_error);
            }
        } else {
            if (!store_.read(offset, buffer)) {
                return fail(sense_, medium_error, unrecovered_read_error);
            }
            if (data.data_in(buffer) != buffer.size()) {
                break;
            }
        }
    }
    return Status::good;
}

Status Disk::mode_sense(std::span<const std::byte> cdb, DataPhase& data) {
    const bool no_block_descriptor = (at(cdb, 1) & 0x08) != 0;
    const unsigned control = at(cdb, 2) >> 6; // 0 current, 1 changeable, 2 default, 3 saved
    const unsigned page = at(cdb, 2) & 0x3f;
    std::vector<std::byte> reply(4);
    if (!no_block_descriptor) {
        std::array<std::byte, 8> descriptor{};
        const std::uint64_t blocks = block_count();
        put_big_endian(descriptor, 1, 3,
                       static_cast<std::uint32_t>(std::min<std::uint64_t>(blocks, 0xff'ffff)));
        put_big_endian(descriptor, 5, 3, block_length);
        reply.insert(reply.end(), descriptor.begin(), descriptor.end());
        reply[3] = std::byte{8};
    }
    const auto add_page = [&](unsigned code, std::size_t length, auto fill) {
        std::vector<std::byte> body(length + 2);
        body[0] = static_cast<std::byte>(code);
        body[1] = static_cast<std::byte>(length);
        if (control != 1) { // changeable values: none
            fill(std::span{body});
        }
        reply.insert(reply.end(), body.begin(), body.end());
    };
    const std::uint32_t cylinders =
        static_cast<std::uint32_t>(block_count() / (std::uint64_t{heads} * sectors_per_track));
    bool known = false;
    if (page == 0x01 || page == 0x3f) { // read-write error recovery
        add_page(0x01, 10, [](std::span<std::byte>) {});
        known = true;
    }
    if (page == 0x03 || page == 0x3f) { // format device
        add_page(0x03, 22, [&](std::span<std::byte> body) {
            put_big_endian(body, 2, 2, heads); // tracks per zone
            put_big_endian(body, 10, 2, sectors_per_track);
            put_big_endian(body, 12, 2, block_length);
            put_big_endian(body, 14, 2, 1); // interleave
            body[20] = std::byte{0x40};     // HSEC: hard sectored
        });
        known = true;
    }
    if (page == 0x04 || page == 0x3f) { // rigid disk geometry
        add_page(0x04, 22, [&](std::span<std::byte> body) {
            put_big_endian(body, 2, 3, cylinders);
            body[5] = static_cast<std::byte>(heads);
            put_big_endian(body, 20, 2, 10'000); // rotations per minute
        });
        known = true;
    }
    if (page == 0x08 || page == 0x3f) { // caching
        add_page(0x08, 10, [](std::span<std::byte>) {});
        known = true;
    }
    if (!known && page != 0x00) {
        return fail(sense_, illegal_request, 0x24); // invalid field in CDB
    }
    reply[0] = static_cast<std::byte>(reply.size() - 1);
    send(data, reply, at(cdb, 4));
    return Status::good;
}

void Disk::save_state(StateImage& image, const std::string& key) const {
    const std::uint32_t registers[] = {sense_[0], sense_[1], sense_[2]};
    image.put(key + ".disk", registers);
}

void Disk::load_state(const StateImage& image, const std::string& key) {
    std::uint32_t registers[3]{};
    if (image.get(key + ".disk", registers)) {
        sense_ = {static_cast<std::uint8_t>(registers[0]), static_cast<std::uint8_t>(registers[1]),
                  static_cast<std::uint8_t>(registers[2])};
    }
}

} // namespace ultraviolent::scsi
