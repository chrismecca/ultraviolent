#include <ultraviolent/devices/one_wire.hpp>

#include <algorithm>

// 1-Wire signaling and the DS2502-family protocol: Dallas/Maxim application note 937 ("Book
// of iButton Standards") and the DS2502 datasheet. Slot timing classification follows the
// values Linux ioc3-eth.c uses with SGI's MicroLAN masters (IP27.adoc "NICs").
namespace ultraviolent::devices {

namespace {

// A reset pulse is at least 480 microseconds; write-1 and read slots hold the line low for
// at most 15, write-0 slots for 60-120. inferred: the MicroLAN master samples `sample`
// microseconds after releasing the line (Linux samples a presence pulse 65 after a 500 pulse,
// and a read slot 13 after a 6 pulse).
constexpr unsigned reset_pulse = 400;
constexpr unsigned short_pulse = 15;

} // namespace

std::uint16_t dallas_crc16(const std::uint8_t* bytes, std::size_t count, std::uint16_t crc) {
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t byte = bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            const bool mix = ((crc ^ byte) & 1) != 0;
            crc = static_cast<std::uint16_t>(crc >> 1);
            if (mix) {
                crc ^= 0xa001;
            }
            byte = static_cast<std::uint8_t>(byte >> 1);
        }
    }
    return crc;
}

std::uint8_t dallas_crc8(const std::uint8_t* bytes, std::size_t count, std::uint8_t crc) {
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t byte = bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            const bool mix = ((crc ^ byte) & 1) != 0;
            crc = static_cast<std::uint8_t>(crc >> 1);
            if (mix) {
                crc ^= 0x8c;
            }
            byte = static_cast<std::uint8_t>(byte >> 1);
        }
    }
    return crc;
}

bool OneWireBus::reset() {
    bool presence = false;
    for (OneWireDevice* device : devices_) {
        presence = device->one_wire_reset() || presence;
    }
    return presence;
}

bool OneWireBus::slot(bool master) {
    bool line = master;
    for (const OneWireDevice* device : devices_) {
        line = line && device->one_wire_output();
    }
    for (OneWireDevice* device : devices_) {
        device->one_wire_slot(line);
    }
    return line;
}

bool OneWireBus::pulse(unsigned pulse, unsigned /*sample*/) {
    if (pulse == 0) {
        // A wait: nothing drives the line.
        return true;
    }
    if (pulse >= reset_pulse) {
        // A present device holds the line low when the master samples.
        return !reset();
    }
    if (pulse <= short_pulse) {
        return slot(true);
    }
    // A write-0 slot; by the sample the line has been released again.
    slot(false);
    return true;
}

Nic::Nic(std::uint8_t family, std::uint64_t serial) {
    rom_[0] = family;
    for (std::size_t i = 0; i < 6; ++i) {
        rom_[1 + i] = static_cast<std::uint8_t>(serial >> (8 * i));
    }
    rom_[7] = dallas_crc8(rom_.data(), 7);
    // Unprogrammed EPROM bits read as ones; status 0xff means no page is redirected.
    memory_.fill(0xff);
    status_.fill(0xff);
}

bool Nic::one_wire_reset() {
    send_queue_.clear();
    select_after_send_ = false;
    begin_receive(State::rom_command, 8);
    return true;
}

void Nic::begin_receive(State state, unsigned bits) {
    state_ = state;
    receive_byte_ = 0;
    receive_bits_ = 0;
    receive_count_ = bits / 8;
}

void Nic::queue(std::uint8_t byte) {
    send_queue_.push_back(byte);
}

bool Nic::one_wire_output() const {
    switch (state_) {
    case State::send:
        if (send_position_ < send_queue_.size()) {
            return ((send_queue_[send_position_] >> send_bit_) & 1) != 0;
        }
        return true; // past the end: ones
    case State::search_bit:
        return ((rom_[search_index_ / 8] >> (search_index_ % 8)) & 1) != 0;
    case State::search_complement:
        return ((rom_[search_index_ / 8] >> (search_index_ % 8)) & 1) == 0;
    default:
        return true;
    }
}

