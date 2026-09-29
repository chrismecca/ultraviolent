#include <ultraviolent/scsi/cdrom.hpp>

#include "command.hpp"

#include <vector>

namespace ultraviolent::scsi {

using namespace detail;

namespace {

// slate's drive (OBS-SLATE-0002): RelAdr, Sync, Linked.
constexpr Identity identity{0x05,  true, 2, 0x80 | 0x10 | 0x08, "TOSHIBA", "CD-ROM XM-6201TA",
                            "1037"};

} // namespace

Status CdRom::check_condition(std::uint8_t key, std::uint8_t asc, std::uint8_t ascq) {
    return fail(sense_, key, asc, ascq);
}

std::uint64_t CdRom::block_count() const {
    return disc_ != nullptr ? disc_->size() / block_length_ : 0;
}

Status CdRom::execute(unsigned lun, std::span<const std::byte> cdb, DataPhase& data) {
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
        return check_condition(illegal_request, lun_not_supported);
    }
    if (unit_attention_) {
        unit_attention_ = false;
        return check_condition(unit_attention, medium_may_have_changed);
    }
    switch (opcode) {
    case test_unit_ready:
        return disc_ != nullptr ? Status::good : check_condition(not_ready, medium_not_present);
    case start_stop_unit:
    case prevent_allow:
        return Status::good;
    case read_capacity: {
        if (disc_ == nullptr) {
            return check_condition(not_ready, medium_not_present);
        }
        std::array<std::byte, 8> reply{};
        const std::uint64_t blocks = block_count();
        put_big_endian(reply, 0, 4, static_cast<std::uint32_t>(blocks == 0 ? 0 : blocks - 1));
        put_big_endian(reply, 4, 4, block_length_);
        send(data, reply, reply.size());
        return Status::good;
    }
    case read_6: {
        const std::uint32_t block = big_endian(cdb, 1, 3) & 0x1f'ffff;
        const std::uint32_t count = at(cdb, 4);
        return read(block, count == 0 ? 256 : count, data);
    }
    case read_10:
        return read(big_endian(cdb, 2, 4), big_endian(cdb, 7, 2), data);
    case mode_sense_6: {
        // Header and one block descriptor; no pages. hypothesis: software asks only for the
        // block length.
        std::array<std::byte, 12> reply{};
        reply[0] = std::byte{reply.size() - 1};
        reply[3] = std::byte{8};
        put_big_endian(reply, 9, 3, block_length_);
        send(data, reply, at(cdb, 4));
        return Status::good;
    }
    case mode_select_6: {
        std::vector<std::byte> parameters(at(cdb, 4));
        data.data_out(parameters);
        if (parameters.size() >= 12 && at(parameters, 3) >= 8) {
            const std::uint32_t length = big_endian(parameters, 9, 3);
            if (length != 512 && length != 2048) {
                return check_condition(illegal_request, invalid_field_in_parameters);
            }
            block_length_ = length;
            tracer_.log(TraceCategory::scsi, "{} block length {}", name_, length);
        }
        return Status::good;
    }
    default:
        tracer_.log(TraceCategory::scsi, "{} unmodeled opcode {:#04x}", name_, opcode);
        return check_condition(illegal_request, invalid_opcode);
    }
}

Status CdRom::read(std::uint64_t block, std::uint32_t count, DataPhase& data) {
    if (disc_ == nullptr) {
        return check_condition(not_ready, medium_not_present);
    }
    if (block + count > block_count()) {
        return check_condition(illegal_request, lba_out_of_range);
    }
    tracer_.log(TraceCategory::scsi, "{} read block {} count {}", name_, block, count);
    std::vector<std::byte> buffer(block_length_);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!disc_->read((block + i) * block_length_, buffer)) {
            return check_condition(medium_error, unrecovered_read_error);
        }
        if (data.data_in(buffer) != buffer.size()) {
            break; // the initiator's buffers are full
        }
    }
    return Status::good;
}

void CdRom::save_state(StateImage& image, const std::string& key) const {
    const std::uint32_t registers[] = {block_length_, sense_[0], sense_[1], sense_[2]};
    image.put(key + ".cdrom", registers);
    image.put(key + ".unit_attention", unit_attention_);
}

void CdRom::load_state(const StateImage& image, const std::string& key) {
    std::uint32_t registers[4]{};
    if (image.get(key + ".cdrom", registers)) {
        block_length_ = registers[0];
        sense_ = {static_cast<std::uint8_t>(registers[1]), static_cast<std::uint8_t>(registers[2]),
                  static_cast<std::uint8_t>(registers[3])};
    }
    image.get(key + ".unit_attention", unit_attention_);
}

} // namespace ultraviolent::scsi
