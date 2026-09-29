#include "support/test.hpp"

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/trace.hpp>
#include <ultraviolent/core/virtual_clock.hpp>
#include <ultraviolent/devices/m48t35.hpp>

#include <cstdint>
#include <vector>

namespace {

using namespace ultraviolent;

struct Bench {
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    // 2026-09-27 12:34:56 UTC; the year register counts from 1970, as on SGI systems.
    devices::M48t35 rtc{scheduler, 1'790'512'496, 1970};
};

const test::Registration clock_reads{
    "m48t35.clock_runs_on_virtual_time", [](test::Context& t) {
        Bench b;
        t.check_equal(b.rtc.read(0x7ff9), std::uint8_t{0x56}); // seconds, BCD
        t.check_equal(b.rtc.read(0x7ffa), std::uint8_t{0x34});
        t.check_equal(b.rtc.read(0x7ffb), std::uint8_t{0x12});
        t.check_equal(b.rtc.read(0x7ffd), std::uint8_t{0x27}); // date
        t.check_equal(b.rtc.read(0x7ffe), std::uint8_t{0x09}); // month
        t.check_equal(b.rtc.read(0x7fff), std::uint8_t{0x56}); // year: 2026 is 56
        b.scheduler.advance_by(VirtualDuration{5'000'000'000});
        t.check_equal(b.rtc.read(0x7ffa), std::uint8_t{0x35}); // 12:35:01
        t.check_equal(b.rtc.read(0x7ff9), std::uint8_t{0x01});
    }};

const test::Registration clock_set{"m48t35.write_bit_sets_the_clock", [](test::Context& t) {
                                       Bench b;
                                       b.rtc.write(0x7ff8, 0x80); // WRITE: registers held
                                       b.rtc.write(0x7fff, 0x29); // 1999
                                       b.rtc.write(0x7ffe, 0x12);
                                       b.rtc.write(0x7ffd, 0x31);
                                       b.rtc.write(0x7ffb, 0x23);
                                       b.rtc.write(0x7ffa, 0x59);
                                       b.rtc.write(0x7ff9, 0x58);
                                       b.rtc.write(0x7ff8, 0x00); // runs from 1999-12-31 23:59:58
                                       b.scheduler.advance_by(VirtualDuration{3'000'000'000});
                                       t.check_equal(b.rtc.read(0x7fff),
                                                     std::uint8_t{0x30}); // 2000
                                       t.check_equal(b.rtc.read(0x7ffe), std::uint8_t{0x01});
                                       t.check_equal(b.rtc.read(0x7ff9), std::uint8_t{0x01});
                                       // READ freezes the registers.
                                       b.rtc.write(0x7ff8, 0x40);
                                       b.scheduler.advance_by(VirtualDuration{2'000'000'000});
                                       t.check_equal(b.rtc.read(0x7ff9), std::uint8_t{0x01});
                                       b.rtc.write(0x7ff8, 0x00);
                                       t.check_equal(b.rtc.read(0x7ff9), std::uint8_t{0x03});
                                       // The rest is plain memory.
                                       b.rtc.write(0x100, 0xab);
                                       t.check_equal(b.rtc.read(0x100), std::uint8_t{0xab});
                                   }};

const test::Registration persistence{
    "m48t35.contents_persist_the_clock", [](test::Context& t) {
        Bench b;
        b.rtc.write(0x1234, 0x5a);
        b.scheduler.advance_by(VirtualDuration{61'000'000'000});
        // The saved contents hold the running time: 12:35:57.
        const auto saved = b.rtc.contents();
        const std::vector<std::uint8_t> file(saved.begin(), saved.end());
        t.check_equal(file[0x7ffa], std::uint8_t{0x35});
        t.check_equal(file[0x7ff9], std::uint8_t{0x57});

        // Power on again: the memory is back, and the clock resumes from the file's time.
        Bench again;
        again.rtc.load_contents(file, true);
        t.check_equal(again.rtc.read(0x1234), std::uint8_t{0x5a});
        again.scheduler.advance_by(VirtualDuration{3'000'000'000});
        t.check_equal(again.rtc.read(0x7ffa), std::uint8_t{0x36}); // 12:36:00
        t.check_equal(again.rtc.read(0x7ff9), std::uint8_t{0x00});

        // Without resume_clock (--date), the configured time stands.
        Bench dated;
        dated.rtc.load_contents(file, false);
        t.check_equal(dated.rtc.read(0x7ffa), std::uint8_t{0x34});
        t.check_equal(dated.rtc.read(0x1234), std::uint8_t{0x5a});

        // A file whose clock registers hold no valid time (a fresh one) keeps the epoch.
        Bench fresh;
        fresh.rtc.load_contents(std::vector<std::uint8_t>(devices::M48t35::size), true);
        t.check_equal(fresh.rtc.read(0x7fff), std::uint8_t{0x56});
    }};

} // namespace
