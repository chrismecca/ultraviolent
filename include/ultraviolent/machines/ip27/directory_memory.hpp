#pragma once

#include <ultraviolent/core/address_space.hpp>
#include <ultraviolent/core/memory_block.hpp>
#include <ultraviolent/core/state_image.hpp>
#include <ultraviolent/core/trace.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
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
// Reads are of window(), which holds every entry as a read returns it at each address that
// reaches it; the machine maps it read-only with this target taking the writes. Doubleword
// and word writes are modeled. The ECC back door (HSPEC + 2 GB) is not.
class DirectoryMemory final : public MmioTarget {
  public:
    static constexpr std::uint64_t window_offset = 0xc000'0000;
    static constexpr std::uint64_t window_size = 0x4000'0000;

    // `bank_bytes[b]` is the memory installed in bank b (0 for empty). Each nonzero size is a
    // power of two from 8 MB to 512 MB.
    DirectoryMemory(Tracer& tracer, const std::array<std::uint64_t, memory_banks>& bank_bytes,
                    DirectoryDimms dimms);

    // Whether the MD addresses the directory as premium (MD_MEMORY_CONFIG DIR_PREMIUM). The
    // Hub sets it; entries are 48 bits wide only with premium DIMMs in premium mode.
    void set_premium_mode(bool premium);

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
    DirectoryDimms dimms_;
    bool premium_mode_{true};
    MemoryBlock window_{window_size};
    // Premium DIMMs only: each entry's 48 bits, which standard mode reads 16 of.
    std::vector<std::uint64_t> entries_;
};

} // namespace ultraviolent::ip27
