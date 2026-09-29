#include <ultraviolent/machines/ip27/directory_memory.hpp>

#include <ultraviolent/core/invariant.hpp>

#include <bit>

// Back-door layout from the IRIX-derived LINUX asm/sn/addrs.h (BDDIR_ENTRY_LO/HI,
// BDPRT_ENTRY) and PRM Table 3-4 (the `L:`, `H:`, `P:` address modifiers); entry widths from
// LINUX sn0/hubmd.h. IP27.adoc "Directory memory" has the model.
namespace ultraviolent::ip27 {

namespace {

// Back-door space per bank: a quarter of the bank window (1 KB per 4 KB page).
constexpr std::uint64_t bank_slice = bank_window / 4;
// MD_PDIR_MASK and MD_SDIR_MASK; premium DIMMs widen the standard bits.
constexpr std::uint64_t premium_bits = 0xffff'ffff'ffff;
constexpr std::uint64_t standard_bits = 0xffff;
constexpr std::uint64_t minimum_bank = std::uint64_t{8} << 20; // MD_SIZE_8MB

} // namespace

DirectoryMemory::DirectoryMemory(Tracer& tracer,
                                 const std::array<std::uint64_t, memory_banks>& bank_bytes,
                                 DirectoryDimms dimms)
    : tracer_{tracer}, bank_bytes_{bank_bytes}, dimms_{dimms} {
    std::size_t entries = 0;
    for (std::size_t bank = 0; bank < memory_banks; ++bank) {
        const std::uint64_t bytes = bank_bytes_[bank];
        invariant(bytes == 0 ||
                      (std::has_single_bit(bytes) && bytes >= minimum_bank && bytes <= bank_window),
                  "bank size must be a power of two from 8 MB to 512 MB");
        bank_first_[bank] = entries;
        entries += bytes / 4 / sizeof(std::uint64_t);
    }
    // hypothesis: directory DRAM contents at power-on are undefined; zero keeps runs
    // deterministic (MemoryBlock).
    entries_.assign(entries, 0);
}

std::size_t DirectoryMemory::entry_index(std::uint64_t offset) const {
    const std::uint64_t bank = offset / bank_slice;
    const std::uint64_t span = bank_bytes_[bank] / 4;
    if (span == 0) {
        return entries_.size();
    }
    // hypothesis: a bank's DIMMs ignore address bits above their size, so back-door
    // addresses beyond the installed directory alias within it. The PROM sizes each bank
    // by testing for this aliasing (observed; IP27.adoc "Directory memory").
    return bank_first_[bank] + (offset % bank_slice % span) / sizeof(std::uint64_t);
}

std::uint64_t DirectoryMemory::entry_mask() const {
    return dimms_ == DirectoryDimms::premium && premium_mode_ ? premium_bits : standard_bits;
}

std::expected<std::uint64_t, AccessFault> DirectoryMemory::mmio_read(std::uint64_t offset,
                                                                     AccessWidth width) {
    if (width == AccessWidth::bits32) {
        // hypothesis: a word access reads its half of the entry doubleword, big-endian
        // (observed: the IRIX kernel reads protection entries a word at a time).
        const auto doubleword = mmio_read(offset & ~std::uint64_t{7}, AccessWidth::bits64);
        if (!doubleword) {
            return doubleword;
        }
        return (offset & 4) != 0 ? *doubleword & 0xffff'ffff : *doubleword >> 32;
    }
    if (width != AccessWidth::bits64) {
        tracer_.log(TraceCategory::hub, "unmodeled directory read {:#010x} width {}", offset,
                    byte_count(width));
        return std::unexpected(AccessFault::unsupported);
    }
    const std::size_t index = entry_index(offset);
    // hypothesis: an empty bank's directory reads as zero. The PROM's probe treats a
    // mismatched read-back as an empty bank (observed), so the read completes.
    return index < entries_.size() ? entries_[index] & entry_mask() : 0;
}

std::expected<void, AccessFault>
DirectoryMemory::mmio_write(std::uint64_t offset, AccessWidth width, std::uint64_t value) {
    if (width == AccessWidth::bits32) {
        // hypothesis: a word access writes its half of the entry doubleword.
        const std::uint64_t aligned = offset & ~std::uint64_t{7};
        const std::uint64_t old = mmio_read(aligned, AccessWidth::bits64).value_or(0);
        const std::uint64_t merged = (offset & 4) != 0 ? (old & ~std::uint64_t{0xffff'ffff}) | value
                                                       : (old & 0xffff'ffff) | (value << 32);
        return mmio_write(aligned, AccessWidth::bits64, merged);
    }
    if (width != AccessWidth::bits64) {
        tracer_.log(TraceCategory::hub, "unmodeled directory write {:#010x} width {}", offset,
                    byte_count(width));
        return std::unexpected(AccessFault::unsupported);
    }
    const std::size_t index = entry_index(offset);
    if (index < entries_.size()) {
        // Bits beyond the entry, including MD_DIR_FORCE_ECC (63), are not stored.
        // hypothesis: ECC fields hold what is written; hardware ECC generation and checking
        // are not modeled.
        const std::uint64_t mask = entry_mask();
        entries_[index] = (entries_[index] & ~mask) | (value & mask);
    }
    return {};
}

void DirectoryMemory::save_state(StateImage& image) const {
    image.put_sparse("directory.entries", std::as_bytes(std::span{entries_}));
}

void DirectoryMemory::load_state(const StateImage& image) {
    // A snapshot without directory state leaves it at its power-on contents.
    (void)image.get_sparse("directory.entries", std::as_writable_bytes(std::span{entries_}));
}

} // namespace ultraviolent::ip27
