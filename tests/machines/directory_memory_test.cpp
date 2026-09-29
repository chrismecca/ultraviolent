#include "support/test.hpp"

#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/machines/ip27/directory_memory.hpp>

#include <array>
#include <cstdint>

namespace {

using namespace ultraviolent;
using ip27::DirectoryDimms;
using ip27::DirectoryMemory;

constexpr std::uint64_t mb = std::uint64_t{1} << 20;
constexpr std::uint64_t force_ecc = std::uint64_t{1} << 63;
// Back-door directory space per bank: a quarter of the 512 MB bank window.
constexpr std::uint64_t bank_slice = 128 * mb;

struct Bench {
    explicit Bench(DirectoryDimms dimms, std::uint64_t bank0 = 64 * mb)
        : directory{tracer, {bank0, 0, 0, 0, 0, 0, 0, 0}, dimms} {}

    std::uint64_t read(std::uint64_t offset) {
        return directory.mmio_read(offset, AccessWidth::bits64).value_or(0xdead);
    }
    bool write(std::uint64_t offset, std::uint64_t value) {
        return directory.mmio_write(offset, AccessWidth::bits64, value).has_value();
    }

    VirtualClock clock;
    Tracer tracer{clock};
    DirectoryMemory directory;
};

const test::Registration premium_entries{
    "ip27.directory.premium_entries", [](test::Context& t) {
        Bench b{DirectoryDimms::premium};
        // MD_PDIR_MASK: 48 bits. FORCE_ECC (bit 63) only qualifies the write.
        t.check(b.write(0x0, force_ecc | 0x1234'5678'9abc'def0));
        t.check_equal(b.read(0x0), std::uint64_t{0x5678'9abc'def0});
        t.check_equal(b.read(0x8), std::uint64_t{0}); // neighbors are separate entries
    }};

const test::Registration standard_entries{"ip27.directory.standard_entries", [](test::Context& t) {
                                              Bench b{DirectoryDimms::standard};
                                              // MD_SDIR_MASK: 16 bits, whatever DIR_PREMIUM says
                                              // (it resets set).
                                              t.check(b.write(0x200, 0xaaaa'aaaa'aaaa'5555));
                                              t.check_equal(b.read(0x200), std::uint64_t{0x5555});
                                              b.directory.set_premium_mode(false);
                                              t.check_equal(b.read(0x200), std::uint64_t{0x5555});
                                          }};

const test::Registration standard_mode_on_premium{
    "ip27.directory.standard_mode_on_premium_dimms", [](test::Context& t) {
        Bench b{DirectoryDimms::premium};
        t.check(b.write(0x10, 0x1111'2222'3333));
        b.directory.set_premium_mode(false);
        // Only the standard bits are addressed; the directory DIMM's bits keep their value.
        t.check_equal(b.read(0x10), std::uint64_t{0x3333});
        t.check(b.write(0x10, 0xffff'ffff'4444));
        b.directory.set_premium_mode(true);
        t.check_equal(b.read(0x10), std::uint64_t{0x1111'2222'4444});
    }};

const test::Registration aliasing{"ip27.directory.small_bank_aliases", [](test::Context& t) {
                                      // A 256 MB bank has 64 MB of back-door space; the DIMMs
                                      // ignore the higher address bits, so the entry for bank
                                      // offset 256 MB is the entry for offset 0.
                                      Bench b{DirectoryDimms::premium, 256 * mb};
                                      t.check(b.write(0x0, 0x55));
                                      t.check(b.write(64 * mb, 0xaa));
                                      t.check_equal(b.read(0x0), std::uint64_t{0xaa});
                                      t.check(
                                          b.write(32 * mb, 0x77)); // half the bank does not alias
                                      t.check_equal(b.read(0x0), std::uint64_t{0xaa});
                                      t.check_equal(b.read(96 * mb), std::uint64_t{0x77});
                                  }};

const test::Registration empty_bank{"ip27.directory.empty_bank", [](test::Context& t) {
                                        Bench b{DirectoryDimms::premium};
                                        // Bank 1 is empty: accesses complete, writes are lost,
                                        // reads return 0.
                                        t.check(b.write(bank_slice, 0x5555));
                                        t.check_equal(b.read(bank_slice), std::uint64_t{0});
                                        t.check_equal(b.read(0x0),
                                                      std::uint64_t{0}); // bank 0 untouched
                                    }};

const test::Registration word_halves{
    "ip27.directory.word_accesses_are_halves", [](test::Context& t) {
        Bench b{DirectoryDimms::premium};
        t.check(b.write(0x0, 0x1234'5678'9abc));
        t.check_equal(b.directory.mmio_read(0x0, AccessWidth::bits32).value_or(1),
                      std::uint64_t{0x1234});
        t.check_equal(b.directory.mmio_read(0x4, AccessWidth::bits32).value_or(1),
                      std::uint64_t{0x5678'9abc});
        t.check(b.directory.mmio_write(0x4, AccessWidth::bits32, 0x1111'2222).has_value());
        t.check_equal(b.read(0x0), std::uint64_t{0x1234'1111'2222});
        // An empty bank reads zero a word at a time too.
        t.check_equal(b.directory.mmio_read(bank_slice + 4, AccessWidth::bits32).value_or(1),
                      std::uint64_t{0});
        t.check(!b.directory.mmio_read(0x0, AccessWidth::bits16).has_value());
    }};

const test::Registration snapshot{"ip27.directory.snapshot", [](test::Context& t) {
                                      Bench a{DirectoryDimms::premium};
                                      t.check(a.write(0x2a8, 0x1234));
                                      t.check(a.write(12 * mb, 0x5678));
                                      StateImage image;
                                      a.directory.save_state(image);
                                      Bench b{DirectoryDimms::premium};
                                      b.directory.load_state(image);
                                      t.check_equal(b.read(0x2a8), std::uint64_t{0x1234});
                                      t.check_equal(b.read(12 * mb), std::uint64_t{0x5678});
                                  }};

} // namespace
