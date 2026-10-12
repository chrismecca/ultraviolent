#pragma once

#include <ultraviolent/core/state_image.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

namespace ultraviolent::mips {

// The CP0 registers CACHE operations read and write (UM 14.20-14.21, chapter 10).
struct CacheRegisters {
    std::uint32_t tag_lo{};
    std::uint32_t tag_hi{};
    std::uint32_t ecc{};
};

// The R10000's cache tag and data arrays as CACHE operations see them (UM chapter 10;
// MIPS.adoc "Caches"). Index operations read and write the arrays; Hit operations compare
// their tags. Refills are not modeled: the arrays hold only what CACHE operations put there,
// and memory stands for every line they do not hold.
//
// The exception is a primary data line that software made valid itself (Index Store Tag):
// its data is in this processor's cache and may be nowhere else (the IP27 PROM keeps each
// CPU's first stack so, IP27.adoc "Two CPUs on a Hub"). While any is valid, cached data
// references consult the primary data cache (data_reference), and a miss replaces the set's
// least recently used way, writing back a line that stores changed (UM 10.3).
//
// Primary caches: 32 KB, two ways, 64-byte (I) and 32-byte (D) blocks. The secondary cache
// size and block size come from the Config mode bits.
class CacheArrays {
  public:
    explicit CacheArrays(std::uint32_t config);

    // Performs CACHE operation `op` (the instruction's 5-bit field, UM Table 10-1) on virtual
    // address `va`, which translated to physical address `pa` (bits 39:0). Returns the new
    // Status.CH value when the operation defines one. Undefined operations have no effect.
    std::optional<bool> execute(unsigned op, std::uint64_t va, std::uint64_t pa,
                                CacheRegisters& registers);

    // A primary data line leaving the cache with data memory does not have.
    struct Writeback {
        std::uint64_t address; // physical, 32-byte aligned
        std::array<std::uint32_t, 8> words;
        friend bool operator==(const Writeback&, const Writeback&) = default;
    };
    // Lines CACHE operations wrote back since the last call (Index WriteBack Invalidate, Hit
    // WriteBack Invalidate, and secondary operations that invalidate primary subsets).
    [[nodiscard]] std::vector<Writeback> take_writebacks() {
        return std::exchange(writebacks_, {});
    }

    // Whether any primary data line is valid.
    [[nodiscard]] bool holds_data_lines() const {
        return valid_data_lines_ != 0;
    }
    // A cached data reference to physical `pa` with virtual index bits `va` (13:0): the way
    // that holds the line, now the most recently used; or none, after the miss replaced the
    // least recently used way (leaving it to memory), with `replaced` set when that way's line
    // must be written back.
    std::optional<unsigned> data_reference(std::uint64_t va, std::uint64_t pa,
                                           std::optional<Writeback>& replaced);
    // Reads or writes `bytes` (1, 2, 4, or 8, naturally aligned) of the line in `way` at
    // virtual index `va`, big-endian. A write makes the line dirty and Inconsistent.
    [[nodiscard]] std::uint64_t read_data(std::uint64_t va, unsigned way, unsigned bytes) const;
    void write_data(std::uint64_t va, unsigned way, unsigned bytes, std::uint64_t value);

    void save_state(StateImage& image, std::string_view prefix = {}) const;
    void load_state(const StateImage& image, std::string_view prefix = {});

    // Every array and its geometry, for comparing execution engines.
    friend bool operator==(const CacheArrays&, const CacheArrays&) = default;

  private:
    // TagLo/TagHi images of a tag entry, holding only the fields the array stores.
    struct Tag {
        std::uint32_t lo{};
        std::uint32_t hi{};
        friend bool operator==(const Tag&, const Tag&) = default;
    };

    [[nodiscard]] std::size_t secondary_set(std::uint64_t pa) const;
    [[nodiscard]] std::size_t secondary_mru_index(std::size_t set) const;
    void invalidate_primary_instruction(std::size_t index);
    void invalidate_primary_data(std::size_t index);
    // Sets a primary data tag, keeping valid_data_lines_.
    void set_data_tag(std::size_t index, Tag tag);
    // Queues the line in primary data entry `index` for writing back when stores changed it.
    void write_back_data(std::size_t index);
    [[nodiscard]] std::optional<Writeback> data_writeback(std::size_t index) const;
    // Invalidates the primary blocks that are subsets of the secondary block at physical
    // address `block` with virtual index bits `pidx` (VA 13:12).
    void invalidate_subsets(std::uint64_t block, unsigned pidx, bool write_back);
    // Hit/Index invalidation of secondary entry `index` in `set`, UM 10.4, 10.13, 10.16.
    void invalidate_secondary(std::size_t set, unsigned way, std::uint64_t va, std::uint64_t pa,
                              bool write_back);

    void primary_instruction(unsigned operation, std::uint64_t va, std::uint64_t pa,
                             CacheRegisters& registers);
    void primary_data(unsigned operation, std::uint64_t va, std::uint64_t pa,
                      CacheRegisters& registers);
    std::optional<bool> secondary(unsigned operation, std::uint64_t va, std::uint64_t pa,
                                  CacheRegisters& registers);

    unsigned secondary_block_shift_;
    std::size_t secondary_sets_;
    std::vector<Tag> instruction_tags_; // [set * 2 + way]
    std::vector<std::uint8_t> instruction_lru_;
    std::vector<std::uint64_t> instruction_data_; // 36-bit instruction | parity << 36
    std::vector<Tag> data_tags_;
    std::vector<std::uint8_t> data_lru_;
    std::vector<std::uint32_t> data_words_;
    std::vector<std::uint8_t> data_parity_;
    std::vector<Tag> secondary_tags_;
    std::vector<std::uint8_t> secondary_mru_;
    std::vector<std::uint64_t> secondary_data_;  // [(way * sets + set) * block dwords + dw]
    std::vector<std::uint16_t> secondary_check_; // one per quadword
    std::size_t valid_data_lines_{};
    std::vector<Writeback> writebacks_;
};

} // namespace ultraviolent::mips
