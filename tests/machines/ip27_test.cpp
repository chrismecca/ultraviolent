#include "arch/mips/assembler.hpp"
#include "support/test.hpp"

#include <ultraviolent/core/byte_order.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/virtual_time.hpp>
#include <ultraviolent/machines/ip27/ip27_machine.hpp>
#include <ultraviolent/machines/ip27/prom_image.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace ultraviolent;
using namespace ultraviolent::mips::assembler;
using ip27::Ip27Config;
using ip27::Ip27Machine;

// Small memory keeps tests light; the address map does not depend on the size.
Ip27Config small_config() {
    Ip27Config config;
    config.memory_bytes = 16u << 20;
    return config;
}

std::vector<std::byte> program(std::initializer_list<std::uint32_t> words) {
    std::vector<std::byte> bytes(words.size() * 4);
    std::size_t offset = 0;
    for (const std::uint32_t word : words) {
        store_unsigned(std::span{bytes}.subspan(offset, 4), word, ByteOrder::big);
        offset += 4;
    }
    return bytes;
}

void put_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    store_unsigned(std::span{bytes}.subspan(offset, 8), value, ByteOrder::big);
}

void put_tag(std::vector<std::byte>& bytes, std::size_t offset, std::string_view tag) {
    for (std::size_t i = 0; i < tag.size(); ++i) {
        bytes[offset + i] = static_cast<std::byte>(tag[i]);
    }
}

// A synthetic image in the promgen layout of IP27.adoc (no SGI content).
std::vector<std::byte> promgen(const std::vector<std::byte>& payload, std::size_t header = 0x1000) {
    std::vector<std::byte> file(header + payload.size());
    put_tag(file, 0x40, "JFKSWCSM");
    put_u64(file, 0x50, file.size());
    put_tag(file, 0x80, "ip27prom");
    put_u64(file, 0x98, header);
    put_u64(file, 0xb0, payload.size());
    std::ranges::copy(payload, file.begin() + static_cast<std::ptrdiff_t>(header));
    return file;
}

const test::Registration raw_image{"ip27.prom_image.raw", [](test::Context& t) {
                                       const auto payload = program({nop(), nop()});
                                       const auto decoded = ip27::decode_prom_image(payload);
                                       t.check(decoded.has_value() && *decoded == payload,
                                               "raw images pass through unchanged");
                                   }};

const test::Registration container_image{
    "ip27.prom_image.promgen", [](test::Context& t) {
        const auto payload = program({lui(t0, 1), nop(), nop()});
        const auto decoded = ip27::decode_prom_image(promgen(payload));
        t.check(decoded.has_value() && *decoded == payload, "payload follows the header");

        auto inconsistent = promgen(payload);
        put_u64(inconsistent, 0xb0, payload.size() + 4);
        const auto rejected = ip27::decode_prom_image(inconsistent);
        t.check(!rejected && rejected.error() == ip27::PromImageError::bad_container);
    }};

const test::Registration image_sizes{
    "ip27.prom_image.sizes", [](test::Context& t) {
        const auto empty = ip27::decode_prom_image({});
        t.check(!empty && empty.error() == ip27::PromImageError::bad_size);
        const std::vector<std::byte> too_large(ip27::prom_code_size + 4);
        const auto large = ip27::decode_prom_image(too_large);
        t.check(!large && large.error() == ip27::PromImageError::bad_size);
    }};

const test::Registration ethernet_nic{
    "ip27.ethernet_nic_record", [](test::Context& t) {
        const auto memory = ip27::ethernet_nic_memory({0x08, 0x00, 0x69, 0x0e, 0x17, 0x01});
        t.check_equal(unsigned{memory[0]}, 10u);    // record length
        t.check_equal(unsigned{memory[10]}, 0x08u); // address byte 0
        t.check_equal(unsigned{memory[5]}, 0x01u);  // address byte 5
        t.check_equal(unsigned{memory[1]}, 0u);
        t.check_equal(unsigned{devices::dallas_crc16(memory.data(), 13)}, 0xb001u);
        t.check_equal(unsigned{memory[13]}, 0xffu); // unprogrammed
    }};

