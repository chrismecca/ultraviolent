#pragma once

#include <ultraviolent/core/scheduler.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace ultraviolent::devices {

// AMD Am29F080 8-Mbit (1 MB x 8) flash memory, 16 uniform 64 KB sectors (AMD Am29F080
// datasheet; the IP27 PROM's flash, PRM 2.3). Command cycles decode address bits A10-A0.
//
// Modeled: read array; reset (F0); autoselect (manufacturer 0x01, device 0xd5, sectors
// unprotected); byte program (A0), which can only clear bits; sector erase (30) and chip erase
// (10); status reads during embedded operations: DQ7 data polling, DQ6 toggle, DQ5 on a
// program that tries to set bits, DQ3 once an erase has begun. Erase suspend is not modeled.
// Program and erase take their typical times in virtual time.
class Am29f080 {
  public:
    static constexpr std::size_t size = 0x10'0000;
    static constexpr std::size_t sector_size = 0x1'0000;
    static constexpr std::uint8_t manufacturer_id = 0x01;
    static constexpr std::uint8_t device_id = 0xd5;
    // Typical embedded operation times (datasheet): byte program 7 us, sector erase 1 s.
    static constexpr VirtualDuration program_time{7'000};
    static constexpr VirtualDuration sector_erase_time{1'000'000'000};

    // `array` is the flash contents (size bytes), which the caller owns and may also map for
    // fast reads while array_mode() holds. `mode_changed` runs when array_mode() changes.
    // `name` distinguishes this part's snapshot fields and event (the default keeps the
    // original names).
    Am29f080(Scheduler& scheduler, Tracer& tracer, std::span<std::byte> array,
             std::function<void()> mode_changed, const std::string& name = "");

    // Whether reads return the array (no command or embedded operation in progress).
    [[nodiscard]] bool array_mode() const {
        return state_ == State::read_array;
    }
    // Whether the contents changed since construction (for host persistence).
    [[nodiscard]] bool dirty() const {
        return dirty_;
    }

    std::uint8_t read(std::uint32_t address);
    void write(std::uint32_t address, std::uint8_t data);

    // Word mode: the same array as a 16-bit part of this size on a 16-bit bus (for example
    // the AMD Am29LV800BT, 0x22da), addressed by word. Commands decode the word address and
    // the low data byte; a program cycle programs the whole word; autoselect returns the
    // manufacturer and `word_device` as words; array words are big-endian byte pairs.
    void set_word_mode(std::uint16_t word_device) {
        word_device_id_ = word_device;
    }
    std::uint16_t read_word(std::uint32_t word);
    void write_word(std::uint32_t word, std::uint16_t data);

    void reset();

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    enum class State : std::uint8_t {
        read_array,
        unlock1,    // AA seen
        unlock2,    // AA 55 seen
        autoselect, // reads return identification
        program,    // next write is the byte to program
        erase1,     // AA 55 80 seen
        erase2,     // ... AA
        erase3,     // ... 55
        busy,       // embedded program or erase
    };

    void set_state(State state);
    void finish_operation();

    Scheduler& scheduler_;
    Tracer& tracer_;
    std::span<std::byte> array_;
    std::function<void()> mode_changed_;
    EventId operation_done_;
    State state_{State::read_array};
    // The embedded operation in progress.
    bool erasing_{};
    bool failed_{};
    std::uint32_t operation_address_{};
    std::uint8_t operation_data_{};
    std::uint32_t erase_sectors_{}; // bit per sector
    bool toggle_{};
    bool dirty_{};
    // Snapshot key prefix ("flash" by default).
    std::string key_;
    // The device code in word mode (0: byte mode only).
    std::uint16_t word_device_id_{};
};

} // namespace ultraviolent::devices
