#include "support/test.hpp"

#include <ultraviolent/core/block_store.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/scsi/cdrom.hpp>
#include <ultraviolent/scsi/disk.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace ultraviolent;

class MemoryStore final : public BlockStore {
  public:
    MemoryStore(std::size_t size, bool writable) : bytes(size), writable_{writable} {
        for (std::size_t i = 0; i < size; ++i) {
            bytes[i] = static_cast<std::byte>(i / 512); // each block holds its number
        }
    }
    [[nodiscard]] std::uint64_t size() const override {
        return bytes.size();
    }
    [[nodiscard]] bool writable() const override {
        return writable_;
    }
    bool read(std::uint64_t offset, std::span<std::byte> out) override {
        if (offset + out.size() > bytes.size()) {
            return false;
        }
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }
    bool write(std::uint64_t offset, std::span<const std::byte> in) override {
        if (!writable_ || offset + in.size() > bytes.size()) {
            return false;
        }
        std::ranges::copy(in, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    }
    std::vector<std::byte> bytes;

  private:
    bool writable_;
};

// Collects DATA IN; supplies DATA OUT from a buffer.
class Initiator final : public scsi::DataPhase {
  public:
    std::size_t data_in(std::span<const std::byte> bytes) override {
        received.insert(received.end(), bytes.begin(), bytes.end());
        return bytes.size();
    }
    std::size_t data_out(std::span<std::byte> bytes) override {
        const std::size_t n = std::min(bytes.size(), outgoing.size() - sent);
        std::copy_n(outgoing.begin() + static_cast<std::ptrdiff_t>(sent), n, bytes.begin());
        sent += n;
        return n;
    }
    std::vector<std::byte> received;
    std::vector<std::byte> outgoing;
    std::size_t sent = 0;
};

std::vector<std::byte> cdb(std::initializer_list<unsigned> bytes) {
    std::vector<std::byte> out;
    for (const unsigned b : bytes) {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

std::string text(const std::vector<std::byte>& bytes, std::size_t first, std::size_t count) {
    std::string out;
    for (std::size_t i = first; i < first + count && i < bytes.size(); ++i) {
        out.push_back(static_cast<char>(bytes[i]));
    }
    return out;
}

unsigned byte_at(const std::vector<std::byte>& bytes, std::size_t i) {
    return i < bytes.size() ? std::to_integer<unsigned>(bytes[i]) : 0xdead;
}

struct Bench {
    VirtualClock clock;
    Tracer tracer{clock};
};

const test::Registration cdrom_identity{
    "scsi.cdrom_inquiry_and_no_disc", [](test::Context& t) {
        Bench b;
        scsi::CdRom drive{b.tracer, "cd"};
        Initiator inquiry;
        t.check(drive.execute(0, cdb({0x12, 0, 0, 0, 36, 0}), inquiry) == scsi::Status::good);
        t.check_equal(byte_at(inquiry.received, 0), 0x05u); // CD-ROM
        t.check_equal(byte_at(inquiry.received, 1), 0x80u); // removable
        t.check_equal(text(inquiry.received, 8, 24), std::string{"TOSHIBA CD-ROM XM-6201TA"});
        // The allocation length limits the reply.
        Initiator shorter;
        drive.execute(0, cdb({0x12, 0, 0, 0, 5, 0}), shorter);
        t.check_equal(shorter.received.size(), std::size_t{5});
        // Other units do not exist.
        Initiator other;
        drive.execute(1, cdb({0x12, 0, 0, 0, 36, 0}), other);
        t.check_equal(byte_at(other.received, 0), 0x7fu);
        // No disc: NOT READY, MEDIUM NOT PRESENT, reported by REQUEST SENSE once.
        Initiator none;
        t.check(drive.execute(0, cdb({0x00, 0, 0, 0, 0, 0}), none) ==
                scsi::Status::check_condition);
        Initiator sense;
        drive.execute(0, cdb({0x03, 0, 0, 0, 18, 0}), sense);
        t.check_equal(byte_at(sense.received, 2), 0x02u);
        t.check_equal(byte_at(sense.received, 12), 0x3au);
        Initiator again;
        drive.execute(0, cdb({0x03, 0, 0, 0, 18, 0}), again);
        t.check_equal(byte_at(again.received, 2), 0x00u);
    }};

const test::Registration cdrom_read{
    "scsi.cdrom_read_and_block_length", [](test::Context& t) {
        Bench b;
        MemoryStore disc{std::size_t{8} * 512, false};
        scsi::CdRom drive{b.tracer, "cd"};
        drive.insert(&disc);
        // The first command after a disc change reports UNIT ATTENTION, NOT READY TO READY.
        Initiator ready;
        t.check(drive.execute(0, cdb({0x00, 0, 0, 0, 0, 0}), ready) ==
                scsi::Status::check_condition);
        Initiator attention;
        drive.execute(0, cdb({0x03, 0, 0, 0, 18, 0}), attention);
        t.check_equal(byte_at(attention.received, 2), 0x06u);
        t.check_equal(byte_at(attention.received, 12), 0x28u);
        Initiator ready_again;
        t.check(drive.execute(0, cdb({0x00, 0, 0, 0, 0, 0}), ready_again) == scsi::Status::good);
        Initiator capacity;
        drive.execute(0, cdb({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}), capacity);
        t.check_equal(byte_at(capacity.received, 3), 7u);    // last block
        t.check_equal(byte_at(capacity.received, 6), 0x02u); // 512 bytes
        Initiator read;
        t.check(drive.execute(0, cdb({0x28, 0, 0, 0, 0, 3, 0, 0, 2, 0}), read) ==
                scsi::Status::good);
        t.check_equal(read.received.size(), std::size_t{1024});
        t.check_equal(byte_at(read.received, 0), 3u);
        t.check_equal(byte_at(read.received, 512), 4u);
        Initiator past;
        t.check(drive.execute(0, cdb({0x28, 0, 0, 0, 0, 7, 0, 0, 2, 0}), past) ==
                scsi::Status::check_condition);
        // MODE SELECT to 2048-byte blocks: two per disc image of 4 KB.
        Initiator select;
        select.outgoing = cdb({0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x08, 0});
        t.check(drive.execute(0, cdb({0x15, 0x10, 0, 0, 12, 0}), select) == scsi::Status::good);
        Initiator capacity2;
        drive.execute(0, cdb({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}), capacity2);
        t.check_equal(byte_at(capacity2.received, 3), 1u);
    }};

const test::Registration disk_read_write{
    "scsi.disk_read_write_and_mode_pages", [](test::Context& t) {
        Bench b;
        MemoryStore store{std::size_t{2048} * 512, true};
        scsi::Disk disk{b.tracer, "disk", store};
        Initiator inquiry;
        disk.execute(0, cdb({0x12, 0, 0, 0, 36, 0}), inquiry);
        t.check_equal(byte_at(inquiry.received, 0), 0x00u); // direct access
        t.check_equal(text(inquiry.received, 8, 3), std::string{"IBM"});
        Initiator write;
        write.outgoing.assign(512, std::byte{0xab});
        t.check(disk.execute(0, cdb({0x2a, 0, 0, 0, 0, 5, 0, 0, 1, 0}), write) ==
                scsi::Status::good);
        t.check(store.bytes[std::size_t{5} * 512] == std::byte{0xab} &&
                store.bytes[std::size_t{6} * 512] == std::byte{6});
        Initiator read6;
        disk.execute(0, cdb({0x08, 0, 0, 5, 1, 0}), read6);
        t.check_equal(byte_at(read6.received, 511), 0xabu);
        // Rigid disk geometry: 2048 blocks of 16 heads x 64 sectors are 2 cylinders.
        Initiator geometry;
        t.check(disk.execute(0, cdb({0x1a, 0x08, 0x04, 0, 255, 0}), geometry) ==
                scsi::Status::good);
        t.check_equal(byte_at(geometry.received, 4), 0x04u); // page code, no descriptor
        t.check_equal(byte_at(geometry.received, 8), 2u);    // cylinders, low byte
        t.check_equal(byte_at(geometry.received, 9), 16u);   // heads
        Initiator unknown;
        t.check(disk.execute(0, cdb({0x1a, 0, 0x2a, 0, 255, 0}), unknown) ==
                scsi::Status::check_condition);
        // A read-only store refuses writes: DATA PROTECT.
        MemoryStore locked{512, false};
        scsi::Disk readonly{b.tracer, "ro", locked};
        Initiator refused;
        refused.outgoing.assign(512, std::byte{1});
        t.check(readonly.execute(0, cdb({0x0a, 0, 0, 0, 1, 0}), refused) ==
                scsi::Status::check_condition);
        Initiator sense;
        readonly.execute(0, cdb({0x03, 0, 0, 0, 18, 0}), sense);
        t.check_equal(byte_at(sense.received, 2), 0x07u);
    }};

} // namespace
