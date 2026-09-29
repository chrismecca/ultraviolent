#include <ultraviolent/arch/mips/cache_arrays.hpp>

#include <algorithm>
#include <span>
#include <string>

// UM chapter 10 (CACHE operations), 14.20-14.21 (TagLo, TagHi, ECC), 5.x (way prediction),
// 14.14 (Config). MIPS.adoc "Caches" has the model and its hypotheses.
namespace ultraviolent::mips {

namespace {

constexpr unsigned ways = 2;
// Primary caches: 16 KB per way, indexed by VA 13:6 (I, 64-byte blocks) and VA 13:5 (D,
// 32-byte blocks); data by VA 13:2 (UM 10.1, 10.17-10.21).
constexpr std::size_t instruction_sets = 256;
constexpr std::size_t data_sets = 512;
constexpr std::size_t primary_words = 4096; // per way

// Operations, instruction bits 20:18 (UM Table 10-1); bits 17:16 select the cache.
constexpr unsigned index_invalidate = 0; // Index (WriteBack) Invalidate
constexpr unsigned index_load_tag = 1;
constexpr unsigned index_store_tag = 2;
constexpr unsigned hit_invalidate = 4;
constexpr unsigned hit_writeback_invalidate = 5; // Cache Barrier for I
constexpr unsigned index_load_data = 6;
constexpr unsigned index_store_data = 7;

// Fields each tag array stores, as TagLo/TagHi images (UM 10.5-10.10). The LRU (TagLo 3) and
// MRU (TagHi 31) bits are per set and kept separately.
constexpr std::uint32_t instruction_lo_fields = 0xffff'ff45; // 31:8 tag, 6 state, 2, 0 parity
constexpr std::uint32_t data_lo_fields = 0xffff'ffc7;        // 31:8, 7:6 state, 2, 1 SCWay, 0
constexpr std::uint32_t data_hi_fields = 0xe000'000f;        // 31:29 StateMod, 3:0 tag 39:36
constexpr std::uint32_t secondary_lo_fields = 0xffff'cdff;   // 31:14, 11:10, 8:7, 6:0 ECC
constexpr std::uint32_t tag_hi_fields = 0xf;
constexpr std::uint32_t lru_bit = 1u << 3;
constexpr std::uint32_t mru_bit = 1u << 31;

constexpr std::uint32_t instruction_state = (1u << 6) | (1u << 2);      // state and its parity
constexpr std::uint32_t data_state = (3u << 6) | (1u << 2) | (1u << 1); // + SCWay
constexpr std::uint32_t statemod_mask = 7u << 29;
constexpr std::uint32_t statemod_normal = 1u << 29;
constexpr std::uint32_t secondary_state = 3u << 10;
// The secondary way prediction table has 8K entries (UM 5.x).
constexpr std::size_t mru_entries = 8192;

// Physical address bits 39:12 held in a primary tag image, and 39:18 in a secondary one.
constexpr std::uint64_t primary_tag(const std::uint32_t lo, const std::uint32_t hi) {
    return (std::uint64_t{hi & 0xf} << 36) | (std::uint64_t{lo >> 8} << 12);
}
constexpr std::uint64_t secondary_tag(const std::uint32_t lo, const std::uint32_t hi) {
    return (std::uint64_t{hi & 0xf} << 36) | (std::uint64_t{lo >> 14} << 18);
}
constexpr std::uint64_t page_mask = ~std::uint64_t{0xfff};

template <class T>
void put_vector(StateImage& image, const std::string& key, const std::vector<T>& values) {
    image.put_sparse(key, std::as_bytes(std::span{values}));
}
template <class T>
void get_vector(const StateImage& image, const std::string& key, std::vector<T>& values) {
    (void)image.get_sparse(key, std::as_writable_bytes(std::span{values}));
}

} // namespace

CacheArrays::CacheArrays(std::uint32_t config)
    // Config SS (18:16): 512 KB << SS; SB (13): 16- or 32-word blocks (UM 14.14).
    : secondary_block_shift_{(config >> 13 & 1) != 0 ? 7u : 6u},
      secondary_sets_{(std::size_t{512} << 10 << (config >> 16 & 7)) / ways >>
                      secondary_block_shift_},
      instruction_tags_(instruction_sets * ways), instruction_lru_(instruction_sets),
      instruction_data_(primary_words * ways), data_tags_(data_sets * ways), data_lru_(data_sets),
      data_words_(primary_words * ways), data_parity_(primary_words * ways),
      secondary_tags_(secondary_sets_ * ways),
      secondary_mru_(std::min(secondary_sets_, mru_entries)),
      secondary_data_(secondary_sets_ * ways << (secondary_block_shift_ - 3)),
      secondary_check_(secondary_sets_ * ways << (secondary_block_shift_ - 4)) {}

std::optional<bool> CacheArrays::execute(unsigned op, std::uint64_t va, std::uint64_t pa,
                                         CacheRegisters& registers) {
    const unsigned operation = op >> 2 & 7;
    switch (op & 3) {
    case 0:
        primary_instruction(operation, va, pa, registers);
        return std::nullopt;
    case 1:
        primary_data(operation, va, pa, registers);
        return std::nullopt;
    case 3:
        return secondary(operation, va, pa, registers);
    default:
        // Target 2 (tertiary) is undefined on the R10000 (UM Table 10-1).
        return std::nullopt;
    }
}

void CacheArrays::invalidate_primary_instruction(std::size_t index) {
    // State and its parity to 0; the LRU bit does not change (UM 10.2, 10.11).
    instruction_tags_[index].lo &= ~instruction_state;
}

void CacheArrays::invalidate_primary_data(std::size_t index) {
    // State 00, SCWay 0, StateMod 001 (Normal), state parity 0 (UM 10.3, 10.12).
    Tag& tag = data_tags_[index];
    tag.lo &= ~data_state;
    tag.hi = (tag.hi & ~statemod_mask) | statemod_normal;
}

void CacheArrays::primary_instruction(unsigned operation, std::uint64_t va, std::uint64_t pa,
                                      CacheRegisters& registers) {
    const std::size_t set = va >> 6 & (instruction_sets - 1);
    const unsigned way = va & 1;
    const std::size_t index = set * ways + way;
    const std::size_t word = way * primary_words + (va >> 2 & (primary_words - 1));
    switch (operation) {
    case index_invalidate:
        invalidate_primary_instruction(index);
        return;
    case index_load_tag:
        registers.tag_lo = instruction_tags_[index].lo | (instruction_lru_[set] != 0 ? lru_bit : 0);
        registers.tag_hi = instruction_tags_[index].hi;
        return;
    case index_store_tag:
        instruction_tags_[index] = {registers.tag_lo & instruction_lo_fields,
                                    registers.tag_hi & tag_hi_fields};
        instruction_lru_[set] = static_cast<std::uint8_t>((registers.tag_lo & lru_bit) != 0);
        return;
    case hit_invalidate:
        for (unsigned w = 0; w < ways; ++w) {
            const Tag& tag = instruction_tags_[set * ways + w];
            if ((tag.lo & (1u << 6)) != 0 && primary_tag(tag.lo, tag.hi) == (pa & page_mask)) {
                invalidate_primary_instruction(set * ways + w);
            }
        }
        return;
    case index_load_data:
        // A 36-bit predecoded instruction in TagHi 3:0 and TagLo, its parity in ECC 0.
        registers.tag_lo = static_cast<std::uint32_t>(instruction_data_[word]);
        registers.tag_hi = static_cast<std::uint32_t>(instruction_data_[word] >> 32 & 0xf);
        registers.ecc = static_cast<std::uint32_t>(instruction_data_[word] >> 36 & 1);
        return;
    case index_store_data:
        instruction_data_[word] = registers.tag_lo | (std::uint64_t{registers.tag_hi & 0xf} << 32) |
                                  (std::uint64_t{registers.ecc & 1} << 36);
        return;
    default:
        // Cache Barrier, and undefined operations.
        return;
    }
}

void CacheArrays::primary_data(unsigned operation, std::uint64_t va, std::uint64_t pa,
                               CacheRegisters& registers) {
    const std::size_t set = va >> 5 & (data_sets - 1);
    const unsigned way = va & 1;
    const std::size_t index = set * ways + way;
    const std::size_t word = way * primary_words + (va >> 2 & (primary_words - 1));
    switch (operation) {
    case index_invalidate:
        // Index WriteBack Invalidate (D): memory is always current, so nothing is written.
        invalidate_primary_data(index);
        return;
    case index_load_tag:
        registers.tag_lo = data_tags_[index].lo | (data_lru_[set] != 0 ? lru_bit : 0);
        registers.tag_hi = data_tags_[index].hi;
        return;
    case index_store_tag:
        data_tags_[index] = {registers.tag_lo & data_lo_fields, registers.tag_hi & data_hi_fields};
        data_lru_[set] = static_cast<std::uint8_t>((registers.tag_lo & lru_bit) != 0);
        return;
    case hit_invalidate:
    case hit_writeback_invalidate:
        for (unsigned w = 0; w < ways; ++w) {
            const Tag& tag = data_tags_[set * ways + w];
            if ((tag.lo & (3u << 6)) != 0 && primary_tag(tag.lo, tag.hi) == (pa & page_mask)) {
                invalidate_primary_data(set * ways + w);
            }
        }
        return;
    case index_load_data:
        // A word in TagLo and its byte parity in ECC 3:0; TagHi is not written.
        registers.tag_lo = data_words_[word];
        registers.ecc = data_parity_[word];
        return;
    case index_store_data:
        data_words_[word] = registers.tag_lo;
        data_parity_[word] = static_cast<std::uint8_t>(registers.ecc & 0xf);
        return;
    default:
        return;
    }
}

std::size_t CacheArrays::secondary_set(std::uint64_t pa) const {
    // PA[size-2 : block size] (UM 10.1).
    return (pa >> secondary_block_shift_) & (secondary_sets_ - 1);
}

std::size_t CacheArrays::secondary_mru_index(std::size_t set) const {
    // UM Table 5-3 for caches up to 1 MB, where the table index is the set index.
    // hypothesis: larger caches use the low 13 bits of the set index.
    return set % secondary_mru_.size();
}

void CacheArrays::invalidate_subsets(std::uint64_t block, unsigned pidx) {
    // Primary blocks are indexed by virtual address: VA 13:12 is the secondary entry's PIdx
    // and VA 11:0 equals PA 11:0.
    const std::uint64_t size = std::uint64_t{1} << secondary_block_shift_;
    for (std::uint64_t pa = block; pa < block + size; pa += 32) {
        const std::uint64_t va = (std::uint64_t{pidx} << 12) | (pa & 0xfff);
        const std::size_t iset = va >> 6 & (instruction_sets - 1);
        const std::size_t dset = va >> 5 & (data_sets - 1);
        for (unsigned w = 0; w < ways; ++w) {
            const Tag& itag = instruction_tags_[iset * ways + w];
            if ((itag.lo & (1u << 6)) != 0 && primary_tag(itag.lo, itag.hi) == (pa & page_mask)) {
                invalidate_primary_instruction(iset * ways + w);
            }
            const Tag& dtag = data_tags_[dset * ways + w];
            if ((dtag.lo & (3u << 6)) != 0 && primary_tag(dtag.lo, dtag.hi) == (pa & page_mask)) {
                invalidate_primary_data(dset * ways + w);
            }
        }
    }
}

void CacheArrays::invalidate_secondary(std::size_t set, unsigned way, std::uint64_t va,
                                       std::uint64_t pa) {
    Tag& tag = secondary_tags_[set * ways + way];
    const std::uint64_t block =
        secondary_tag(tag.lo, tag.hi) | (static_cast<std::uint64_t>(set) << secondary_block_shift_);
    invalidate_subsets(block & ~((std::uint64_t{1} << secondary_block_shift_) - 1),
                       tag.lo >> 7 & 3);
    // All tag bits are written at once: State 00, the tag and PIdx from the CACHE
    // instruction's PA and VA 13:12. hypothesis: the generated tag ECC is not modeled and
    // reads as zero. Nothing is written back: memory is always current.
    tag.lo = static_cast<std::uint32_t>((pa >> 18) << 14) |
             static_cast<std::uint32_t>((va >> 12 & 3) << 7);
    tag.hi = static_cast<std::uint32_t>(pa >> 36 & 0xf);
    // The MRU bit points away from the invalidated way.
    secondary_mru_[secondary_mru_index(set)] = static_cast<std::uint8_t>(way ^ 1);
}

std::optional<bool> CacheArrays::secondary(unsigned operation, std::uint64_t va, std::uint64_t pa,
                                           CacheRegisters& registers) {
    const std::size_t set = secondary_set(pa);
    const unsigned way = pa & 1;
    const std::size_t index = set * ways + way;
    const std::size_t block_dwords = std::size_t{1} << (secondary_block_shift_ - 3);
    const std::size_t dword =
        (way * secondary_sets_ + set) * block_dwords + (pa >> 3 & (block_dwords - 1));
    switch (operation) {
    case index_invalidate:
        // Index WriteBack Invalidate (S): nothing happens to an invalid entry (UM 10.4).
        if ((secondary_tags_[index].lo & secondary_state) != 0) {
            invalidate_secondary(set, way, va, pa);
        }
        return std::nullopt;
    case index_load_tag:
        registers.tag_lo = secondary_tags_[index].lo;
        registers.tag_hi = secondary_tags_[index].hi |
                           (secondary_mru_[secondary_mru_index(set)] != 0 ? mru_bit : 0);
        return std::nullopt;
    case index_store_tag:
        secondary_tags_[index] = {registers.tag_lo & secondary_lo_fields,
                                  registers.tag_hi & tag_hi_fields};
        secondary_mru_[secondary_mru_index(set)] =
            static_cast<std::uint8_t>((registers.tag_hi & mru_bit) != 0);
        return std::nullopt;
    case hit_invalidate:
    case hit_writeback_invalidate: {
        // CH is set on a hit and cleared on a miss (UM 10.1, 10.13, 10.16).
        const std::uint64_t tag_bits = pa & ~((std::uint64_t{1} << 18) - 1);
        for (unsigned w = 0; w < ways; ++w) {
            const Tag& tag = secondary_tags_[set * ways + w];
            if ((tag.lo & secondary_state) != 0 && secondary_tag(tag.lo, tag.hi) == tag_bits) {
                invalidate_secondary(set, w, va, pa);
                return true;
            }
        }
        return false;
    }
    case index_load_data:
        // The doubleword in TagHi:TagLo and its quadword's check bits in ECC 9:0; MRU
        // unchanged.
        registers.tag_hi = static_cast<std::uint32_t>(secondary_data_[dword] >> 32);
        registers.tag_lo = static_cast<std::uint32_t>(secondary_data_[dword]);
        registers.ecc = secondary_check_[dword / 2];
        return std::nullopt;
    case index_store_data:
        // A quadword: the addressed doubleword from TagHi:TagLo, the other zero (UM 10.22).
        secondary_data_[dword] = (std::uint64_t{registers.tag_hi} << 32) | registers.tag_lo;
        secondary_data_[dword ^ 1] = 0;
        secondary_check_[dword / 2] = static_cast<std::uint16_t>(registers.ecc & 0x3ff);
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

void CacheArrays::save_state(StateImage& image) const {
    put_vector(image, "cache.instruction_tags", instruction_tags_);
    put_vector(image, "cache.instruction_lru", instruction_lru_);
    put_vector(image, "cache.instruction_data", instruction_data_);
    put_vector(image, "cache.data_tags", data_tags_);
    put_vector(image, "cache.data_lru", data_lru_);
    put_vector(image, "cache.data_words", data_words_);
    put_vector(image, "cache.data_parity", data_parity_);
    put_vector(image, "cache.secondary_tags", secondary_tags_);
    put_vector(image, "cache.secondary_mru", secondary_mru_);
    put_vector(image, "cache.secondary_data", secondary_data_);
    put_vector(image, "cache.secondary_check", secondary_check_);
}

void CacheArrays::load_state(const StateImage& image) {
    get_vector(image, "cache.instruction_tags", instruction_tags_);
    get_vector(image, "cache.instruction_lru", instruction_lru_);
    get_vector(image, "cache.instruction_data", instruction_data_);
    get_vector(image, "cache.data_tags", data_tags_);
    get_vector(image, "cache.data_lru", data_lru_);
    get_vector(image, "cache.data_words", data_words_);
    get_vector(image, "cache.data_parity", data_parity_);
    get_vector(image, "cache.secondary_tags", secondary_tags_);
    get_vector(image, "cache.secondary_mru", secondary_mru_);
    get_vector(image, "cache.secondary_data", secondary_data_);
    get_vector(image, "cache.secondary_check", secondary_check_);
}

} // namespace ultraviolent::mips
