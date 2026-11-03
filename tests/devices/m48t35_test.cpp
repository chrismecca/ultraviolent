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

        // Time on the battery: two days and a minute off, from 12:35:57 on September 27.
        Bench battery;
        battery.rtc.load_contents(file, true, 2 * 86'400 + 60);
        t.check_equal(battery.rtc.read(0x7ffd), std::uint8_t{0x29});
        t.check_equal(battery.rtc.read(0x7ffa), std::uint8_t{0x36});
        t.check_equal(battery.rtc.read(0x7ff9), std::uint8_t{0x57});
        t.check_equal(battery.rtc.read(0x7ffc), std::uint8_t{0x02}); // Sunday + 2: Tuesday

        // A file whose clock registers hold no valid time (a fresh one) keeps the epoch.
        Bench fresh;
        fresh.rtc.load_contents(std::vector<std::uint8_t>(devices::M48t35::size), true);
        t.check_equal(fresh.rtc.read(0x7fff), std::uint8_t{0x56});
    }};

// Loads the clock through WRITE: year register, month, date, hours, minutes, seconds (BCD),
// and the day of week.
void set_clock(Bench& b, std::uint8_t year, std::uint8_t month, std::uint8_t date,
               std::uint8_t hours, std::uint8_t minutes, std::uint8_t seconds,
               std::uint8_t weekday = 1) {
    b.rtc.write(0x7ff8, 0x80);
    b.rtc.write(0x7fff, year);
    b.rtc.write(0x7ffe, month);
    b.rtc.write(0x7ffd, date);
    b.rtc.write(0x7ffc, weekday);
    b.rtc.write(0x7ffb, hours);
    b.rtc.write(0x7ffa, minutes);
    b.rtc.write(0x7ff9, seconds);
    b.rtc.write(0x7ff8, 0x00);
}

// The chip's leap years are its register's multiples of 4 (datasheet): from a 1970 base, 2026
// (56) has a February 29 and 2028 (58) does not.
const test::Registration chip_leap_years{"m48t35.leap_years_by_the_register",
                                         [](test::Context& t) {
    Bench b;
    set_clock(b, 0x56, 0x02, 0x28, 0x23, 0x59, 0x59);
    b.scheduler.advance_by(VirtualDuration::from_seconds(1));
    t.check_equal(b.rtc.read(0x7ffd), std::uint8_t{0x29});
    t.check_equal(b.rtc.read(0x7ffe), std::uint8_t{0x02});
    b.scheduler.advance_by(VirtualDuration::from_seconds(86'400));
    t.check_equal(b.rtc.read(0x7ffd), std::uint8_t{0x01});
    t.check_equal(b.rtc.read(0x7ffe), std::uint8_t{0x03});

    set_clock(b, 0x58, 0x02, 0x28, 0x23, 0x59, 0x59);
    b.scheduler.advance_by(VirtualDuration::from_seconds(1));
    t.check_equal(b.rtc.read(0x7ffd), std::uint8_t{0x01});
    t.check_equal(b.rtc.read(0x7ffe), std::uint8_t{0x03});

    // The last day of register year 99 runs on into 00, a leap year.
    set_clock(b, 0x99, 0x12, 0x31, 0x23, 0x59, 0x59);
    b.scheduler.advance_by(VirtualDuration::from_seconds(1 + 59 * 86'400));
    t.check_equal(b.rtc.read(0x7fff), std::uint8_t{0x00});
    t.check_equal(b.rtc.read(0x7ffe), std::uint8_t{0x02});
    t.check_equal(b.rtc.read(0x7ffd), std::uint8_t{0x29});
}};

// A chip-only date (February 29 of register 56) persists like any other.
const test::Registration chip_date_persists{"m48t35.chip_dates_persist", [](test::Context& t) {
    Bench b;
    set_clock(b, 0x56, 0x02, 0x29, 0x12, 0x00, 0x00);
    const auto saved = b.rtc.contents();
    const std::vector<std::uint8_t> file(saved.begin(), saved.end());
    Bench again;
    again.rtc.load_contents(file, true);
    t.check_equal(again.rtc.read(0x7ffd), std::uint8_t{0x29});
    t.check_equal(again.rtc.read(0x7ffe), std::uint8_t{0x02});
    t.check_equal(again.rtc.read(0x7fff), std::uint8_t{0x56});
}};

// An epoch goes in as its real date, after the chip's extra day in 2026: 2026-11-02 12:00:00
// UTC reads November 2, a Monday.
const test::Registration epoch_real_date{"m48t35.epoch_is_the_real_date", [](test::Context& t) {
    VirtualClock clock;
    Tracer tracer{clock};
    Scheduler scheduler{clock, tracer};
    devices::M48t35 rtc{scheduler, 1'793'620'800, 1970};
    t.check_equal(rtc.read(0x7fff), std::uint8_t{0x56});
    t.check_equal(rtc.read(0x7ffe), std::uint8_t{0x11});
    t.check_equal(rtc.read(0x7ffd), std::uint8_t{0x02});
    t.check_equal(rtc.read(0x7ffb), std::uint8_t{0x12});
    t.check_equal(rtc.read(0x7ffc), std::uint8_t{0x01});
}};

// The day of week counts on from what software wrote, 7 to 1.
const test::Registration weekday_counter{"m48t35.weekday_counts_from_the_written_value",
                                         [](test::Context& t) {
    Bench b;
    set_clock(b, 0x56, 0x03, 0x10, 0x23, 0x59, 0x59, 6);
    t.check_equal(b.rtc.read(0x7ffc), std::uint8_t{0x06});
    b.scheduler.advance_by(VirtualDuration::from_seconds(1));
    t.check_equal(b.rtc.read(0x7ffc), std::uint8_t{0x07});
    b.scheduler.advance_by(VirtualDuration::from_seconds(86'400));
    t.check_equal(b.rtc.read(0x7ffc), std::uint8_t{0x01});
}};

} // namespace
