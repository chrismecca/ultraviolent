#include <ultraviolent/devices/am29f080.hpp>

#include <algorithm>

// Command definitions and status bits: AMD Am29F080 datasheet ("Command Definitions",
// "Write Operation Status").
namespace ultraviolent::devices {

namespace {

constexpr std::uint32_t command_mask = 0x7ff; // A10-A0
constexpr std::uint32_t address_555 = 0x555;
constexpr std::uint32_t address_2aa = 0x2aa;
constexpr std::uint8_t dq7 = 0x80;
constexpr std::uint8_t dq6 = 0x40;
constexpr std::uint8_t dq5 = 0x20;
constexpr std::uint8_t dq3 = 0x08;

} // namespace

Am29f080::Am29f080(Scheduler& scheduler, Tracer& tracer, std::span<std::byte> array,
                   std::function<void()> mode_changed, const std::string& name)
    : scheduler_{scheduler}, tracer_{tracer}, array_{array}, mode_changed_{std::move(mode_changed)},
      operation_done_{scheduler.add_event(name.empty() ? "am29f080.operation" : name + ".operation",
                                          [this] { finish_operation(); })},
      key_{name.empty() ? "flash" : name} {}

void Am29f080::set_state(State state) {
    const bool was_array = array_mode();
    state_ = state;
    if (was_array != array_mode() && mode_changed_) {
        mode_changed_();
    }
}

void Am29f080::reset() {
    scheduler_.cancel(operation_done_);
    failed_ = false;
    set_state(State::read_array);
}

std::uint8_t Am29f080::read(std::uint32_t address) {
    address %= size;
    switch (state_) {
    case State::autoselect:
        switch (address & 0xff) {
        case 0x00:
            return manufacturer_id;
        case 0x01:
            return device_id;
        default:
            return 0x00; // sector protection verify at SA+02: unprotected
        }
    case State::busy: {
        // Data polling: DQ7 is the complement of the programmed bit, 0 while erasing; DQ6
        // toggles on every read; DQ5 reports a failed program; DQ3 an erase in progress.
        toggle_ = !toggle_;
        std::uint8_t status = toggle_ ? dq6 : 0;
        if (erasing_) {
            status |= dq3;
        } else {
            status |= static_cast<std::uint8_t>(~operation_data_ & dq7);
        }
        if (failed_) {
            status |= dq5;
        }
        return status;
    }
    default:
        return std::to_integer<std::uint8_t>(array_[address]);
    }
}

std::uint16_t Am29f080::read_word(std::uint32_t word) {
    const auto byte = static_cast<std::uint32_t>(2 * std::size_t{word} % size);
    if (state_ == State::autoselect) {
        switch (word & 0xff) {
        case 0x00:
            return manufacturer_id;
        case 0x01:
            return word_device_id_;
        default:
            return 0x0000; // unprotected
        }
    }
    if (state_ == State::busy) {
        // Status on DQ7-DQ0; hypothesis: DQ15-DQ8 mirror it.
        const std::uint8_t status = read(byte);
        return static_cast<std::uint16_t>(status << 8 | status);
    }
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(array_[byte]) << 8 |
                                      std::to_integer<std::uint16_t>(array_[byte + 1]));
}

void Am29f080::write_word(std::uint32_t word, std::uint16_t data) {
    const auto byte = static_cast<std::uint32_t>(2 * std::size_t{word} % size);
    const auto low = static_cast<std::uint8_t>(data);
    if (state_ == State::program) {
        // Both bytes of the word; a program can only clear bits.
        const std::uint8_t high_old = std::to_integer<std::uint8_t>(array_[byte]);
        const std::uint8_t low_old = std::to_integer<std::uint8_t>(array_[byte + 1]);
        const auto high = static_cast<std::uint8_t>(data >> 8);
        array_[byte] = static_cast<std::byte>(high_old & high);
        array_[byte + 1] = static_cast<std::byte>(low_old & low);
        dirty_ = true;
        erasing_ = false;
        failed_ = (high & ~high_old) != 0 || (low & ~low_old) != 0;
        operation_address_ = byte + 1;
        operation_data_ = low;
        set_state(State::busy);
        scheduler_.schedule_after(operation_done_, program_time);
        return;
    }
    if (state_ == State::erase3 && low == 0x30) {
        write(byte, low); // the sector of this word's byte address
        return;
    }
    write(word, low); // command cycles decode the word address
}

