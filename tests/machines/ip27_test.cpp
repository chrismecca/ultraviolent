#include "arch/mips/assembler.hpp"
#include "support/test.hpp"

#include <ultraviolent/core/byte_order.hpp>
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
        t.check_equal(machine.cpu().load(0x9200'0000'0122'0048, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x10});
        t.check_equal(machine.cpu().load(0x9200'0000'01a2'0048, AccessWidth::bits64).value_or(0),
                      std::uint64_t{0x10});
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

} // namespace
