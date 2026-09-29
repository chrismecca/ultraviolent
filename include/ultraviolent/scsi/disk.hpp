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

// A SCSI-2 direct-access disk (IP27.adoc "SCSI"), logical unit 0, 512-byte blocks, on a
// BlockStore. Commands: TEST UNIT READY, REZERO UNIT, REQUEST SENSE, FORMAT UNIT, READ(6/10),
// WRITE(6/10), SEEK(6/10), INQUIRY, MODE SELECT(6), MODE SENSE(6) (pages 1, 3, 4, 8),
// RESERVE, RELEASE, START STOP UNIT, SEND DIAGNOSTIC, PREVENT ALLOW MEDIUM REMOVAL, READ
// CAPACITY, VERIFY(10), SYNCHRONIZE CACHE. Sources: ANSI X3.131-1994 (SCSI-2) chapters 7-9.
//
// Identity: one of slate's system disks, IBM HUS103036FL3800 revision RPQR (OBS-SLATE-0002).
// hypothesis: the geometry it reports (mode pages 3 and 4) is 16 heads of 64 sectors, and
// FORMAT UNIT, VERIFY, and SEND DIAGNOSTIC succeed without touching the data.
class Disk final : public Target {
  public:
    static constexpr std::uint32_t block_length = 512;

    Disk(Tracer& tracer, std::string name, BlockStore& store)
        : tracer_{tracer}, name_{std::move(name)}, store_{store} {}

    Status execute(unsigned lun, std::span<const std::byte> cdb, DataPhase& data) override;

    void save_state(StateImage& image, const std::string& key) const;
    void load_state(const StateImage& image, const std::string& key);

  private:
    Status transfer(std::uint64_t block, std::uint32_t count, bool write, DataPhase& data);
    Status mode_sense(std::span<const std::byte> cdb, DataPhase& data);
    [[nodiscard]] std::uint64_t block_count() const {
        return store_.size() / block_length;
    }

    Tracer& tracer_;
    std::string name_;
    BlockStore& store_;
    std::array<std::uint8_t, 3> sense_{};
};

} // namespace ultraviolent::scsi