void Nic::one_wire_slot(bool line) {
    switch (state_) {
    case State::idle:
        return;
    case State::send:
        if (send_position_ < send_queue_.size() && ++send_bit_ == 8) {
            send_bit_ = 0;
            ++send_position_;
            if (send_position_ == send_queue_.size() && select_after_send_) {
                // Read ROM leaves the device selected for a memory function.
                select_after_send_ = false;
                begin_receive(State::memory_command, 8);
            }
        }
        return;
    case State::search_bit:
        state_ = State::search_complement;
        return;
    case State::search_complement:
        state_ = State::search_choice;
        return;
    case State::search_choice: {
        const bool bit = ((rom_[search_index_ / 8] >> (search_index_ % 8)) & 1) != 0;
        if (line != bit) {
            state_ = State::idle; // another device was chosen
            return;
        }
        if (++search_index_ == 64) {
            begin_receive(State::memory_command, 8); // selected
        } else {
            state_ = State::search_bit;
        }
        return;
    }
    default:
        // Receiving bytes, least significant bit first.
        receive_byte_ = static_cast<std::uint8_t>(receive_byte_ | (line ? 1u << receive_bits_ : 0));
        if (++receive_bits_ == 8) {
            const std::uint8_t byte = receive_byte_;
            receive_byte_ = 0;
            receive_bits_ = 0;
            on_byte(byte);
        }
        return;
    }
}

void Nic::on_byte(std::uint8_t byte) {
    switch (state_) {
    case State::rom_command:
        switch (byte) {
        case 0x33: // Read ROM, then selected
            send_queue_.assign(rom_.begin(), rom_.end());
            send_position_ = 0;
            send_bit_ = 0;
            select_after_send_ = true;
            state_ = State::send;
            return;
        case 0x55: // Match ROM
            match_index_ = 0;
            matching_ = true;
            state_ = State::match_rom;
            return;
        case 0xcc: // Skip ROM
            begin_receive(State::memory_command, 8);
            return;
        case 0xf0: // Search ROM
            search_index_ = 0;
            state_ = State::search_bit;
            return;
        default:
            state_ = State::idle;
            return;
        }
    case State::match_rom:
        matching_ = matching_ && byte == rom_[match_index_];
        if (++match_index_ == rom_.size()) {
            if (matching_) {
                begin_receive(State::memory_command, 8);
            } else {
                state_ = State::idle;
            }
        }
        return;
    case State::memory_command:
        memory_function_ = byte;
        if (byte == 0xf0 || byte == 0xc3 || byte == 0xaa) {
            header_[0] = byte;
            receive_count_ = 0;
            state_ = State::address;
        } else {
            state_ = State::idle; // programming functions are not modeled
        }
        return;
    case State::address:
        header_[1 + receive_count_] = byte;
        if (++receive_count_ == 2) {
            address_ = static_cast<std::uint16_t>(header_[1] | (header_[2] << 8));
            queue_memory_read();
        }
        return;
    default:
        return;
    }
}

void Nic::queue_memory_read() {
    send_queue_.clear();
    send_position_ = 0;
    send_bit_ = 0;
    state_ = State::send;
    // Every memory function first returns the CRC of its command and address bytes.
    queue(dallas_crc8(header_.data(), header_.size()));
    if (memory_function_ == 0xaa) {
        // Read Status: the status bytes to the end, then their CRC. hypothesis on the CRC.
        const std::size_t start = std::min<std::size_t>(address_, status_size);
        for (std::size_t i = start; i < status_size; ++i) {
            queue(status_[i]);
        }
        queue(dallas_crc8(status_.data() + start, status_size - start));
        return;
    }
    const std::size_t start = std::min<std::size_t>(address_, memory_size);
    if (memory_function_ == 0xf0) {
        // Read Memory: data to the end of memory. hypothesis: then the CRC of the data read.
        for (std::size_t i = start; i < memory_size; ++i) {
            queue(memory_[i]);
        }
        queue(dallas_crc8(memory_.data() + start, memory_size - start));
        return;
    }
    // Read Data/Generate 8-bit CRC: each page's remaining data followed by its CRC.
    for (std::size_t page_start = start; page_start < memory_size;) {
        const std::size_t page_end = (page_start / page_size + 1) * page_size;
        for (std::size_t i = page_start; i < page_end; ++i) {
            queue(memory_[i]);
        }
        queue(dallas_crc8(memory_.data() + page_start, page_end - page_start));
        page_start = page_end;
    }
}

} // namespace ultraviolent::devices
