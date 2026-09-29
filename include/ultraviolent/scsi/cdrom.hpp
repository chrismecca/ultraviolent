#pragma once

#include <ultraviolent/core/block_store.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/scsi/scsi.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace ultraviolent::scsi {

// A SCSI-2 CD-ROM drive (IP27.adoc "SCSI"), logical unit 0, with a disc image in a
// BlockStore (none: no disc). Commands: TEST UNIT READY, REQUEST SENSE, INQUIRY, READ(6),
// READ(10), READ CAPACITY, MODE SENSE(6), MODE SELECT(6) (block length), START STOP UNIT,
// PREVENT ALLOW MEDIUM REMOVAL. Others fail with ILLEGAL REQUEST. Sources: ANSI X3.131-1994
// (SCSI-2) chapters 7, 8, and 13.
//
// Identity: slate's drive, TOSHIBA CD-ROM XM-6201TA revision 1037, ANSI version 2 with
// relative addressing, synchronous transfer, and linked commands (OBS-SLATE-0002).
// hypothesis: the drive starts with 512-byte logical blocks, the sector size of SGI CD
// volume headers (IRIX media carry an SGI disk label with 512-byte sectors).
class CdRom final : public Target {
  public:
    static constexpr std::uint32_t default_block_length = 512;

    CdRom(Tracer& tracer, std::string name) : tracer_{tracer}, name_{std::move(name)} {}

    // A disc change; the next command other than INQUIRY and REQUEST SENSE reports UNIT
    // ATTENTION, NOT READY TO READY CHANGE (SCSI-2 7.9, sense 06/28/00).
    void insert(BlockStore* disc) {
        disc_ = disc;
        unit_attention_ = true;
    }

    Status execute(unsigned lun, std::span<const std::byte> cdb, DataPhase& data) override;

    // Snapshot fields under `name`.
    void save_state(StateImage& image, const std::string& key) const;
    void load_state(const StateImage& image, const std::string& key);

  private:
    Status check_condition(std::uint8_t key, std::uint8_t asc, std::uint8_t ascq = 0);
    Status read(std::uint64_t block, std::uint32_t count, DataPhase& data);
    [[nodiscard]] std::uint64_t block_count() const;

    Tracer& tracer_;
    std::string name_;
    BlockStore* disc_{};
    std::uint32_t block_length_{default_block_length};
    bool unit_attention_{};
    // Sense data for the next REQUEST SENSE: key, additional sense code and qualifier.
    std::array<std::uint8_t, 3> sense_{};
};

} // namespace ultraviolent::scsi
