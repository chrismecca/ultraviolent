#pragma once

#include <ultraviolent/core/address.hpp>
#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/state_image.hpp>

#include <array>
#include <atomic>
#include <cstdint>

namespace ultraviolent::mips {

class Cpu;

// What CPUs sharing one bus must tell each other beyond the memory itself (SMP.adoc "Memory,
// LL/SC, SYNC"): their LL/SC reservations, and when every other CPU must drop the writable
// host pages it caches. A one-CPU machine has none, and its CPU behaves as before.
//
// CPUs share it while running one at a time (reference mode, ADR-023): what one CPU does
// here, another sees at its next engine entry or bus access. On host threads (SMP.adoc "S4"),
// LL/SC is a compare-and-swap in each CPU instead of reservations here, and a CPU that marks
// a code frame waits until every other CPU has dropped its writable pages (acknowledged) or
// is parked: waiting where it stores nothing before it checks the generation again.
class Coherence {
  public:
    // Two nodes of two CPUs (SMP.adoc "S5: the second node").
    static constexpr unsigned max_cpus = 4;
    // hypothesis: a reservation covers one secondary-cache block, 128 bytes (SCBlkSize 32
    // words in the IP27's R10000 mode bits, IP27.adoc "Reset mode bits"); a store by another
    // CPU anywhere in it breaks the reservation.
    static constexpr std::uint64_t line_size = 128;
    static constexpr std::uint64_t frame_size = 4096;

    // LL by `cpu`: a reservation on the line holding `address`. Other CPUs must stop storing
    // into its frame through their host pages, so they are told to drop them.
    void reserve(unsigned cpu, PhysicalAddress address) {
        reservations_[cpu] = {.line = address.value / line_size, .held = 1};
        held_mask_ |= 1u << cpu;
        // Reservations exist only in reference mode, on one thread.
        advance_generation();
    }
    // ERET clears LLbit (UM 14.30); the reservation goes with it.
    void release(unsigned cpu) {
        reservations_[cpu].held = 0;
        held_mask_ &= ~(1u << cpu);
    }
    [[nodiscard]] bool held(unsigned cpu) const {
        return reservations_[cpu].held != 0;
    }

    // A store by `cpu` to [address, address + size) breaks every other CPU's reservation on
    // the lines it touches.
    void store(unsigned cpu, PhysicalAddress address, std::uint64_t size) {
        // On every store with several CPUs: usually no other CPU holds a reservation.
        if ((held_mask_ & ~(1u << cpu)) == 0) [[likely]] {
            return;
        }
        const std::uint64_t first = address.value / line_size;
        const std::uint64_t last = (address.value + size - 1) / line_size;
        for (unsigned other = 0; other < max_cpus; ++other) {
            Reservation& r = reservations_[other];
            if (other != cpu && r.held != 0 && r.line >= first && r.line <= last) {
                r.held = 0;
                held_mask_ &= ~(1u << other);
            }
        }
    }

    // Whether another CPU holds a reservation in the 4 KiB frame at `frame`: `cpu`'s stores
    // there must go through the bus (store) rather than a cached writable host page.
    [[nodiscard]] bool watched(unsigned cpu, PhysicalAddress frame) const {
        if ((held_mask_ & ~(1u << cpu)) == 0) [[likely]] {
            return false;
        }
        for (unsigned other = 0; other < max_cpus; ++other) {
            const Reservation& r = reservations_[other];
            if (other != cpu && r.held != 0 && r.line * line_size / frame_size == frame.value / frame_size) {
                return true;
            }
        }
        return false;
    }

    // Every CPU's writable host pages may need dropping: some CPU claimed a code frame
    // (Cpu::claim_code_frame) or took a reservation. Each CPU compares generation() with the
    // value it saw last.
    // Returns the new generation.
    std::uint64_t drop_writable_pages() {
        return generation_.fetch_add(1, std::memory_order_seq_cst) + 1;
    }
    [[nodiscard]] std::uint64_t generation() const {
        return generation_.load(std::memory_order_relaxed);
    }

    // Host threads: the CPUs, so a waiting CPU can ask one for attention.
    void attach(unsigned index, Cpu& cpu) {
        cpus_[index] = &cpu;
    }
    [[nodiscard]] Cpu* cpu(unsigned index) const {
        return cpus_[index];
    }
    // `cpu` has dropped its writable pages up to `generation`.
    void acknowledge(unsigned cpu, std::uint64_t generation) {
        seen_[cpu].store(generation, std::memory_order_release);
    }
    // Parked: waiting (for the device lock, for the other CPUs, or not running) where it
    // stores nothing to memory until it has looked at the generation again (Cpu::unpark).
    void park(unsigned cpu) {
        parked_[cpu].store(true, std::memory_order_seq_cst);
    }
    void unpark(unsigned cpu) {
        parked_[cpu].store(false, std::memory_order_seq_cst);
    }
    // Whether `cpu` can no longer store through a writable page cached before `generation`.
    [[nodiscard]] bool acknowledged(unsigned cpu, std::uint64_t generation) const {
        return cpus_[cpu] == nullptr || parked_[cpu].load(std::memory_order_seq_cst) ||
               seen_[cpu].load(std::memory_order_acquire) >= generation;
    }
    // The generation, ordered after this thread's earlier unpark.
    [[nodiscard]] std::uint64_t generation_after_unpark() const {
        return generation_.load(std::memory_order_seq_cst);
    }

    // Reset: no CPU holds a reservation.
    void clear() {
        reservations_ = {};
        held_mask_ = 0;
        advance_generation();
    }

    // CPUs 0 and 1 keep the two-CPU snapshot field; CPUs 2 and 3 have their own.
    void save_state(StateImage& image) const {
        image.put("coherence.reservations", half(0));
        image.put("coherence.reservations_2", half(2));
    }
    void load_state(const StateImage& image) {
        for (const unsigned first : {0u, 2u}) {
            std::array<Reservation, 2> pair = half(first);
            image.get(first == 0 ? "coherence.reservations" : "coherence.reservations_2", pair);
            reservations_[first] = pair[0];
            reservations_[first + 1] = pair[1];
        }
        held_mask_ = 0;
        for (unsigned cpu = 0; cpu < max_cpus; ++cpu) {
            held_mask_ |= reservations_[cpu].held != 0 ? 1u << cpu : 0;
        }
        advance_generation();
    }

  private:
    // No padding: snapshots save the array's bytes.
    struct Reservation {
        std::uint64_t line{};
        std::uint64_t held{};
    };
    // From one thread only (no CPU running on another).
    void advance_generation() {
        generation_.store(generation_.load(std::memory_order_relaxed) + 1,
                          std::memory_order_relaxed);
    }
    [[nodiscard]] std::array<Reservation, 2> half(unsigned first) const {
        return {reservations_[first], reservations_[first + 1]};
    }
    std::array<Reservation, max_cpus> reservations_{};
    // Bit n: CPU n holds a reservation (reservations_[n].held).
    unsigned held_mask_{};
    std::atomic<std::uint64_t> generation_{};
    // Host threads only.
    std::array<Cpu*, max_cpus> cpus_{};
    std::array<std::atomic<std::uint64_t>, max_cpus> seen_{};
    std::array<std::atomic<bool>, max_cpus> parked_{};
};

} // namespace ultraviolent::mips
