#pragma once

#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/memory_block.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

namespace ultraviolent::ip27 {

// Memory banks per node (LINUX sn0/hubmd.h MD_MEM_BANKS, M mode), each decoded in a 512 MB
// window (MD_BANK_SIZE).
inline constexpr std::size_t memory_banks = 8;
inline constexpr std::uint64_t bank_window = std::uint64_t{1} << 29;

// Directory DIMM population (PRM 1.4): standard directory bits live in the memory DIMMs;
// premium adds directory DIMMs that widen each entry.
enum class DirectoryDimms : std::uint8_t { standard, premium };

// The Hub MD's back-door view of directory and protection memory, at HSPEC + 3 GB
// (IP27.adoc "Directory memory"). Each 4 KB page of memory has 1 KB here: 64 protection
// doublewords, then 32 two-doubleword directory entries.
//
// Reads are of window(), which holds every entry as a read returns it where the bank's
// directory starts (bank b at b * bank_slice); the machine maps that much of each bank,
// installed(b) bytes, read-only with this target taking the writes. The rest of each slice
// (aliases of a small bank's directory, and empty banks) is mapped to this target, whose
// reads fold the address. Doubleword and word writes are modeled. The ECC back door
// (HSPEC + 2 GB) is not.
class DirectoryMemory final : public MmioTarget {
  public:
    static constexpr std::uint64_t window_offset = 0xc000'0000;
    static constexpr std::uint64_t window_size = 0x4000'0000;
    // Back-door space per bank: a quarter of the bank window (1 KB per 4 KB page).
    static constexpr std::uint64_t bank_slice = bank_window / 4;

    // `bank_bytes[b]` is the memory installed in bank b (0 for empty). Each nonzero size is a
    // power of two from 8 MB to 512 MB.
    DirectoryMemory(Tracer& tracer, const std::array<std::uint64_t, memory_banks>& bank_bytes,
                    DirectoryDimms dimms);

    // Whether the MD addresses the directory as premium (MD_MEMORY_CONFIG DIR_PREMIUM). The
    // Hub sets it; entries are 48 bits wide only with premium DIMMs in premium mode.
    void set_premium_mode(bool premium);

    // Bytes of bank `bank`'s directory, from the start of its slice (0 for an empty bank).
    [[nodiscard]] std::uint64_t installed(std::size_t bank) const {
        return bank_bytes_[bank] / 4;
    }

    // How the machine maps the window, in order and covering it: a bank's installed
    // directory as memory (window() at `offset`, read-only, writes to `target`), or
    // aliases and empty banks as `target` alone. `target` takes offsets from the region's
    // start, as an address space gives them.
    struct Region {
        std::uint64_t offset;
        std::uint64_t size;
        bool memory;
        MmioTarget* target;
    };
    [[nodiscard]] const std::vector<Region>& regions() const {
        return regions_;
    }

    // What reads of the window return, big-endian (window_size bytes): memory loads, so a CPU
    // reads it as RAM (measured: the PROM's directory test reads all of it three times).
    // Written only by mmio_write.
    [[nodiscard]] MemoryBlock& window() {
        return window_;
    }

    void save_state(StateImage& image) const;
    void load_state(const StateImage& image);

    std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                        AccessWidth width) override;
    std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                std::uint64_t value) override;

  private:
    // This directory at an offset in the window (Region::target).
    class View final : public MmioTarget {
      public:
        View(DirectoryMemory& directory, std::uint64_t base)
            : directory_{directory}, base_{base} {}
        std::expected<std::uint64_t, AccessFault> mmio_read(std::uint64_t offset,
                                                            AccessWidth width) override {
            return directory_.mmio_read(base_ + offset, width);
        }
        std::expected<void, AccessFault> mmio_write(std::uint64_t offset, AccessWidth width,
                                                    std::uint64_t value) override {
            return directory_.mmio_write(base_ + offset, width, value);
        }

      private:
        DirectoryMemory& directory_;
        std::uint64_t base_;
    };

    void add_region(std::uint64_t offset, std::uint64_t size, bool memory);
    [[nodiscard]] std::uint64_t entry_mask() const;
    // Entry `index` of bank `bank` reads as `value` at every address that reaches it.
    void show(std::size_t bank, std::uint64_t index, std::uint64_t value);
    // Every entry as read: entries_ under the mask, or the window's first alias.
    [[nodiscard]] std::vector<std::uint64_t> entries() const;
    void show_entries(const std::vector<std::uint64_t>& entries);

    Tracer& tracer_;
    std::array<std::uint64_t, memory_banks> bank_bytes_;
    // First entry of each bank in entries().
    std::array<std::size_t, memory_banks> bank_first_{};
    std::size_t entry_count_{};
    // Whether each bank's entries start where the window has its first alias, as when every
    // bank but the last is full (the machine's memory layout): saved state then loads in place.
    bool in_window_order_{true};
    DirectoryDimms dimms_;
    bool premium_mode_{true};
    MemoryBlock window_{window_size};
    // Premium DIMMs only: each entry's 48 bits, which standard mode reads 16 of.
    std::vector<std::uint64_t> entries_;
    std::vector<std::unique_ptr<View>> views_;
    std::vector<Region> regions_;
};

} // namespace ultraviolent::ip27
