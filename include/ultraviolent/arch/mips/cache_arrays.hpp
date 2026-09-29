#pragma once

#include <ultraviolent/core/state_image.hpp>

#include <cstdint>
#include <optional>
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
// their tags. Loads, stores, and fetches never use them: memory is always coherent, so the
// arrays hold only what CACHE operations put there.
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

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

  private:
    // TagLo/TagHi images of a tag entry, holding only the fields the array stores.
    struct Tag {
        std::uint32_t lo{};
        std::uint32_t hi{};
    };

    [[nodiscard]] std::size_t secondary_set(std::uint64_t pa) const;
    [[nodiscard]] std::size_t secondary_mru_index(std::size_t set) const;
    void invalidate_primary_instruction(std::size_t index);
    void invalidate_primary_data(std::size_t index);
    // Invalidates the primary blocks that are subsets of the secondary block at physical
    // address `block` with virtual index bits `pidx` (VA 13:12).
    void invalidate_subsets(std::uint64_t block, unsigned pidx);
    // Hit/Index invalidation of secondary entry `index` in `set`, UM 10.4, 10.13, 10.16.
    void invalidate_secondary(std::size_t set, unsigned way, std::uint64_t va, std::uint64_t pa);

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
};

} // namespace ultraviolent::mips
