#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/isp1020.hpp>
#include <ultraviolent/scsi/scsi.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace {

using namespace ultraviolent;
using devices::Isp1020;

// Host memory at PCI 0x1000, byte-addressed.
class Memory final : public pci::DmaPort {
  public:
    bool dma_read(std::uint64_t address, std::span<std::byte> bytes) override {
        if (address < base || address + bytes.size() > base + data.size()) {
            return false;
        }
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(address - base), bytes.size(),
                    bytes.begin());
        return true;
    }
    bool dma_write(std::uint64_t address, std::span<const std::byte> bytes) override {
        if (address < base || address + bytes.size() > base + data.size()) {
            return false;
        }
        std::ranges::copy(bytes, data.begin() + static_cast<std::ptrdiff_t>(address - base));
        return true;
    }
    static constexpr std::uint64_t base = 0x1000;
    std::vector<std::byte> data = std::vector<std::byte>(0x400);
};

// Register offsets (Linux qlogicisp.c).
constexpr std::uint64_t intf_sts = 0x0a;
constexpr std::uint64_t semaphore = 0x0c;
constexpr std::uint64_t mbox0 = 0x70;
constexpr std::uint64_t hccr = 0xc0;

struct Bench {
    Bench() {
        isp.config_write(0x14, 0x0010'0000, 0xf); // memory BAR at 1 MB
        isp.config_write(0x04, 0x6, 0x3);
        isp.connect_dma(memory);
        isp.connect_bus(bus);
        write(hccr, 0x3000); // release the RISC
    }
    std::uint32_t read(std::uint64_t reg) {
        return isp.read(pci::Space::memory, 0x10'0000 + reg, 2).value_or(0xdead);
    }
    void write(std::uint64_t reg, std::uint32_t value) {
        (void)isp.write(pci::Space::memory, 0x10'0000 + reg, 2, value);
    }
    // Issues a mailbox command the way Linux's isp1020_mbox_command does and returns the
    // outgoing mailboxes.
    std::array<std::uint32_t, 8> command(std::initializer_list<std::uint32_t> mailboxes) {
        std::uint64_t reg = mbox0;
        for (const std::uint32_t value : mailboxes) {
            write(reg, value);
            reg += 2;
        }
        write(semaphore, 0);
        write(hccr, 0x7000); // clear RISC interrupt
        write(hccr, 0x5000); // set host interrupt
        scheduler.advance_by(Isp1020::command_time);
        std::array<std::uint32_t, 8> out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = read(mbox0 + 2 * i);
        }
        return out;
    }
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    Memory memory;
    scsi::Bus bus;
    Isp1020 isp{scheduler, tracer, "isp"};
};

const test::Registration handshake{"isp1020.mailbox_handshake", [](test::Context& t) {
                                       Bench b;
                                       t.check_equal(b.read(0x00), std::uint32_t{0x1077});
                                       b.write(mbox0, 0x0000); // NO_OP
                                       b.write(hccr, 0x5000);
                                       t.check_equal(b.read(hccr) & 0x80,
                                                     std::uint32_t{0x80}); // host interrupt pending
                                       t.check_equal(b.read(semaphore), std::uint32_t{0});
                                       b.scheduler.advance_by(Isp1020::command_time);
                                       // Status posted: RISC interrupt, semaphore lock, completion
                                       // in mailbox 0.
                                       t.check_equal(b.read(hccr) & 0x80, std::uint32_t{0});
                                       t.check_equal(b.read(intf_sts) & 4, std::uint32_t{4});
                                       t.check_equal(b.read(semaphore), std::uint32_t{1});
                                       t.check_equal(b.read(mbox0), std::uint32_t{0x4000});
                                       b.write(hccr, 0x7000);
                                       b.write(semaphore, 0);
                                       t.check_equal(b.read(intf_sts) & 4, std::uint32_t{0});
                                       t.check_equal(b.read(semaphore), std::uint32_t{0});
                                   }};

const test::Registration paused{"isp1020.paused_risc_waits", [](test::Context& t) {
                                    Bench b;
                                    b.write(hccr, 0x2000); // pause
                                    b.write(mbox0, 0x0000);
                                    b.write(hccr, 0x5000);
                                    b.scheduler.advance_by(Isp1020::command_time);
                                    t.check_equal(b.read(semaphore), std::uint32_t{0});
                                    b.write(hccr, 0x3000); // release
                                    b.scheduler.advance_by(Isp1020::command_time);
                                    t.check_equal(b.read(mbox0), std::uint32_t{0x4000});
                                }};

const test::Registration ram_words{
    "isp1020.ram_words_and_register_test", [](test::Context& t) {
        Bench b;
        t.check_equal(b.command({0x0004, 0x1234, 0xbeef})[0], std::uint32_t{0x4000});
        const auto read = b.command({0x0005, 0x1234});
        t.check_equal(read[0], std::uint32_t{0x4000});
        t.check_equal(read[2], std::uint32_t{0xbeef});
        const auto echo = b.command({0x0006, 1, 2, 3, 4, 5, 6, 7});
        t.check_equal(echo[1], std::uint32_t{1});
        t.check_equal(echo[5], std::uint32_t{5});
        t.check_equal(b.command({0x00ff})[0], std::uint32_t{0x4001}); // invalid command
    }};

const test::Registration load_and_checksum{
    "isp1020.load_dump_and_checksum", [](test::Context& t) {
        Bench b;
        // A four-word image at PCI 0x1010, little-endian: length 4 in its fourth word, and
        // words summing to zero.
        const std::uint16_t image[] = {0x1111, 0x2222, 0x0000, 0x0004};
        std::uint16_t sum = 0;
        for (const std::uint16_t w : image) {
            sum = static_cast<std::uint16_t>(sum + w);
        }
        const std::uint16_t words[] = {image[0], image[1],
                                       static_cast<std::uint16_t>(0x10000 - sum), image[3]};
        for (std::size_t i = 0; i < 4; ++i) {
            b.memory.data[0x10 + 2 * i] = static_cast<std::byte>(words[i] & 0xff);
            b.memory.data[0x11 + 2 * i] = static_cast<std::byte>(words[i] >> 8);
        }
        t.check_equal(b.command({0x0001, 0x2000, 0x0000, 0x1010, 4})[0], std::uint32_t{0x4000});
        t.check_equal(b.command({0x0005, 0x2001})[2], std::uint32_t{0x2222});
        t.check_equal(b.command({0x0007, 0x2000})[0], std::uint32_t{0x4000});
        // A corrupted word fails the checksum.
        b.command({0x0004, 0x2001, 0x2223});
        t.check_equal(b.command({0x0007, 0x2000})[0], std::uint32_t{0x4003});
        // DUMP_RAM writes words back to host memory, little-endian.
        t.check_equal(b.command({0x0003, 0x2000, 0x0000, 0x1080, 2})[0], std::uint32_t{0x4000});
        t.check(b.memory.data[0x80] == std::byte{0x11} && b.memory.data[0x82] == std::byte{0x23});
        // A transfer the bus refuses is a host interface error.
        t.check_equal(b.command({0x0001, 0x2000, 0x0000, 0x9000, 4})[0], std::uint32_t{0x4002});
    }};

// Answers any command with eight bytes of DATA IN.
class Target final : public scsi::Target {
  public:
    scsi::Status execute(unsigned lun, std::span<const std::byte> cdb,
                         scsi::DataPhase& data) override {
        last_lun = lun;
        last_opcode = std::to_integer<unsigned>(cdb[0]);
        const std::array<std::byte, 8> reply{std::byte{1}, std::byte{2}, std::byte{3},
                                             std::byte{4}, std::byte{5}, std::byte{6},
                                             std::byte{7}, std::byte{8}};
        data.data_in(reply);
        return scsi::Status::good;
    }
    unsigned last_lun = 99;
    unsigned last_opcode = 0;
};

void put(Memory& m, std::uint64_t address, std::initializer_list<unsigned> bytes) {
    std::size_t i = address - Memory::base;
    for (const unsigned b : bytes) {
        m.data[i++] = static_cast<std::byte>(b);
    }
}

unsigned byte(const Memory& m, std::uint64_t address) {
    return std::to_integer<unsigned>(m.data[address - Memory::base]);
}

const test::Registration queues{
    "isp1020.request_and_response_queues", [](test::Context& t) {
        Bench b;
        Target target;
        b.bus.attach(2, target);
        // Firmware: a banner naming its version, then EXECUTE FIRMWARE.
        const char banner[] = "  Version 02.55  ";
        for (std::uint32_t i = 0; i + 1 < sizeof banner; i += 2) {
            b.command({0x0004, 0x1000 + i / 2,
                       static_cast<std::uint32_t>(banner[i]) << 8 |
                           static_cast<std::uint8_t>(banner[i + 1])});
        }
        t.check_equal(b.command({0x0002, 0x1000})[0], std::uint32_t{0x4000});
        const auto about = b.command({0x0008});
        t.check_equal(about[1], std::uint32_t{2});
        t.check_equal(about[2], std::uint32_t{55});
        // A64 queues: four responses at 0x1200 and four requests at 0x1100 (mailboxes 6 and 7
        // hold address bits 63:32).
        t.check_equal(b.command({0x0053, 4, 0, 0x1200, 0, 0, 0, 0})[0], std::uint32_t{0x4000});
        t.check_equal(b.command({0x0052, 4, 0, 0x1100, 0, 0, 0, 0})[0], std::uint32_t{0x4000});
        // A marker, then a command: INQUIRY to target 2 lun 0 into 16 bytes at 0x1300.
        put(b.memory, 0x1100, {4, 1});
        put(b.memory, 0x1140, {1, 1, 0, 0, 0x78, 0x56, 0x34, 0x12, 0, 2, 6, 0});
        put(b.memory, 0x1140 + 18, {1, 0, 0x12, 0, 0, 0, 16, 0});
        put(b.memory, 0x1140 + 32, {0x00, 0x13, 0, 0, 16, 0, 0, 0});
        b.write(mbox0 + 8, 2); // request in-pointer
        b.scheduler.advance_by(Isp1020::command_time);
        t.check_equal(target.last_opcode, 0x12u);
        t.check_equal(target.last_lun, 0u);
        t.check_equal(byte(b.memory, 0x1300), 1u);
        t.check_equal(byte(b.memory, 0x1307), 8u);
        t.check_equal(b.read(mbox0 + 8), std::uint32_t{2});  // request out-pointer
        t.check_equal(b.read(mbox0 + 10), std::uint32_t{1}); // response in-pointer
        t.check_equal(b.read(intf_sts) & 4, std::uint32_t{4});
        // The status entry: type 3, the handle, completion DATA UNDERRUN with 8 left over.
        t.check_equal(byte(b.memory, 0x1200), 3u);
        t.check_equal(byte(b.memory, 0x1207), 0x12u);
        t.check_equal(byte(b.memory, 0x120a), 0x15u);
        t.check_equal(byte(b.memory, 0x1214), 8u);
        // A command to an empty ID: selection timeout.
        put(b.memory, 0x1180, {1, 1, 0, 0, 0, 0, 0, 0, 0, 5, 6, 0});
        b.write(mbox0 + 8, 3);
        b.scheduler.advance_by(Isp1020::command_time);
        t.check_equal(byte(b.memory, 0x1240 + 10), 0x01u);
        t.check_equal(byte(b.memory, 0x1240 + 13), 0x01u); // state: got bus only
    }};

const test::Registration reset{"isp1020.reset_clears_mailboxes", [](test::Context& t) {
                                   Bench b;
                                   b.command({0x0000});
                                   t.check_equal(b.read(semaphore), std::uint32_t{1});
                                   b.write(0x08, 0x0001); // ISP_RESET
                                   t.check_equal(b.read(mbox0), std::uint32_t{0});
                                   t.check_equal(b.read(semaphore), std::uint32_t{0});
                                   t.check_equal(b.read(0x08) & 1, std::uint32_t{0});
                                   t.check_equal(b.read(hccr) & 0x20, std::uint32_t{0x20});
                               }};

} // namespace
