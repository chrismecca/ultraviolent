#pragma once

#include <ultraviolent/core/address.hpp>
#include <ultraviolent/core/address_space.hpp>

#include <array>
#include <cstdint>

namespace ultraviolent::mips {

// What CPUs sharing one bus must tell each other beyond the memory itself (SMP.adoc "Memory,
// LL/SC, SYNC"): their LL/SC reservations, and when every other CPU must drop the writable
// host pages it caches. A one-CPU machine has none, and its CPU behaves as before.
//
// CPUs share it while running one at a time (reference mode, ADR-023): what one CPU does
// here, another sees at its next engine entry or bus access.
class Coherence {
  public:
    static constexpr unsigned max_cpus = AddressSpace::max_requesters;
    // hypothesis: a reservation covers one secondary-cache block, 128 bytes (SCBlkSize 32
    // words in the IP27's R10000 mode bits, IP27.adoc "Reset mode bits"); a store by another
    // CPU anywhere in it breaks the reservation.
    static constexpr std::uint64_t line_size = 128;
    static constexpr std::uint64_t frame_size = 4096;

    // LL by `cpu`: a reservation on the line holding `address`. Other CPUs must stop storing
    // into its frame through their host pages, so they are told to drop them.
    void reserve(unsigned cpu, PhysicalAddress address) {
        reservations_[cpu] = {.line = address.value / line_size, .held = true};
        ++generation_;
    }
    // ERET clears LLbit (UM 14.30); the reservation goes with it.
    void release(unsigned cpu) {
        reservations_[cpu].held = false;
    }
    [[nodiscard]] bool held(unsigned cpu) const {
        return reservations_[cpu].held;
    }

    // A store by `cpu` to [address, address + size) breaks every other CPU's reservation on
    // the lines it touches.
    void store(unsigned cpu, PhysicalAddress address, std::uint64_t size) {
        const std::uint64_t first = address.value / line_size;
        const std::uint64_t last = (address.value + size - 1) / line_size;
        for (unsigned other = 0; other < max_cpus; ++other) {
            Reservation& r = reservations_[other];
            if (other != cpu && r.held && r.line >= first && r.line <= last) {
                r.held = false;
            }
        }
    }

    // Whether another CPU holds a reservation in the 4 KiB frame at `frame`: `cpu`'s stores
    // there must go through the bus (store) rather than a cached writable host page.
    [[nodiscard]] bool watched(unsigned cpu, PhysicalAddress frame) const {
        for (unsigned other = 0; other < max_cpus; ++other) {
            const Reservation& r = reservations_[other];
            if (other != cpu && r.held && r.line * line_size / frame_size == frame.value / frame_size) {
                return true;
            }
        }
        return false;
    }

    // Every CPU's writable host pages may need dropping: some CPU claimed a code frame
    // (Cpu::claim_code_frame) or took a reservation. Each CPU compares generation() with the
    // value it saw last.
    void drop_writable_pages() {
        ++generation_;
    }
    [[nodiscard]] std::uint64_t generation() const {
        return generation_;
    }

  private:
    struct Reservation {
        std::uint64_t line{};
        bool held{};
    };
    std::array<Reservation, max_cpus> reservations_{};
    std::uint64_t generation_{};
};

} // namespace ultraviolent::mips