void Am29f080::write(std::uint32_t address, std::uint8_t data) {
    address %= size;
    const std::uint32_t command_address = address & command_mask;
    if (state_ == State::busy) {
        // A failed operation (DQ5) waits for a reset; hypothesis: other commands during an
        // embedded operation are ignored.
        if (failed_ && !scheduler_.is_pending(operation_done_) && data == 0xf0) {
            failed_ = false;
            set_state(State::read_array);
        }
        return;
    }
    if (data == 0xf0 && state_ != State::program) {
        set_state(State::read_array);
        return;
    }
    switch (state_) {
    case State::read_array:
    case State::autoselect:
        if (command_address == address_555 && data == 0xaa) {
            set_state(State::unlock1);
        }
        return;
    case State::unlock1:
        set_state(command_address == address_2aa && data == 0x55 ? State::unlock2
                                                                 : State::read_array);
        return;
    case State::unlock2:
        if (command_address != address_555) {
            set_state(State::read_array);
            return;
        }
        switch (data) {
        case 0x90:
            set_state(State::autoselect);
            return;
        case 0xa0:
            set_state(State::program);
            return;
        case 0x80:
            set_state(State::erase1);
            return;
        default:
            set_state(State::read_array);
            return;
        }
    case State::program: {
        // A program can only clear bits.
        const std::uint8_t old = std::to_integer<std::uint8_t>(array_[address]);
        array_[address] = static_cast<std::byte>(old & data);
        dirty_ = true;
        erasing_ = false;
        failed_ = (data & ~old) != 0;
        operation_address_ = address;
        operation_data_ = data;
        set_state(State::busy);
        scheduler_.schedule_after(operation_done_, program_time);
        return;
    }
    case State::erase1:
        set_state(command_address == address_555 && data == 0xaa ? State::erase2
                                                                 : State::read_array);
        return;
    case State::erase2:
        set_state(command_address == address_2aa && data == 0x55 ? State::erase3
                                                                 : State::read_array);
        return;
    case State::erase3:
        if (data == 0x10 && command_address == address_555) {
            erase_sectors_ = 0xffff; // chip erase
        } else if (data == 0x30) {
            erase_sectors_ = 1u << (address / sector_size);
        } else {
            set_state(State::read_array);
            return;
        }
        erasing_ = true;
        failed_ = false;
        tracer_.log(TraceCategory::machine, "flash erase sectors {:#06x}", erase_sectors_);
        set_state(State::busy);
        scheduler_.schedule_after(
            operation_done_,
            VirtualDuration{sector_erase_time.nanoseconds *
                            static_cast<std::uint64_t>(std::popcount(erase_sectors_))});
        return;
    case State::busy:
        return;
    }
}

void Am29f080::finish_operation() {
    if (erasing_) {
        for (std::size_t sector = 0; sector < size / sector_size; ++sector) {
            if ((erase_sectors_ & (1u << sector)) != 0) {
                std::fill_n(array_.begin() + static_cast<std::ptrdiff_t>(sector * sector_size),
                            sector_size, std::byte{0xff});
            }
        }
        dirty_ = true;
        erasing_ = false;
    }
    // hypothesis: a failed program stays in status mode (DQ5) until reset (F0).
    set_state(failed_ ? State::busy : State::read_array);
    if (failed_) {
        tracer_.log(TraceCategory::machine, "flash program failure at {:#07x}", operation_address_);
    }
}

void Am29f080::save_state(StateImage& image) const {
    image.put_sparse(key_ + ".array", array_);
    const std::uint32_t registers[] = {static_cast<std::uint32_t>(state_),
                                       erasing_,
                                       failed_,
                                       operation_address_,
                                       operation_data_,
                                       erase_sectors_,
                                       toggle_,
                                       dirty_};
    image.put(key_ + ".state", registers);
}

void Am29f080::load_state(const StateImage& image) {
    (void)image.get_sparse(key_ + ".array", array_);
    std::uint32_t registers[8]{};
    if (image.get(key_ + ".state", registers)) {
        state_ = static_cast<State>(registers[0]);
        erasing_ = registers[1] != 0;
        failed_ = registers[2] != 0;
        operation_address_ = registers[3];
        operation_data_ = static_cast<std::uint8_t>(registers[4]);
        erase_sectors_ = registers[5];
        toggle_ = registers[6] != 0;
        dirty_ = registers[7] != 0;
    }
    if (mode_changed_) {
        mode_changed_();
    }
}

} // namespace ultraviolent::devices
