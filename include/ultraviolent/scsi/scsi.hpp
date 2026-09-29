#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// The parallel SCSI bus as a host adapter sees it (ANSI X3.131-1994, SCSI-2): targets by ID,
// each executing a command descriptor block for a logical unit, with the data phases driven by
// the target. Transfer timing, arbitration, and messages are not modeled.
namespace ultraviolent::scsi {

// Status byte (SCSI-2 7.3).
enum class Status : std::uint8_t {
    good = 0x00,
    check_condition = 0x02,
    busy = 0x08,
};

// The initiator's side of a command's data phases.
class DataPhase {
  public:
    // DATA IN: the target sends `bytes`. Returns how many the initiator took (its buffers
    // may be shorter).
    virtual std::size_t data_in(std::span<const std::byte> bytes) = 0;
    // DATA OUT: the target asks for up to `bytes.size()` bytes. Returns how many it got.
    virtual std::size_t data_out(std::span<std::byte> bytes) = 0;

  protected:
    DataPhase() = default;
    DataPhase(const DataPhase&) = default;
    DataPhase& operator=(const DataPhase&) = default;
    ~DataPhase() = default;
};

// A device on the bus.
class Target {
  public:
    virtual Status execute(unsigned lun, std::span<const std::byte> cdb, DataPhase& data) = 0;

  protected:
    Target() = default;
    Target(const Target&) = default;
    Target& operator=(const Target&) = default;
    ~Target() = default;
};

// Wide SCSI: IDs 0-15. An empty ID does not answer selection.
class Bus {
  public:
    static constexpr unsigned id_count = 16;

    void attach(unsigned id, Target& target) {
        targets_[id % id_count] = &target;
    }
    [[nodiscard]] Target* target(unsigned id) const {
        return targets_[id % id_count];
    }

  private:
    std::array<Target*, id_count> targets_{};
};

} // namespace ultraviolent::scsi
