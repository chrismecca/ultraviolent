#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ultraviolent::mips {

// One joint-TLB entry, held in the formats of the CP0 registers that load it.
struct TlbEntry {
    // PageMask register format: MASK in bits 24:13.
    std::uint64_t page_mask{};
    // EntryHi format with the VPN2 bits covered by the mask cleared.
    std::uint64_t entry_hi{};
    // EntryLo formats without the G bit, which is stored once per entry.
    std::uint64_t entry_lo0{};
    std::uint64_t entry_lo1{};
    bool global{};
    // False when the entry cannot match: never written since power-on, or invalidated by a
    // conflicting write (UM 14.10 "TS").
    bool enabled{};
};

struct TlbLookup {
    std::size_t index;
    // True when the address selects the odd page of the pair (EntryLo1).
    bool odd;
};

// The R10000 64-entry joint TLB (UM 16.3). Each entry maps an even/odd pair of pages whose
// size is set per entry by PageMask.
class Tlb {
  public:
    static constexpr std::size_t entry_count = 64;

    // Power-on contents are undefined; Ultraviolent starts with every entry disabled.
    void clear();

    [[nodiscard]] const TlbEntry& entry(std::size_t index) const;

    // Writes `entry` at `index`. Any other entry that could match an address also matched by
    // the new entry is invalidated first, as the R10000 does to prevent multiple matches
    // (UM 14.10 "TS"). Returns true when such a conflict existed.
    bool write(std::size_t index, TlbEntry entry);

    // Entry matching region, VPN2, and ASID of `entry_hi` (TLBP semantics).
    [[nodiscard]] std::optional<std::size_t> probe(std::uint64_t entry_hi) const;

    // Entry translating virtual address `address` for `asid`.
    [[nodiscard]] std::optional<TlbLookup> lookup(std::uint64_t address, std::uint8_t asid) const;

    // Bytes in one page of the entry, 4 KiB to 16 MiB.
    [[nodiscard]] static std::uint64_t page_size(const TlbEntry& entry);

  private:
    std::array<TlbEntry, entry_count> entries_{};
    // Index of the most recent match, tried first. Transparent: write() keeps enabled entries
    // from overlapping, so a match is unique and the scan order cannot change the result.
    // Measured: the full scan was 44% of PROM run time (callgrind, 2026-09-24).
    mutable std::size_t last_match_{};
};

} // namespace ultraviolent::mips
