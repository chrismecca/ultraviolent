#include <ultraviolent/machines/ip27/directory_memory.hpp>

#include <ultraviolent/core/byte_order.hpp>
#include <ultraviolent/core/invariant.hpp>

#include <bit>
#include <span>

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
constexpr std::uint64_t entry_bytes = sizeof(std::uint64_t);

} // namespace

DirectoryMemory::DirectoryMemory(Tracer& tracer,
                                 const std::array<std::uint64_t, memory_banks>& bank_bytes,
                                 DirectoryDimms dimms)
    : tracer_{tracer}, bank_bytes_{bank_bytes}, dimms_{dimms} {
    for (std::size_t bank = 0; bank < memory_banks; ++bank) {
        const std::uint64_t bytes = bank_bytes_[bank];
        invariant(bytes == 0 ||
                      (std::has_single_bit(bytes) && bytes >= minimum_bank && bytes <= bank_window),
                  "bank size must be a power of two from 8 MB to 512 MB");
        bank_first_[bank] = entry_count_;
        entry_count_ += bytes / 4 / entry_bytes;
    }
    // hypothesis: directory DRAM contents at power-on are undefined; zero keeps runs
    // deterministic (MemoryBlock).
    if (dimms_ == DirectoryDimms::premium) {
        entries_.assign(entry_count_, 0);
    }
}

std::uint64_t DirectoryMemory::entry_mask() const {
    return dimms_ == DirectoryDimms::premium && premium_mode_ ? premium_bits : standard_bits;
}

void DirectoryMemory::set_premium_mode(bool premium) {
    const std::uint64_t mask = entry_mask();
    premium_mode_ = premium;
    if (entry_mask() != mask) {
        show_entries(entries_);
    }
}

void DirectoryMemory::show(std::size_t bank, std::uint64_t index, std::uint64_t value) {
    // hypothesis: a bank's DIMMs ignore address bits above their size, so back-door
    // addresses beyond the installed directory alias within it. The PROM sizes each bank
    // by testing for this aliasing (observed; IP27.adoc "Directory memory").
    const std::uint64_t span = bank_bytes_[bank] / 4;
    for (std::uint64_t at = bank * bank_slice + index * entry_bytes; at < (bank + 1) * bank_slice;
         at += span) {
        const auto bytes = window_.bytes().subspan(at, entry_bytes);
        // Unchanged entries are not written, so pages that stay zero take no host memory.
        if (load_unsigned(bytes, ByteOrder::big) != value) {
            window_.note_write(at, entry_bytes);
            store_unsigned(bytes, value, ByteOrder::big);
        }
    }
}

std::vector<std::uint64_t> DirectoryMemory::entries() const {
    if (!entries_.empty()) {
        return entries_;
    }
    std::vector<std::uint64_t> result(entry_count_);
    for (std::size_t bank = 0; bank < memory_banks; ++bank) {
        for (std::uint64_t i = 0; i < bank_bytes_[bank] / 4 / entry_bytes; ++i) {
            result[bank_first_[bank] + i] = load_unsigned(
                window_.bytes().subspan(bank * bank_slice + i * entry_bytes, entry_bytes),
                ByteOrder::big);
        }
    }
    return result;
}

void DirectoryMemory::show_entries(const std::vector<std::uint64_t>& entries) {
    const std::uint64_t mask = entry_mask();
    for (std::size_t bank = 0; bank < memory_banks; ++bank) {
        for (std::uint64_t i = 0; i < bank_bytes_[bank] / 4 / entry_bytes; ++i) {
            show(bank, i, entries[bank_first_[bank] + i] & mask);
        }
    }
}

std::expected<std::uint64_t, AccessFault> DirectoryMemory::mmio_read(std::uint64_t offset,
                                                                     AccessWidth width) {
    // As the window reads, a word being its half of the entry doubleword, big-endian
    // (observed: the IRIX kernel reads protection entries a word at a time). hypothesis:
    // narrower reads are the entry's bytes likewise. An empty bank's directory reads as zero
    // (hypothesis: the PROM's probe treats a mismatched read-back as an empty bank, observed,
    // so the read completes).
    return load_unsigned(window_.bytes().subspan(offset, byte_count(width)), ByteOrder::big);
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
    const std::size_t bank = offset / bank_slice;
    const std::uint64_t span = bank_bytes_[bank] / 4;
    if (span == 0) {
        // hypothesis: an empty bank's directory ignores writes.
        return {};
    }
    // Bits beyond the entry, including MD_DIR_FORCE_ECC (63), are not stored.
    // hypothesis: ECC fields hold what is written; hardware ECC generation and checking are
    // not modeled.
    const std::uint64_t index = offset % bank_slice % span / entry_bytes;
    const std::uint64_t mask = entry_mask();
    if (!entries_.empty()) {
        std::uint64_t& kept = entries_[bank_first_[bank] + index];
        kept = (kept & ~mask) | (value & mask);
    }
    show(bank, index, value & mask);
    return {};
}

void DirectoryMemory::save_state(StateImage& image) const {
    const std::vector<std::uint64_t> saved = entries();
    image.put_sparse("directory.entries", std::as_bytes(std::span{saved}));
}

void DirectoryMemory::load_state(const StateImage& image) {
    // A snapshot without directory state leaves it at its power-on contents.
    std::vector<std::uint64_t> loaded(entry_count_);
    if (!image.get_sparse("directory.entries", std::as_writable_bytes(std::span{loaded}))) {
        return;
    }
    if (!entries_.empty()) {
        entries_ = loaded;
    }
    show_entries(loaded);
}

} // namespace ultraviolent::ip27
