#include "arch/mips/assembler.hpp"
#include "support/test.hpp"

#include <ultraviolent/core/byte_order.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/virtual_time.hpp>
#include <ultraviolent/machines/ip27/hub.hpp>
#include <ultraviolent/machines/ip27/ip27_machine.hpp>
#include <ultraviolent/machines/ip27/prom_image.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <time.h>

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
        // The part number ends the 25-character field: its last six characters in page 1.
        t.check(text(11, 13) == std::string(13, ' '));
        t.check(text(24, 6) == "030-07");
        t.check(text(32, 6) == "34-003");
        // IRIX joins the two pieces, dropping spaces, and writes one space for a piece with
        // no text (IP27.adoc "NICs"); a part number left in page 0 alone read "030-0734-003 ".
        const auto irix_text = [](std::string_view piece) {
            std::string kept;
            for (const char c : piece) {
                if (c != ' ' && c != '\xff') {
                    kept += c;
                }
            }
            return kept.empty() ? std::string{" "} : kept;
        };
        t.check_equal(irix_text(text(11, 19)) + irix_text(text(32, 6)), std::string{"030-0734-003"});
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
        // MD_SLOTID_USTAT: FPROMRDY and slot ID 3, deskside node slot n1 (the default).
        t.check_equal(machine.cpu().load(0x9200'0000'0122'0048, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x13});
        t.check_equal(machine.cpu().load(0x9200'0000'01a2'0048, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x13});
        // A rack module's n1 has slot ID 7.
        Ip27Config rack = small_config();
        rack.module = ip27::ModuleType::rack;
        Ip27Machine rack_machine{rack, program({nop()})};
        rack_machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        t.check_equal(
            rack_machine.cpu().load(0x9200'0000'0122'0048, AccessWidth::bits64).value_or(0),
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
        machine.set_execution_engine(mips::ExecutionEngine::reference);
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

const test::Registration tier0_engine{
    "ip27.tier0_engine_matches_reference", [](test::Context& t) {
        // The same program on both engines: identical architectural state (IR.adoc "Stages").
        const auto prom = program({lui(t0, 0xa800), dsll32(t0, t0, 0), addiu(t1, zero, 7),
                                   sd(t1, 0x100, t0), ld(t2, 0x100, t0), mfc0(t3, mips::cp0::count),
                                   tlbwr(), daddu(t1, t1, t2), beq(zero, zero, -24), nop()});
        Ip27Machine reference{small_config(), prom};
        Ip27Machine tier0{small_config(), prom};
        reference.set_execution_engine(mips::ExecutionEngine::reference);
        tier0.set_execution_engine(mips::ExecutionEngine::tier0);
        for (Ip27Machine* machine : {&reference, &tier0}) {
            machine->cpu().mtc0(mips::cp0::status, mips::status::kx);
            machine->run(5000);
        }
        t.check(reference.cpu().capture() == tier0.cpu().capture(), "same CPU state");
        t.check(reference.now() == tier0.now(), "same virtual time");
        t.check(tier0.block_statistics().operations > tier0.block_statistics().entries,
                "blocks of more than one operation ran");
    }};

const test::Registration tier0_event_boundaries{
    "ip27.tier0_events_run_at_cycle_boundaries", [](test::Context& t) {
        // ip27.events_run_at_cycle_boundaries on the tier-0 engine.
        const auto prom = program({lui(t0, 0xa800), dsll32(t0, t0, 0), lw(t1, 0x100, t0),
                                   daddu(t2, t2, t1), beq(zero, zero, -12), nop()});
        Ip27Machine machine{small_config(), prom};
        machine.set_execution_engine(mips::ExecutionEngine::tier0);
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
        scheduler.schedule_at(dma, time_at_cycles(50, small_config().cpu_clock));
        machine.run(200);
        t.check_equal(fired_at, std::uint64_t{50});
        t.check_equal(machine.cpu().state().gpr[t2], std::uint64_t{38});
    }};

const test::Registration tier1_engine{
    "ip27.tier1_engine_matches_reference", [](test::Context& t) {
        // Tier 1 (JIT.adoc) on the machine: the same CPU state and time as the reference.
        const auto prom = program({lui(t0, 0xa800), dsll32(t0, t0, 0), addiu(t1, zero, 7),
                                   sd(t1, 0x100, t0), ld(t2, 0x100, t0), mfc0(t3, mips::cp0::count),
                                   tlbwr(), daddu(t1, t1, t2), beq(zero, zero, -24), nop()});
        Ip27Machine reference{small_config(), prom};
        Ip27Machine tier1{small_config(), prom};
        reference.set_execution_engine(mips::ExecutionEngine::reference);
        if (!t.check(tier1.set_execution_engine(mips::ExecutionEngine::tier1), "tier 1 here")) {
            return;
        }
        for (Ip27Machine* machine : {&reference, &tier1}) {
            machine->cpu().mtc0(mips::cp0::status, mips::status::kx);
            machine->run(5000);
        }
        t.check(reference.cpu().capture() == tier1.cpu().capture(), "same CPU state");
        t.check(reference.now() == tier1.now(), "same virtual time");
        t.check(tier1.native_code()->statistics().blocks_translated > 0, "native code ran");
    }};

const test::Registration tier1_event_boundaries{
    "ip27.tier1_events_run_at_cycle_boundaries", [](test::Context& t) {
        // ip27.events_run_at_cycle_boundaries on tier 1.
        const auto prom = program({lui(t0, 0xa800), dsll32(t0, t0, 0), lw(t1, 0x100, t0),
                                   daddu(t2, t2, t1), beq(zero, zero, -12), nop()});
        Ip27Machine machine{small_config(), prom};
        if (!t.check(machine.set_execution_engine(mips::ExecutionEngine::tier1), "tier 1 here")) {
            return;
        }
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
        scheduler.schedule_at(dma, time_at_cycles(50, small_config().cpu_clock));
        machine.run(200);
        t.check_equal(fired_at, std::uint64_t{50});
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
        // And another module type or node slot: the guest saw its slot ID and Crossbow ports.
        // Snapshots without these fields were taken in rack slot n1.
        Ip27Config rack = small_config();
        rack.module = ip27::ModuleType::rack;
        Ip27Machine in_rack{rack, prom};
        t.check(!in_rack.load_state(*image).has_value(), "a rack module refuses it");
        Ip27Machine rack_first{rack, prom};
        rack_first.run(100);
        Ip27Machine deskside{small_config(), prom};
        t.check(!deskside.load_state(rack_first.save_state()).has_value(),
                "a deskside module refuses a rack snapshot");
        Ip27Config n2 = small_config();
        n2.node_slot = 2;
        Ip27Machine n2_first{n2, prom};
        n2_first.run(100);
        Ip27Machine n1{small_config(), prom};
        t.check(!n1.load_state(n2_first.save_state()).has_value(), "another node slot is refused");
        // And another processor: the guest read its PRId.
        Ip27Config r12000 = small_config();
        r12000.processor = ip27::ProcessorType::r12000;
        r12000.cpu.processor_id = r12000.cpu.fpu_id = ip27::r12000_identity;
        Ip27Machine r12000_machine{r12000, prom};
        t.check(!r12000_machine.load_state(*image).has_value(), "an R12000 refuses it");
    }};

// The R12000 configuration (SMP.adoc "S6: IP31 / R12000"): the CPUs report slate's PRId and
// FPU implementation, and the model is the R12000.
const test::Registration r12000_identity{
    "ip27.r12000_identity", [](test::Context& t) {
        Ip27Config config = small_config();
        config.cpus = 2;
        config.processor = ip27::ProcessorType::r12000;
        config.cpu.processor_id = config.cpu.fpu_id = ip27::r12000_identity;
        Ip27Machine machine{config, program({beq(zero, zero, -4), nop()})};
        for (unsigned i = 0; i < 2; ++i) {
            t.check_equal(machine.cpu(i).mfc0(mips::cp0::prid), std::uint64_t{0x0e35});
            t.check_equal(machine.cpu(i).fpu_id(), std::uint32_t{0x0e35});
            t.check(machine.cpu(i).model().name() == "R12000");
        }
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
        // Read as memory (the host page cache now holds the page): a later write is seen.
        t.check(
            machine.cpu().store(0x9000'0000'c084'02a0, AccessWidth::bits64, 0x1'aaaa).has_value());
        t.check_equal(machine.cpu().load(0x9000'0000'c084'02a0, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0xaaaa});
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
        // Rack node slots n1-n4 have slot IDs 7-4; deskside n1 and n2 have 3 and 2.
        t.check_equal(ip27::node_slot_id(ip27::ModuleType::rack, 1), 7u);
        t.check_equal(ip27::node_slot_id(ip27::ModuleType::rack, 4), 4u);
        t.check_equal(ip27::node_slot_id(ip27::ModuleType::deskside, 1), 3u);
        t.check_equal(ip27::node_slot_id(ip27::ModuleType::deskside, 2), 2u);
        config.node_slot = 5;
        t.check(!ip27::validate(config, program({nop()})), "no slot n5");
        config.node_slot = 3;
        config.module = ip27::ModuleType::deskside;
        t.check(!ip27::validate(config, program({nop()})), "a deskside has n1 and n2");
        config.node_slot = 1;
        config.nodes = 2;
        t.check(ip27::validate(config, program({nop()})).has_value(), "two nodes, deskside");
        config.module = ip27::ModuleType::rack;
        t.check(!ip27::validate(config, program({nop()})), "two nodes need one Crossbow");
    }};

const test::Registration crosstalk{
    "ip27.crosstalk_windows", [](test::Context& t) {
        Ip27Machine machine{small_config(), program({nop()})};
        machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        // IO widget 0 is the crossbow; its WIDGET_ID at offset 4.
        t.check_equal(machine.cpu().load(0x9200'0000'0000'0004, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0x4000'0001});
        // The n1 Hub is on port 0xa (deskside, the default): link 0xa alive, and widget 0xa
        // answers with the Hub's ID.
        t.check_equal(machine.cpu().load(0x9200'0000'0000'0194, AccessWidth::bits32).value_or(0),
                      std::uint64_t{0x8000'0000});
        t.check_equal(machine.cpu().load(0x9200'0000'0a00'0004, AccessWidth::bits32).value_or(0) >>
                              12 &
                          0xffff,
                      std::uint64_t{0xc101});
        t.check_equal(ip27::hub_xbow_port(ip27::ModuleType::rack, 1), 0x9u);
        t.check_equal(ip27::hub_xbow_port(ip27::ModuleType::rack, 2), 0xau);
        // slate's deskside: n1 on 0xa, n2 on 9, the BaseIO (io1) on 15.
        t.check_equal(ip27::hub_xbow_port(ip27::ModuleType::deskside, 1), 0xau);
        t.check_equal(ip27::hub_xbow_port(ip27::ModuleType::deskside, 2), 0x9u);
        t.check_equal(ip27::baseio_port(ip27::ModuleType::deskside), 0xfu);
        t.check_equal(ip27::baseio_port(ip27::ModuleType::rack), 0x8u);
    }};

Ip27Config two_cpu_config(std::uint64_t slice = 180) {
    Ip27Config config = small_config();
    config.cpus = 2;
    config.cpu_slice = slice;
    return config;
}

void kernel_64(Ip27Machine& machine) {
    for (unsigned slice = 0; slice < 2; ++slice) {
        machine.cpu(slice).mtc0(mips::cp0::status, mips::status::kx);
    }
}

std::uint64_t memory_word(Ip27Machine& machine, std::uint64_t address) {
    return machine.bus().read(PhysicalAddress{address}, AccessWidth::bits64).value_or(~0ull);
}

// Both CPUs run the same reset code; PI_CPU_NUM, read through each CPU's own Hub port,
// tells them apart (IP27.adoc "Two CPUs on a Hub"). Each stores 0x10 + its slice at 0x100 +
// 8 * slice.
const test::Registration two_cpu_slices{
    "ip27.two_cpus.pi_cpu_num_per_cpu", [](test::Context& t) {
        const auto prom = program({lui(t0, 0x9200), dsll32(t0, t0, 0), lui(t2, 0x0100),
                                   daddu(t0, t0, t2), ld(t1, 0x20, t0), lui(t3, 0xa800),
                                   dsll32(t3, t3, 0), dsll(t4, t1, 3), daddu(t3, t3, t4),
                                   addiu(t5, t1, 0x10), sd(t5, 0x100, t3), beq(zero, zero, -4),
                                   nop()});
        Ip27Machine machine{two_cpu_config(), prom};
        kernel_64(machine);
        machine.run(200);
        t.check(machine.cpu(0).cycles() >= 200 && machine.cpu(1).cycles() >= 200,
                "both CPUs ran the cycles");
        t.check_equal(memory_word(machine, 0x100), std::uint64_t{0x10});
        t.check_equal(memory_word(machine, 0x108), std::uint64_t{0x11});
    }};

// Both CPUs increment one counter with LL/SC and count their successes in t6: no update is
// lost (each CPU may have stored without counting yet), at the strict slice (the CPUs
// alternate every cycle) and others, and two runs agree exactly (ADR-023).
const auto counter_prom = program({lui(t3, 0xa800), dsll32(t3, t3, 0), daddiu(t3, t3, 0x200),
                                   ll(t1, 0, t3), addiu(t1, t1, 1), sc(t1, 0, t3),
                                   beq(t1, zero, -16), nop(), daddiu(t6, t6, 1),
                                   beq(zero, zero, -28), nop()});

const test::Registration two_cpu_counter{
    "ip27.two_cpus.ll_sc_counter", [](test::Context& t) {
        for (const std::uint64_t slice : {std::uint64_t{1}, std::uint64_t{7}, std::uint64_t{180}}) {
            Ip27Machine first{two_cpu_config(slice), counter_prom};
            Ip27Machine second{two_cpu_config(slice), counter_prom};
            for (Ip27Machine* machine : {&first, &second}) {
                kernel_64(*machine);
                machine->run(20000);
            }
            const std::uint64_t a = first.cpu(0).state().gpr[t6];
            const std::uint64_t b = first.cpu(1).state().gpr[t6];
            t.check(a > 0 && b > 0, "both CPUs make progress");
            const std::uint64_t counter = memory_word(first, 0x200) >> 32;
            t.check(counter >= a + b && counter <= a + b + 2, "no lost update");
            t.check(first.cpu(0).capture() == second.cpu(0).capture() &&
                        first.cpu(1).capture() == second.cpu(1).capture(),
                    "two runs agree");
        }
    }};

// A two-CPU snapshot resumes exactly where the run would have gone.
const test::Registration two_cpu_snapshot{
    "ip27.two_cpus.snapshot", [](test::Context& t) {
        Ip27Machine straight{two_cpu_config(5), counter_prom};
        Ip27Machine saved{two_cpu_config(5), counter_prom};
        kernel_64(straight);
        kernel_64(saved);
        straight.run(6000);
        saved.run(3001);
        const StateImage image = saved.save_state();
        Ip27Machine resumed{two_cpu_config(5), counter_prom};
        if (!t.check(resumed.load_state(image).has_value(), "the snapshot loads")) {
            return;
        }
        resumed.run(6000 - std::min(resumed.cpu(0).cycles(), resumed.cpu(1).cycles()));
        t.check(straight.cpu(0).capture() == resumed.cpu(0).capture(), "CPU A");
        t.check(straight.cpu(1).capture() == resumed.cpu(1).capture(), "CPU B");
        t.check_equal(memory_word(straight, 0x200), memory_word(resumed, 0x200));
        Ip27Machine one{small_config(), counter_prom};
        t.check(!one.load_state(image).has_value(), "a one-CPU machine refuses it");
    }};

// Each CPU sets KX; the first to find the marker at physical 0x400 clear sets it and requests a
// Hub local reset through NI_PORT_RESET, as the PROM does early (IP27.adoc "Hub local reset");
// after the reset both count in t6. CPU A goes first on the tie at power-on, so it requests the
// reset while B is behind: the reset event is due at once, inside A's slice.
const auto local_reset_prom =
    program({lui(t7, 0x40), ori(t7, t7, 0x80), mtc0(t7, mips::cp0::status), lui(t3, 0xa800),
             dsll32(t3, t3, 0), ld(t1, 0x400, t3), bne(t1, zero, 36), nop(), ori(t2, zero, 1),
             sd(t2, 0x400, t3), lui(t5, 0x9200), dsll32(t5, t5, 0), lui(t2, 0x0160),
             daddu(t5, t5, t2), ori(t4, zero, 1), sd(t4, 8, t5), daddiu(t6, t6, 1),
             beq(zero, zero, -8), nop()});

// Observing a run (--probe or --trace cpu steps one instruction at a time) does not change
// the order in which the CPUs run (ADR-023): a slice ends only where it is cpu_slice ahead,
// even when an event makes the engine return early.
const test::Registration two_cpu_observed{
    "ip27.two_cpus.observation_keeps_the_schedule", [](test::Context& t) {
        for (const std::uint64_t slice : {std::uint64_t{7}, std::uint64_t{50}}) {
            Ip27Machine plain{two_cpu_config(slice), local_reset_prom};
            Ip27Machine observed{two_cpu_config(slice), local_reset_prom};
            observed.add_pc_probe(0x4); // never reached: only makes the machine step
            plain.run(3000);
            observed.run(3000);
            t.check_equal(memory_word(plain, 0x400), std::uint64_t{1});
            t.check_equal(plain.cpu(0).cycles(), observed.cpu(0).cycles());
            t.check_equal(plain.cpu(1).cycles(), observed.cpu(1).cycles());
            t.check(plain.cpu(0).capture() == observed.cpu(0).capture() &&
                        plain.cpu(1).capture() == observed.cpu(1).capture(),
                    "same state with and without observation");
        }
    }};

// A Hub local reset releases both CPUs together (IP27.adoc "Two CPUs on a Hub"): A requests it
// while B is behind, and afterwards both Count registers started from 0 at the same cycle,
// whatever the slice.
const test::Registration two_cpu_local_reset{
    "ip27.two_cpus.leave_local_reset_together", [](test::Context& t) {
        for (const std::uint64_t slice : {std::uint64_t{1}, std::uint64_t{50}, std::uint64_t{2000}}) {
            Ip27Machine machine{two_cpu_config(slice), local_reset_prom};
            machine.run(6000);
            t.check_equal(memory_word(machine, 0x400), std::uint64_t{1});
            // The cycle at which each CPU's Count was 0 (Count runs at half the cycle rate).
            const auto released = [](mips::Cpu& cpu) {
                return static_cast<std::uint32_t>(cpu.cycles() >> 1) -
                       static_cast<std::uint32_t>(cpu.mfc0(mips::cp0::count));
            };
            t.check_equal(released(machine.cpu(0)), released(machine.cpu(1)));
            t.check(machine.cpu(0).state().gpr[t6] > 0 && machine.cpu(1).state().gpr[t6] > 0,
                    "both CPUs run after the reset");
        }
    }};

// A CPU's reads of PI_RT_COUNT follow virtual time at 1.25 MHz (IP27.adoc "Hub"): two reads
// 1,000 loop iterations (about 3,000 cycles, 16.7 us at 180 MHz) apart, stored at physical
// 0x300 and 0x308. The machine answers this read directly from the Hub.
const test::Registration rt_count_by_cpu{
    "ip27.rt_count_read_by_cpu", [](test::Context& t) {
        std::vector<std::uint32_t> words(32, nop());
        const std::initializer_list<std::uint32_t> code = {
            // t0 = the Hub's registers, t2 = t0 + 0x30000.
            lui(t0, 0x9200), dsll32(t0, t0, 0), lui(t2, 0x0100), daddu(t0, t0, t2),
            lui(t2, 3), daddu(t2, t0, t2), ld(t1, 0x100, t2),
            // 1,000 iterations, then the second read.
            addiu(t7, zero, 1000), addiu(t7, t7, -1), bne(t7, zero, -8), nop(),
            ld(t3, 0x100, t2),
            lui(t5, 0xa800), dsll32(t5, t5, 0), sd(t1, 0x300, t5), sd(t3, 0x308, t5),
            beq(zero, zero, -4), nop()};
        std::ranges::copy(code, words.begin());
        std::vector<std::byte> prom(words.size() * 4);
        for (std::size_t w = 0; w < words.size(); ++w) {
            store_unsigned(std::span{prom}.subspan(w * 4, 4), words[w], ByteOrder::big);
        }
        Ip27Machine machine{small_config(), prom};
        machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        machine.run(5000);
        const std::uint64_t first = memory_word(machine, 0x300);
        const std::uint64_t second = memory_word(machine, 0x308);
        t.check(second - first >= 20 && second - first <= 21, "about 20.8 ticks apart");
    }};

// Back-to-back reads of PI_RT_COUNT, which one CPU's machine answers from a count it
// remembers until the next tick (JIT.adoc "Device time"): 400 reads into memory at physical
// 0x1000, with PI_RT_COUNT written to 5,000 after the 200th. Between reads the count moves by
// 0 or 1, at the 1.25 MHz rate, and the write takes effect at once.
const test::Registration rt_count_back_to_back{
    "ip27.rt_count_back_to_back", [](test::Context& t) {
        std::vector<std::uint32_t> words(48, nop());
        const std::initializer_list<std::uint32_t> code = {
            // t2 = the Hub's registers + 0x30000; t5 = physical 0x1000 (uncached); t7 = 400.
            lui(t0, 0x9200), dsll32(t0, t0, 0), lui(t2, 0x0100), daddu(t0, t0, t2), lui(t2, 3),
            daddu(t2, t0, t2), lui(t5, 0xa800), dsll32(t5, t5, 0), ori(t5, t5, 0x1000),
            addiu(t7, zero, 400), addiu(t6, zero, 5000),
            // Write the count once first, so that the write in the loop goes through the CPU's
            // device page and only the count's stamp ends a held read (Hub::rt_sequence).
            sd(zero, 0x100, t2),
            // Loop (index 12): read, store, step; at 200 left, write the count.
            ld(t1, 0x100, t2), sd(t1, 0, t5), daddiu(t5, t5, 8), addiu(t7, t7, -1),
            addiu(t3, t7, -200), bne(t3, zero, 8), nop(), sd(t6, 0x100, t2), bne(t7, zero, -36),
            nop(), beq(zero, zero, -4), nop()};
        std::ranges::copy(code, words.begin());
        std::vector<std::byte> prom(words.size() * 4);
        for (std::size_t w = 0; w < words.size(); ++w) {
            store_unsigned(std::span{prom}.subspan(w * 4, 4), words[w], ByteOrder::big);
        }
        Ip27Machine machine{small_config(), prom};
        machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        machine.run(20000);
        std::vector<std::uint64_t> reads;
        for (unsigned i = 0; i < 400; ++i) {
            reads.push_back(memory_word(machine, 0x1000 + 8 * i));
        }
        bool steps = true;
        for (unsigned i = 1; i < 400; ++i) {
            if (i != 200 && reads[i] - reads[i - 1] > 1) {
                steps = false;
            }
        }
        t.check(steps, "the count moves by 0 or 1 between reads");
        t.check(reads[200] >= 5000 && reads[200] <= 5001, "the write takes effect at once");
        // About 7 cycles per iteration: 200 reads span about 1,400 cycles, 9.7 ticks.
        t.check(reads[199] - reads[0] >= 6 && reads[199] - reads[0] <= 14, "ticks at 1.25 MHz");
        t.check(reads[399] - reads[200] >= 6 && reads[399] - reads[200] <= 14,
                "ticks at 1.25 MHz after the write");
    }};

// NI_STATUS_REV_ID changes only through Hub writes, so with one CPU its reads hold for the
// run (Hub::read_has_no_effect) until any Hub register is written (Hub::write_sequence): a
// write of its settings in the middle of a polling loop shows at the next read.
const test::Registration ni_status_held{
    "ip27.ni_status_reads_hold_until_a_hub_write", [](test::Context& t) {
        std::vector<std::uint32_t> words(48, nop());
        const std::initializer_list<std::uint32_t> code = {
            // t2 = NI_STATUS_REV_ID; t5 = physical 0x1000 (uncached); t7 = 400; t6 = bit 17
            // (NSRI_REGIONSIZE, writable).
            lui(t0, 0x9200), dsll32(t0, t0, 0), lui(t2, 0x0100), daddu(t0, t0, t2), lui(t2, 0x60),
            daddu(t2, t0, t2), lui(t5, 0xa800), dsll32(t5, t5, 0), ori(t5, t5, 0x1000),
            addiu(t7, zero, 400), lui(t6, 2),
            // Write the register once first, so that the write in the loop goes through the
            // CPU's device page and only the Hub's stamp ends a held read.
            sd(zero, 0, t2),
            // Loop (index 12): read, store, step; at 200 left, write the settings.
            ld(t1, 0, t2), sd(t1, 0, t5), daddiu(t5, t5, 8), addiu(t7, t7, -1),
            addiu(t3, t7, -200), bne(t3, zero, 8), nop(), sd(t6, 0, t2), bne(t7, zero, -36),
            nop(), beq(zero, zero, -4), nop()};
        std::ranges::copy(code, words.begin());
        std::vector<std::byte> prom(words.size() * 4);
        for (std::size_t w = 0; w < words.size(); ++w) {
            store_unsigned(std::span{prom}.subspan(w * 4, 4), words[w], ByteOrder::big);
        }
        Ip27Machine machine{small_config(), prom};
        machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        machine.run(20000);
        constexpr std::uint64_t region_size = std::uint64_t{1} << 17;
        const std::uint64_t first = memory_word(machine, 0x1000);
        bool before = true;
        bool after = true;
        for (unsigned i = 0; i < 400; ++i) {
            const std::uint64_t read = memory_word(machine, 0x1000 + 8 * i);
            if (i < 200) {
                before = before && read == first;
            } else {
                after = after && read == (first | region_size);
            }
        }
        t.check((first & region_size) == 0, "the setting starts clear");
        t.check(before, "reads before the write are equal");
        t.check(after, "every read after the write sees it");
    }};

// A held PI_RT_COUNT read steps through later windows, one count every P = f / 1.25 MHz
// cycles (Ip27Machine::SynchronizedTarget::mmio_read_held), which is exact only if the
// counter's tick at cycle c, as the Hub converts it (the cycle's time, then that time's
// tick), is c / P: checked for the clocks that qualify, over the first cycles and around
// second boundaries.
const test::Registration rt_ticks_step{
    "ip27.rt_ticks_step_every_period", [](test::Context& t) {
        for (const std::uint64_t hertz :
             {180'000'000u, 195'000'000u, 250'000'000u, 300'000'000u, 400'000'000u, 997'500'000u}) {
            const CycleTime cpu_time{Frequency{hertz}};
            const std::uint64_t period = hertz / ip27::Hub::rtc_clock.hertz;
            bool exact = true;
            for (const std::uint64_t start : {std::uint64_t{0}, hertz - 2000, 7 * hertz - 2000}) {
                for (std::uint64_t c = start; c < start + 4000; ++c) {
                    if (cycles_at(cpu_time.time_at(c), ip27::Hub::rtc_clock) != c / period) {
                        exact = false;
                    }
                }
            }
            t.check(exact, std::format("ticks step every {} cycles at {} Hz", period, hertz));
        }
    }};

// End to end, held and stepped: a loop of six one-cycle instructions reads PI_RT_COUNT 2,000
// times (about 83 ticks of 144 cycles), so read i is at cycle c0 + 6i, and the counts must
// be floor((o + 6i) / 144) - floor(o / 144) past the first for one o in [0, 144).
const test::Registration rt_count_stepped{
    "ip27.rt_count_stepped_reads", [](test::Context& t) {
        std::vector<std::uint32_t> words(32, nop());
        const std::initializer_list<std::uint32_t> code = {
            // t2 = the Hub's registers + 0x30000; t5 = physical 0x1000 (uncached); t7 = 2000.
            lui(t0, 0x9200), dsll32(t0, t0, 0), lui(t2, 0x0100), daddu(t0, t0, t2), lui(t2, 3),
            daddu(t2, t0, t2), lui(t5, 0xa800), dsll32(t5, t5, 0), ori(t5, t5, 0x1000),
            addiu(t7, zero, 2000),
            // Loop (index 10): read, store, step.
            ld(t1, 0x100, t2), sd(t1, 0, t5), daddiu(t5, t5, 8), addiu(t7, t7, -1),
            bne(t7, zero, -20), nop(), beq(zero, zero, -4), nop()};
        std::ranges::copy(code, words.begin());
        std::vector<std::byte> prom(words.size() * 4);
        for (std::size_t w = 0; w < words.size(); ++w) {
            store_unsigned(std::span{prom}.subspan(w * 4, 4), words[w], ByteOrder::big);
        }
        Ip27Machine machine{small_config(), prom};
        machine.cpu().mtc0(mips::cp0::status, mips::status::kx);
        machine.run(20000);
        std::vector<std::uint64_t> reads;
        for (unsigned i = 0; i < 2000; ++i) {
            reads.push_back(memory_word(machine, 0x1000 + 8 * i) - memory_word(machine, 0x1000));
        }
        constexpr std::uint64_t period = 180'000'000 / 1'250'000;
        bool fits = false;
        for (std::uint64_t o = 0; o < period && !fits; ++o) {
            fits = true;
            for (std::uint64_t i = 0; i < reads.size() && fits; ++i) {
                fits = reads[i] == (o + 6 * i) / period - o / period;
            }
        }
        t.check(reads.back() >= 80, std::format("{} ticks", reads.back()));
        t.check(fits, "every read is the count of its cycle");
    }};

// An inter-processor interrupt is an ordinary Hub interrupt level (IP27.adoc "Hub
// interrupts"; LINUX intr.h CPU_ACTION_B = 11): B unmasks level 11 in INT_MASK0_B and enables
// IP2; A sets the level through PI_INT_PEND_MOD; B takes the interrupt at its BEV general
// exception vector (0xbfc00380), which records it at physical 0x300. A's mask is clear.
std::vector<std::byte> ipi_prom() {
    std::vector<std::uint32_t> words(230, nop());
    const auto at = [&](std::size_t index, std::initializer_list<std::uint32_t> code) {
        std::ranges::copy(code, words.begin() + static_cast<std::ptrdiff_t>(index));
    };
    // Both: t0 = the Hub's registers, t1 = PI_CPU_NUM; B goes to 15.
    at(0, {lui(t0, 0x9200), dsll32(t0, t0, 0), lui(t2, 0x0100), daddu(t0, t0, t2),
           ld(t1, 0x20, t0), bne(t1, zero, 36), nop()});
    // A: wait 200 iterations, set level 11 (0x100 | 11), loop.
    at(7, {addiu(t7, zero, 200), addiu(t7, t7, -1), bne(t7, zero, -8), nop(),
           ori(t3, zero, 0x10b), sd(t3, 0x90, t0), beq(zero, zero, -4), nop()});
    // B: INT_MASK0_B = bit 11; Status = BEV | IM2 | KX | IE; loop.
    at(15, {ori(t3, zero, 0x800), sd(t3, 0xb8, t0), lui(t4, 0x40), ori(t4, t4, 0x481),
            mtc0(t4, mips::cp0::status), beq(zero, zero, -4), nop()});
    // The BEV general exception vector, offset 0x380.
    at(0x380 / 4, {lui(t5, 0xa800), dsll32(t5, t5, 0), ori(t6, zero, 0x77),
                   sd(t6, 0x300, t5), beq(zero, zero, -4), nop()});
    std::vector<std::byte> prom(words.size() * 4);
    for (std::size_t w = 0; w < words.size(); ++w) {
        store_unsigned(std::span{prom}.subspan(w * 4, 4), words[w], ByteOrder::big);
    }
    return prom;
}

const test::Registration two_cpu_ipi{
    "ip27.two_cpus.interprocessor_interrupt", [](test::Context& t) {
        Ip27Machine machine{two_cpu_config(), ipi_prom()};
        kernel_64(machine);
        machine.run(2000);
        t.check_equal(memory_word(machine, 0x300), std::uint64_t{0x77});
        t.check_equal(machine.cpu(1).mfc0(mips::cp0::cause) >> 2 & 0x1f, std::uint64_t{0});
        t.check((machine.cpu(1).mfc0(mips::cp0::cause) & (1u << 10)) != 0, "B sees IP2");
        t.check((machine.cpu(0).mfc0(mips::cp0::cause) & (1u << 10)) == 0, "A does not");
    }};

// A PIO read of an absent crosstalk widget is a bus error for the CPU and a PIO error in its
// own slice's PI_ERR_STATUS0 (IP27.adoc "PIO errors"): the PROM's own signature read, widget
// 2 at 0x9200000002400208, made by CPU B. A records nothing.
const test::Registration pio_error_by_slice{
    "ip27.two_cpus.pio_error_status", [](test::Context& t) {
        std::vector<std::uint32_t> words(230, nop());
        const auto at = [&](std::size_t index, std::initializer_list<std::uint32_t> code) {
            std::ranges::copy(code, words.begin() + static_cast<std::ptrdiff_t>(index));
        };
        // Both: t0 = the Hub's registers, t1 = PI_CPU_NUM; A loops at 7, B goes on at 9.
        at(0, {lui(t0, 0x9200), dsll32(t0, t0, 0), lui(t2, 0x0100), daddu(t0, t0, t2),
               ld(t1, 0x20, t0), bne(t1, zero, 8), nop(), beq(zero, zero, -4), nop()});
        // B: read widget 2's small window at 0x400208.
        at(9, {lui(t5, 0x9200), dsll32(t5, t5, 0), lui(t6, 0x0240), ori(t6, t6, 0x208),
               daddu(t5, t5, t6), ld(t7, 0, t5), beq(zero, zero, -4), nop()});
        // The BEV general exception vector: loop.
        at(0x380 / 4, {beq(zero, zero, -4), nop()});
        std::vector<std::byte> prom(words.size() * 4);
        for (std::size_t w = 0; w < words.size(); ++w) {
            store_unsigned(std::span{prom}.subspan(w * 4, 4), words[w], ByteOrder::big);
        }
        Ip27Machine machine{two_cpu_config(), prom};
        kernel_64(machine);
        machine.run(2000);
        t.check_equal(machine.cpu(1).mfc0(mips::cp0::cause) >> 2 & 0x1f, std::uint64_t{7}); // DBE
        const auto hub = [&](std::uint64_t offset) {
            return machine.cpu(0)
                .load(0x9200'0000'0100'0000 + offset, AccessWidth::bits64)
                .value_or(~std::uint64_t{0});
        };
        t.check_equal((hub(0x450) >> 25) & ((std::uint64_t{1} << 37) - 1),
                      std::uint64_t{0x48'0041});
        t.check(hub(0x450) >> 63 == 1, "valid");
        t.check_equal(hub(0x430), std::uint64_t{0});
    }};

// Host-parallel CPUs (SMP.adoc "S4"): the same guest programs with each CPU on a host thread.
// Not deterministic, so the checks are the programs' own results.
const test::Registration parallel_ipi{
    "ip27.parallel.interprocessor_interrupt", [](test::Context& t) {
        Ip27Machine machine{two_cpu_config(), ipi_prom()};
        machine.set_parallel(true, 100);
        kernel_64(machine);
        machine.run(20000);
        t.check_equal(memory_word(machine, 0x300), std::uint64_t{0x77});
        t.check((machine.cpu(1).mfc0(mips::cp0::cause) & (1u << 10)) != 0, "B sees IP2");
        t.check((machine.cpu(0).mfc0(mips::cp0::cause) & (1u << 10)) == 0, "A does not");
    }};

// A local reset requested by one CPU reaches the other through its mailbox; both run again.
const test::Registration parallel_local_reset{
    "ip27.parallel.local_reset", [](test::Context& t) {
        Ip27Machine machine{two_cpu_config(), local_reset_prom};
        machine.set_parallel(true, 100);
        machine.run(200000);
        t.check_equal(memory_word(machine, 0x400), std::uint64_t{1});
        t.check(machine.cpu(0).state().gpr[t6] > 0 && machine.cpu(1).state().gpr[t6] > 0,
                "both CPUs run after the reset");
    }};

// A host hold (main's --realtime) makes the run wait for the host's clock, set from an event
// as the pacer does, and changes nothing the guest sees: one CPU, two taking turns, and two on
// host threads (whose results are the program's own, as above).
const test::Registration host_hold{"ip27.host_hold", [](test::Context& t) {
    const auto prom = program({lui(t0, 0xa800), dsll32(t0, t0, 0), daddiu(t1, t1, 1),
                               sd(t1, 0x100, t0), beq(zero, zero, -8), nop()});
    constexpr auto hold = std::chrono::milliseconds{100};
    // This thread's CPU time: a held run sleeps rather than spins.
    const auto thread_time = [] {
        timespec now{};
        ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
        return std::chrono::seconds{now.tv_sec} + std::chrono::nanoseconds{now.tv_nsec};
    };
    for (const unsigned mode : {0u, 1u, 2u}) {
        const unsigned cpus = mode == 0 ? 1 : 2;
        const auto make = [&] {
            auto machine =
                std::make_unique<Ip27Machine>(cpus == 1 ? small_config() : two_cpu_config(), prom);
            if (mode == 2) {
                machine->set_parallel(true, 100);
            }
            for (unsigned cpu = 0; cpu < cpus; ++cpu) {
                machine->cpu(cpu).mtc0(mips::cp0::status, mips::status::kx);
            }
            return machine;
        };
        auto held = make();
        auto& scheduler = held->scheduler();
        const EventId pace = scheduler.add_event("test.pace", [&] {
            held->hold_host_until(std::chrono::steady_clock::now() + hold);
        });
        scheduler.schedule_at(pace, time_at_cycles(1000, small_config().cpu_clock));
        const auto start = std::chrono::steady_clock::now();
        const auto start_cpu = thread_time();
        held->run(5000);
        t.check(std::chrono::steady_clock::now() - start >= hold, "the run waited");
        t.check(thread_time() - start_cpu < hold / 2, "asleep while it waited");
        for (unsigned cpu = 0; cpu < cpus; ++cpu) {
            t.check(held->cpu(cpu).cycles() >= 5000, "every CPU ran its cycles");
        }
        if (mode != 2) {
            auto free = make();
            free->run(5000);
            for (unsigned cpu = 0; cpu < cpus; ++cpu) {
                t.check(held->cpu(cpu).state().gpr == free->cpu(cpu).state().gpr,
                        "same registers");
                t.check_equal(held->cpu(cpu).cycles(), free->cpu(cpu).cycles());
            }
            t.check_equal(held->now(), free->now());
        }
    }
}};

} // namespace

namespace {

// Two nodes in a deskside module, as slate (SMP.adoc "S5: the second node").
Ip27Config two_node_config(unsigned cpus = 1) {
    Ip27Config config = small_config();
    config.module = ip27::ModuleType::deskside;
    config.nodes = 2;
    config.cpus = cpus;
    return config;
}

constexpr std::uint64_t ialias = 0x9200'0000'0100'0000;
constexpr std::uint64_t ni_status_rev_id = 0x60'0000;
constexpr std::uint64_t pi_int_pend_mod = 0x90;
constexpr std::uint64_t pi_int_pend0 = 0x98;
// REMOTE_HUB_ADDR(node, 0): widget 1's window + 0x800000 at the node's addresses.
constexpr std::uint64_t remote_hub(std::uint64_t node) {
    return ialias + (node << 32) + 0x80'0000;
}

std::uint64_t node_id_field(std::uint64_t nsri) {
    return nsri >> 8 & 0x1ff;
}

constexpr std::uint64_t nsri_link_up = std::uint64_t{1} << 29;
constexpr std::uint64_t nsri_down_reason = std::uint64_t{1} << 28;
constexpr std::uint64_t ni_scratch_reg1 = 0x60'0108;
constexpr std::uint64_t ni_vector_parms = 0x60'0200;
constexpr std::uint64_t ni_vector = 0x60'0208;
constexpr std::uint64_t ni_vector_data = 0x60'0210;
constexpr std::uint64_t ni_vector_status = 0x60'0300;
constexpr std::uint64_t ni_vector_read_data = 0x60'0310;
constexpr std::uint64_t ni_vector_clear = 0x60'0380;
constexpr std::uint64_t nvs_valid = std::uint64_t{1} << 63;
constexpr std::uint64_t nvs_overrun = std::uint64_t{1} << 62;

// NI_VECTOR_PARMS: PIO ID 49:40, write ID 39:32, address 19:3, type 1:0 (LINUX hubni.h).
constexpr std::uint64_t vector_parms(std::uint64_t pio_id, std::uint64_t write_id,
                                     std::uint64_t address, std::uint64_t type) {
    return pio_id << 40 | write_id << 32 | address | type;
}

// The NUMAlink cable: with two nodes both NIs report the link up; vector PIO reaches the
// neighbor's NI registers, and the response echoes the request (SMP.adoc "S5").
const test::Registration two_node_link{
    "ip27.two_nodes.link_and_vector_pio", [](test::Context& t) {
        Ip27Machine one{small_config(), program({nop()})};
        one.cpu().mtc0(mips::cp0::status, mips::status::kx);
        const std::uint64_t alone = one.cpu().load(ialias + ni_status_rev_id, AccessWidth::bits64).value_or(0);
        t.check((alone & nsri_link_up) == 0 && (alone & nsri_down_reason) != 0, "one node: down");

        Ip27Machine machine{two_node_config(), program({nop()})};
        mips::Cpu& a = machine.cpu(0);
        mips::Cpu& b = machine.cpu(2);
        a.mtc0(mips::cp0::status, mips::status::kx);
        b.mtc0(mips::cp0::status, mips::status::kx);
        const auto reg = [](mips::Cpu& cpu, std::uint64_t offset) {
            return cpu.load(ialias + offset, AccessWidth::bits64).value_or(0xbad);
        };
        const auto set = [&t](mips::Cpu& cpu, std::uint64_t offset, std::uint64_t value) {
            t.check(cpu.store(ialias + offset, AccessWidth::bits64, value).has_value());
        };
        t.check((reg(a, ni_status_rev_id) & (nsri_link_up | nsri_down_reason)) == nsri_link_up,
                "n1: up");
        t.check((reg(b, ni_status_rev_id) & (nsri_link_up | nsri_down_reason)) == nsri_link_up,
                "n2: up");

        // A read of the neighbor's NI_STATUS_REV_ID (NI offset 0), as the PROM's discovery.
        set(b, ni_status_rev_id, 1u << 8);
        set(a, ni_vector_read_data, 0xdead'beef'dead'beef);
        set(a, ni_vector, 0);
        set(a, ni_vector_parms, vector_parms(0x1c8, 0, 0, 0));
        t.check_equal(reg(a, ni_vector_status), nvs_valid | vector_parms(0x1c8, 0, 0, 0));
        t.check_equal(node_id_field(reg(a, ni_vector_read_data)), std::uint64_t{1});
        // NI_VECTOR_CLEAR returns the status and clears it.
        t.check_equal(reg(a, ni_vector_clear), nvs_valid | vector_parms(0x1c8, 0, 0, 0));
        t.check_equal(reg(a, ni_vector_status), std::uint64_t{0});

        // A write of the neighbor's NI_SCRATCH_REG1, as the global master hands out node IDs.
        set(a, ni_vector_data, (std::uint64_t{1} << 51) | 1);
        set(a, ni_vector_parms, vector_parms(0x7b, 5, 0x108, 1));
        t.check_equal(reg(a, ni_vector_status), nvs_valid | vector_parms(0x7b, 5, 0x108, 1));
        t.check_equal(reg(b, ni_scratch_reg1), (std::uint64_t{1} << 51) | 1);

        // An exchange returns the old value; a second response before a clear overruns.
        set(a, ni_vector_data, 7);
        set(a, ni_vector_parms, vector_parms(1, 0, 0x108, 3));
        t.check_equal(reg(a, ni_vector_status),
                      nvs_valid | nvs_overrun | vector_parms(1, 0, 0x108, 3));
        t.check_equal(reg(a, ni_vector_read_data), (std::uint64_t{1} << 51) | 1);
        t.check_equal(reg(b, ni_scratch_reg1), std::uint64_t{7});

        // An address with no register answers with an address error (type 4).
        (void)reg(a, ni_vector_clear);
        set(a, ni_vector_parms, vector_parms(2, 0, 0xf'fff8, 0));
        t.check_equal(reg(a, ni_vector_status) & 7, std::uint64_t{4});
    }};

// The node boards' MicroLANs (IP27.adoc "NICs"): each Hub reaches its node board's NIC; n1's
// bus also holds the module's. A Read ROM on n2, whose bus holds one part, returns the
// node board NIC's ROM ID: family 0x09, serial 6.
const test::Registration node_nics{
    "ip27.two_nodes.node_nics", [](test::Context& t) {
        Ip27Machine machine{two_node_config(), program({nop()})};
        mips::Cpu& b = machine.cpu(2);
        b.mtc0(mips::cp0::status, mips::status::kx);
        constexpr std::uint64_t md_mlan_ctl = 0x20'00a8;
        // MCR_PACK(pulse, sample) and the slot timings Linux ioc3-eth.c uses.
        const auto pulse = [&](std::uint64_t width, std::uint64_t sample) {
            (void)b.store(ialias + md_mlan_ctl, AccessWidth::bits64, width << 10 | sample << 2);
            return (b.load(ialias + md_mlan_ctl, AccessWidth::bits64).value_or(1) & 1) != 0;
        };
        t.check(!pulse(500, 65), "a presence pulse");
        for (unsigned bit = 0; bit < 8; ++bit) {
            const bool one = (0x33u >> bit & 1) != 0; // Read ROM
            (void)(one ? pulse(6, 110) : pulse(80, 30));
        }
        std::uint64_t rom = 0;
        for (unsigned bit = 0; bit < 64; ++bit) {
            rom |= std::uint64_t{pulse(6, 13)} << bit;
        }
        t.check_equal(rom & 0xff, std::uint64_t{0x09});
        t.check_equal(rom >> 8 & 0xffff'ffff'ffff, std::uint64_t{6});

        // n1's bus answers too (several parts, so only presence is checked here).
        mips::Cpu& a = machine.cpu(0);
        a.mtc0(mips::cp0::status, mips::status::kx);
        (void)a.store(ialias + md_mlan_ctl, AccessWidth::bits64, std::uint64_t{500} << 10 | 65 << 2);
        t.check((a.load(ialias + md_mlan_ctl, AccessWidth::bits64).value_or(1) & 1) == 0,
                "n1: a presence pulse");
    }};

// Over crosstalk, a Hub answers with its local registers at 0x1000000 + offset: n1 reads n2's
// node ID through a big window aimed at n2's Crossbow port (9), as the PROM's xbow_update.
const test::Registration hub_crosstalk_registers{
    "ip27.two_nodes.hub_registers_over_crosstalk", [](test::Context& t) {
        Ip27Machine machine{two_node_config(), program({nop()})};
        mips::Cpu& a = machine.cpu(0);
        mips::Cpu& b = machine.cpu(2);
        a.mtc0(mips::cp0::status, mips::status::kx);
        b.mtc0(mips::cp0::status, mips::status::kx);
        t.check(b.store(ialias + ni_status_rev_id, AccessWidth::bits64, 1u << 8).has_value());
        constexpr std::uint64_t iio_itte1 = 0x40'0160;
        t.check(a.store(ialias + iio_itte1, AccessWidth::bits64, 0x900).has_value());
        constexpr std::uint64_t big_window1 = 0x9200'0000'2000'0000;
        t.check_equal(
            node_id_field(a.load(big_window1 + 0x160'0000, AccessWidth::bits64).value_or(0)),
            std::uint64_t{1});
    }};

// Each node sees its own resources at its node ID, the aliases (UALIAS, IALIAS) as its own,
// and the other node at the other's ID once they differ.
const test::Registration two_node_views{
    "ip27.two_nodes.views_follow_node_ids", [](test::Context& t) {
        Ip27Machine machine{two_node_config(), program({nop()})};
        mips::Cpu& a = machine.cpu(0); // node 0 (n1), slice A
        mips::Cpu& b = machine.cpu(2); // node 1 (n2), slice A
        a.mtc0(mips::cp0::status, mips::status::kx);
        b.mtc0(mips::cp0::status, mips::status::kx);
        constexpr std::uint64_t cac = 0xa800'0000'0000'1000;
        // Before node IDs are assigned both are 0, and each node has its own memory there.
        t.check(a.store(cac, AccessWidth::bits64, 1).has_value());
        t.check(b.store(cac, AccessWidth::bits64, 2).has_value());
        t.check_equal(a.load(cac, AccessWidth::bits64).value_or(0), std::uint64_t{1});
        t.check_equal(b.load(cac, AccessWidth::bits64).value_or(0), std::uint64_t{2});
        t.check(!a.load(cac + (std::uint64_t{1} << 32), AccessWidth::bits64), "no node 1 yet");
        // Node 1 takes node ID 1 in its NI_STATUS_REV_ID.
        t.check(b.store(ialias + ni_status_rev_id, AccessWidth::bits64, 1u << 8).has_value());
        t.check_equal(node_id_field(b.load(ialias + ni_status_rev_id, AccessWidth::bits64).value_or(0)),
                      std::uint64_t{1});
        t.check_equal(node_id_field(a.load(ialias + ni_status_rev_id, AccessWidth::bits64).value_or(1)),
                      std::uint64_t{0});
        // Node 0 reaches node 1's memory and Hub at node 1's addresses.
        t.check_equal(a.load(cac + (std::uint64_t{1} << 32), AccessWidth::bits64).value_or(0),
                      std::uint64_t{2});
        t.check_equal(node_id_field(a.load(remote_hub(1) + ni_status_rev_id, AccessWidth::bits64)
                                        .value_or(0)),
                      std::uint64_t{1});
        // Node 1 reaches node 0's memory at node 0's addresses, but UALIAS is its own memory.
        t.check_equal(b.load(cac, AccessWidth::bits64).value_or(0), std::uint64_t{1});
        t.check_equal(b.load(0x9000'0000'0000'1000, AccessWidth::bits64).value_or(0),
                      std::uint64_t{2});
        // An interrupt sent to node 1's Hub (level 11, as an IPI) is pending there only.
        t.check(a.store(remote_hub(1) + pi_int_pend_mod, AccessWidth::bits64, 0x10b).has_value());
        t.check((b.load(ialias + pi_int_pend0, AccessWidth::bits64).value_or(0) & 0x800) != 0,
                "pending on node 1");
        t.check((a.load(ialias + pi_int_pend0, AccessWidth::bits64).value_or(0) & 0x800) == 0,
                "not on node 0");
    }};

std::uint64_t node_word(Ip27Machine& machine, unsigned cpu, std::uint64_t address) {
    return machine.cpu(cpu).load(0xa800'0000'0000'0000 | address, AccessWidth::bits64).value_or(~0ull);
}

void kernel_64_all(Ip27Machine& machine, unsigned cpus) {
    for (unsigned i = 0; i < cpus; ++i) {
        machine.cpu(i).mtc0(mips::cp0::status, mips::status::kx);
    }
}

// Four CPUs take turns lowest-first (SMP.adoc "Time and scheduling"): every CPU runs, the
// order is the same however the run is split, and each node's LL/SC counter loses no update.
const test::Registration four_cpus{
    "ip27.two_nodes.four_cpus_take_turns", [](test::Context& t) {
        for (const std::uint64_t slice : {std::uint64_t{1}, std::uint64_t{50}}) {
            Ip27Config config = two_node_config(2);
            config.cpu_slice = slice;
            Ip27Machine whole{config, counter_prom};
            Ip27Machine split{config, counter_prom};
            kernel_64_all(whole, 4);
            kernel_64_all(split, 4);
            whole.run(20000);
            split.run(7001);
            std::uint64_t lowest = ~0ull;
            for (unsigned cpu = 0; cpu < 4; ++cpu) {
                lowest = std::min(lowest, split.cpu(cpu).cycles());
            }
            split.run(20000 - lowest);
            for (unsigned cpu = 0; cpu < 4; ++cpu) {
                t.check(whole.cpu(cpu).state().gpr[t6] > 0, "every CPU makes progress");
                t.check(whole.cpu(cpu).capture() == split.cpu(cpu).capture(),
                        "the same however the run is split");
            }
            // Both nodes still have node ID 0: each counts in its own memory.
            for (const unsigned node : {0u, 1u}) {
                const std::uint64_t counted = whole.cpu(node * 2).state().gpr[t6] +
                                              whole.cpu(node * 2 + 1).state().gpr[t6];
                const std::uint64_t counter = node_word(whole, node * 2, 0x200) >> 32;
                t.check(counter >= counted && counter <= counted + 2, "no lost update");
            }
        }
    }};

// Host-parallel CPUs (SMP.adoc "S4"): four CPUs on four host threads count with LL/SC, and
// no update is lost; the state then resumes in reference mode, which goes on counting.
const test::Registration parallel_four_cpus{
    "ip27.parallel.four_cpus_ll_sc_counter", [](test::Context& t) {
        Ip27Machine machine{two_node_config(2), counter_prom};
        machine.set_parallel(true, 1000);
        kernel_64_all(machine, 4);
        machine.run(200000);
        const auto no_lost_update = [&](Ip27Machine& m, std::string_view when) {
            for (const unsigned node : {0u, 1u}) {
                const std::uint64_t counted =
                    m.cpu(node * 2).state().gpr[t6] + m.cpu(node * 2 + 1).state().gpr[t6];
                const std::uint64_t counter = node_word(m, node * 2, 0x200) >> 32;
                t.check(counter >= counted && counter <= counted + 2, when);
            }
        };
        for (unsigned cpu = 0; cpu < 4; ++cpu) {
            t.check(machine.cpu(cpu).state().gpr[t6] > 0, "every CPU makes progress");
        }
        no_lost_update(machine, "no lost update on threads");
        Ip27Config config = two_node_config(2);
        Ip27Machine resumed{config, counter_prom};
        if (!t.check(resumed.load_state(machine.save_state()).has_value(), "the snapshot loads")) {
            return;
        }
        std::array<std::uint64_t, 4> before{};
        for (unsigned cpu = 0; cpu < 4; ++cpu) {
            before[cpu] = resumed.cpu(cpu).state().gpr[t6];
        }
        resumed.run(20000);
        for (unsigned cpu = 0; cpu < 4; ++cpu) {
            t.check(resumed.cpu(cpu).state().gpr[t6] > before[cpu], "reference mode goes on");
        }
        no_lost_update(resumed, "no lost update in reference mode");
    }};

// A two-node snapshot resumes exactly where the run would have gone.
const test::Registration two_node_snapshot{
    "ip27.two_nodes.snapshot", [](test::Context& t) {
        Ip27Config config = two_node_config(2);
        config.cpu_slice = 5;
        Ip27Machine straight{config, counter_prom};
        Ip27Machine saved{config, counter_prom};
        kernel_64_all(straight, 4);
        kernel_64_all(saved, 4);
        straight.run(6000);
        saved.run(3001);
        const StateImage image = saved.save_state();
        Ip27Machine resumed{config, counter_prom};
        if (!t.check(resumed.load_state(image).has_value(), "the snapshot loads")) {
            return;
        }
        std::uint64_t lowest = ~0ull;
        for (unsigned cpu = 0; cpu < 4; ++cpu) {
            lowest = std::min(lowest, resumed.cpu(cpu).cycles());
        }
        resumed.run(6000 - lowest);
        for (unsigned cpu = 0; cpu < 4; ++cpu) {
            t.check(straight.cpu(cpu).capture() == resumed.cpu(cpu).capture(), "each CPU");
        }
        t.check_equal(node_word(straight, 2, 0x200), node_word(resumed, 2, 0x200));
        Ip27Machine one_node{two_cpu_config(5), counter_prom};
        t.check(!one_node.load_state(image).has_value(), "a one-node machine refuses it");
    }};

} // namespace