const test::Registration board_nic{
    "ip27.board_nic_record", [](test::Context& t) {
        const auto memory = ip27::board_nic_memory({.serial = "UVX002",
                                                    .part = "030-0734-003",
                                                    .revision = "A",
                                                    .name = "BASEIO",
                                                    .capability = 0x1234'5678});
        const auto text = [&memory](std::size_t at, std::size_t length) {
            return std::string_view{reinterpret_cast<const char*>(memory.data()) + at, length};
        };
        t.check_equal(unsigned{memory[0]}, 1u); // format
        t.check(text(1, 6) == "UVX002");
        t.check_equal(unsigned{memory[7]}, unsigned{' '}); // space padding
        t.check(text(11, 12) == "030-0734-003");
        t.check(text(32 + 6, 1) == "A");
        t.check_equal(unsigned{memory[32 + 10]}, 0xffu); // group
        t.check_equal(unsigned{memory[32 + 11]}, 0x12u); // capability, most significant first
        t.check_equal(unsigned{memory[32 + 14]}, 0x78u);
        t.check_equal(unsigned{memory[32 + 15]}, 0xffu); // variety
        t.check(text(32 + 16, 6) == "BASEIO");
        // Each page's CRC-16 leaves the residue IRIX checks.
        t.check_equal(unsigned{devices::dallas_crc16(memory.data(), 32)}, 0xb001u);
        t.check_equal(unsigned{devices::dallas_crc16(memory.data() + 32, 32)}, 0xb001u);
        t.check_equal(unsigned{memory[64]}, 0xffu); // unprogrammed
    }};

const test::Registration configuration{
    "ip27.configuration_validation", [](test::Context& t) {
        const auto prom = program({nop()});
        t.check(ip27::validate(Ip27Config{}, prom).has_value(), "defaults are valid");
        Ip27Config config;
        config.memory_bytes = 0;
        t.check(!ip27::validate(config, prom), "no memory");
        config.memory_bytes = (16u << 20) + 4096;
        t.check(!ip27::validate(config, prom), "not a whole MiB");
        config.memory_bytes = (std::uint64_t{4} << 30) + (1u << 20);
        t.check(!ip27::validate(config, prom), "larger than a node");
        config = Ip27Config{};
        config.cpu.config &= ~mips::config_big_endian;
        t.check(!ip27::validate(config, prom), "little-endian");
        t.check(!ip27::validate(Ip27Config{}, {}), "no PROM");
    }};

// A flash image with an ip27conf block (IP27.adoc "Reset mode bits"); synthetic values.
std::vector<std::byte> prom_with_mode_bits(std::uint32_t mode) {
    auto prom = program({nop(), nop()});
    prom.resize(0x100, std::byte{0});
    store_unsigned(std::span{prom}.subspan(0x64, 4), mode, ByteOrder::big);
    put_tag(prom, 0x68, "ip27conf");
    return prom;
}

const test::Registration mode_bits{
    "ip27.reset_config_from_flash", [](test::Context& t) {
        // Bits 24:0 of the flash mode word, with the hardwired 32 KB cache fields.
        const auto prom = prom_with_mode_bits(0x1211'a785);
        t.check_equal(ip27::prom_mode_bits(prom).value_or(0), std::uint32_t{0x1211'a785});
        t.check_equal(ip27::reset_config_word(Ip27Config{}, prom), std::uint32_t{0x6c11'a785});
        Ip27Machine machine{small_config(), prom};
        t.check_equal(machine.cpu().mfc0(mips::cp0::config), std::uint64_t{0x6c11'a785});
        // The flash is also visible at the base of LBOOT.
        machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        t.check_equal(machine.cpu().load(0x9000'0000'1000'0064, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0x1211'a785});
        // And at the base of RBOOT, the node's boot PROM through its HSPEC space (observed:
        // the PROM reads the ip27conf clock at RBOOT + 0x70).
        t.check_equal(machine.cpu().load(0x9000'0000'3000'0064, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0x1211'a785});
        // Without the block the configured value is used.
        t.check_equal(ip27::reset_config_word(Ip27Config{}, program({nop()})),
                      Ip27Config{}.cpu.config);
        // A mode word that selects little-endian memory is a configuration error.
        t.check(!ip27::validate(Ip27Config{}, prom_with_mode_bits(0x1211'2785)), "little-endian");
    }};

const test::Registration first_fetch{
    "ip27.reset.first_fetch", [](test::Context& t) {
        // The reset vector reaches the start of the flash (IP27.adoc "Boot PROM").
        const auto prom = program({lui(t0, 0x1234), ori(t0, t0, 0x5678)});
        Ip27Machine machine{small_config(), prom};
        t.check_equal(machine.cpu().state().pc, mips::Cpu::reset_vector);
        machine.run(2);
        t.check_equal(machine.cpu().state().gpr[t0], std::uint64_t{0x1234'5678});
        t.check_equal(machine.cpu().state().pc, mips::Cpu::reset_vector + 8);
    }};

const test::Registration address_windows{
    "ip27.address_windows", [](test::Context& t) {
        const auto prom = program({0x0102'0304, nop()});
        Ip27Machine machine{small_config(), prom};
        mips::Cpu& cpu = machine.cpu();
        cpu.mtc0(mips::cp0::status, mips::status::kx);

        // CAC, UNCAC, and HSPEC UALIAS reach the same memory (IP27.adoc "Memory").
        t.check(cpu.store(0xa800'0000'0000'1000, AccessWidth::bits64, 0x1122'3344'5566'7788)
                    .has_value());
        t.check_equal(cpu.load(0x9600'0000'0000'1000, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x1122'3344'5566'7788});
        t.check_equal(cpu.load(0x9000'0000'0000'1000, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x1122'3344'5566'7788});

        // HSPEC 0x1fc00000 is the flash; unprogrammed flash reads as 0xff.
        t.check_equal(cpu.load(0x9000'0000'1fc0'0000, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0x0102'0304});
        t.check_equal(cpu.load(0x9000'0000'1fc0'0100, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0xffff'ffff});
        // Cached 0x1fc00000 is memory, not the PROM; with 16 MiB installed nothing answers.
        const auto cached = cpu.load(0xa800'0000'1fc0'0000, AccessWidth::bits32);
        t.check(!cached && cached.error().code == mips::ExceptionCode::bus_error_data);

        // The flash is not writable in M2: the write is lost and takes no exception.
        t.check(cpu.store(0x9000'0000'1fc0'0000, AccessWidth::bits32, 0).has_value());
        t.check_equal(cpu.load(0x9000'0000'1fc0'0000, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0x0102'0304});
    }};

const test::Registration hub_windows{
    "ip27.hub_windows", [](test::Context& t) {
        // The Hub answers at IALIAS and at the remote-Hub alias for NASID 0 (IP27.adoc "IO").
        Ip27Machine machine{small_config(), program({nop()})};
        machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        // MD_SLOTID_USTAT: FPROMRDY and slot ID 7, node slot n1 (the default).
        t.check_equal(machine.cpu().load(0x9200'0000'0122'0048, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x17});
        t.check_equal(machine.cpu().load(0x9200'0000'01a2'0048, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x17});
    }};

const test::Registration virtual_time{
    "ip27.virtual_time_follows_cycles", [](test::Context& t) {
        const auto prom = program({beq(zero, zero, -4), nop()});
        Ip27Machine machine{small_config(), prom};
        machine.run(1000);
        t.check_equal(machine.cpu().cycles(), std::uint64_t{1000});
        t.check_equal(machine.now(), time_at_cycles(1000, small_config().cpu_clock));
    }};

const test::Registration determinism{
    "ip27.deterministic_execution", [](test::Context& t) {
        // A program that touches memory, the TLB, and CP0; two machines must agree exactly.
        const auto prom = program({lui(t0, 0xa800), dsll32(t0, t0, 0), addiu(t1, zero, 7),
                                   sd(t1, 0x100, t0), ld(t2, 0x100, t0), mfc0(t3, mips::cp0::count),
                                   tlbwr(), daddu(t1, t1, t2), beq(zero, zero, -24), nop()});
        Ip27Machine first{small_config(), prom};
        Ip27Machine second{small_config(), prom};
        first.cpu().mtc0(mips::cp0::status, mips::status::kx);
        second.cpu().mtc0(mips::cp0::status, mips::status::kx);
        first.run(5000);
        second.run(5000);
        t.check(first.cpu().state().gpr == second.cpu().state().gpr, "same registers");
        t.check_equal(first.cpu().state().pc, second.cpu().state().pc);
        t.check_equal(first.cpu().mfc0(mips::cp0::random), second.cpu().mfc0(mips::cp0::random));
        t.check_equal(first.now(), second.now());
    }};

const test::Registration event_boundaries{
    "ip27.events_run_at_cycle_boundaries", [](test::Context& t) {
        // Scheduler invariant (IR.adoc): an event runs at the exact cycle its deadline
        // converts to, between instructions, and guest RAM it writes (as DMA does, through
        // a writable span) is seen by the next instruction and not before. The loop loads
        // the word at physical 0x100 every four cycles, from cycle 2, and counts what it
        // sees.
        const auto prom = program({lui(t0, 0xa800), dsll32(t0, t0, 0), lw(t1, 0x100, t0),
                                   daddu(t2, t2, t1), beq(zero, zero, -12), nop()});
        Ip27Machine machine{small_config(), prom};
        machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        std::uint64_t fired_at = 0;
        auto& scheduler = machine.scheduler();
        const EventId dma = scheduler.add_event("test.dma", [&] {
            fired_at = machine.cpu().cycles();
            const auto bytes = machine.bus().writable_memory_bytes(PhysicalAddress{0x100}, 4);
            if (t.check_equal(bytes.size(), std::size_t{4})) {
                store_unsigned(bytes, 1, ByteOrder::big);
            }
        });
        // Cycle 50 is a load (2 + 4 * 12).
        scheduler.schedule_at(dma, time_at_cycles(50, small_config().cpu_clock));
        machine.run(200);
        t.check_equal(fired_at, std::uint64_t{50});
        // Loads at cycles 50, 54, ..., 198 see the write; their DADDUs run by cycle 199.
        t.check_equal(machine.cpu().state().gpr[t2], std::uint64_t{38});
    }};

const test::Registration snapshot{
    "ip27.snapshot_resumes_exactly", [](test::Context& t) {
        // Saving at cycle N and resuming must match running straight through.
        const auto prom = program({lui(t0, 0xa800), dsll32(t0, t0, 0), addiu(t1, zero, 7),
                                   sd(t1, 0x100, t0), ld(t2, 0x100, t0), mfc0(t3, mips::cp0::count),
                                   tlbwr(), daddu(t1, t1, t2), beq(zero, zero, -24), nop()});
        Ip27Machine straight{small_config(), prom};
        straight.cpu().mtc0(mips::cp0::status, mips::status::kx);
        straight.run(5000);

        Ip27Machine first{small_config(), prom};
        first.cpu().mtc0(mips::cp0::status, mips::status::kx);
        first.run(2000);
        const auto image = StateImage::deserialize(first.save_state().serialize());
        if (!t.check(image.has_value())) {
            return;
        }
        Ip27Machine resumed{small_config(), prom};
        t.check(resumed.load_state(*image).has_value());
        resumed.run(3000);
        t.check(resumed.cpu().state().gpr == straight.cpu().state().gpr, "same registers");
        t.check_equal(resumed.cpu().state().pc, straight.cpu().state().pc);
        t.check_equal(resumed.cpu().cycles(), straight.cpu().cycles());
        t.check_equal(resumed.cpu().mfc0(mips::cp0::count), straight.cpu().mfc0(mips::cp0::count));
        t.check(resumed.now() == straight.now(), "same virtual time");

        // Snapshots refuse a different configuration.
        Ip27Config other = small_config();
        other.memory_bytes = 32u << 20;
        Ip27Machine mismatched{other, prom};
        t.check(!mismatched.load_state(*image).has_value());
    }};

const test::Registration back_door{
    "ip27.back_door_directory", [](test::Context& t) {
        Ip27Machine machine{small_config(), program({nop()})};
        machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        // PRM Table 3-4: `L:0x2100560` is 0x90000000c08402a0, the directory entry (low
        // doubleword) of that memory address. The default directory is standard: 16 bits.
        t.check(machine.cpu()
                    .store(0x9000'0000'c084'02a0, AccessWidth::bits64, 0x8000'0000'0001'5555)
                    .has_value());
        t.check_equal(machine.cpu().load(0x9000'0000'c084'02a0, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x5555});
    }};

const test::Registration controller{
    "ip27.system_controller_configuration", [](test::Context& t) {
        Ip27Config config;
        config.module_number = 3;
        const auto nvram = ip27::initial_controller_nvram(config);
        // The magic byte the PROM checks, and the module number it reads (slate's layout).
        t.check_equal(std::to_integer<int>(nvram[0x700]), 0x37);
        t.check_equal(std::to_integer<int>(nvram[0x708]), 3);
        t.check_equal(std::to_integer<int>(nvram[0x000]), 0xff); // otherwise erased
        // Node slots n1-n4 have slot IDs 7-4.
        t.check_equal(ip27::node_slot_id(1), 7u);
        t.check_equal(ip27::node_slot_id(4), 4u);
        config.node_slot = 5;
        t.check(!ip27::validate(config, program({nop()})), "no slot n5");
    }};

const test::Registration crosstalk{
    "ip27.crosstalk_windows", [](test::Context& t) {
        Ip27Machine machine{small_config(), program({nop()})};
        machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        // IO widget 0 is the crossbow; its WIDGET_ID at offset 4.
        t.check_equal(machine.cpu().load(0x9200'0000'0000'0004, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0x4000'0001});
        // The n1 Hub is on port 9: link 9 alive, and widget 9 answers with the Hub's ID.
        t.check_equal(machine.cpu().load(0x9200'0000'0000'0154, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0x8000'0000});
        t.check_equal(machine.cpu().load(0x9200'0000'0900'0004, AccessWidth::bits32).value_or(0) >>
                              12 &
                          0xffff,
                      std::uint64_t{0xc101});
        t.check_equal(ip27::hub_xbow_port(2), 0xau);
    }};

} // namespace
