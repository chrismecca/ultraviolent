#pragma once

#include <ultraviolent/core/block_store.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace ultraviolent::backends {

// A copy-on-write disk image (ORCHESTRATION.adoc "Copy-on-write overlay format"): an overlay
// file that holds the clusters written since it was made, on a base image that is only read.
// Any number of overlays may share one base, and a base may itself be an overlay. The base
// must not change while overlays depend on it; its size is checked when an overlay opens.
class CowBlockStore final : public BlockStore {
  public:
    static constexpr std::uint32_t cluster_size = 65536;
    static constexpr std::uint64_t header_size = 4096;

    // Creates an empty overlay at `path` on `base` (a plain image or another overlay). The
    // base path is stored as given: relative paths are taken from the overlay's directory.
    // Fails if `path` exists.
    static std::expected<void, std::string> create(const std::string& path,
                                                   const std::string& base);
    // Whether `path` starts with an overlay's magic.
    [[nodiscard]] static bool is_overlay(const std::string& path);
    static std::expected<std::unique_ptr<CowBlockStore>, std::string> open(const std::string& path,
                                                                           bool writable);

    CowBlockStore(const CowBlockStore&) = delete;
    CowBlockStore& operator=(const CowBlockStore&) = delete;
    ~CowBlockStore() override;

    [[nodiscard]] std::uint64_t size() const override {
        return size_;
    }
    [[nodiscard]] bool writable() const override {
        return writable_;
    }
    bool read(std::uint64_t offset, std::span<std::byte> bytes) override;
    bool write(std::uint64_t offset, std::span<const std::byte> bytes) override;

    // The base path as stored, and as resolved from the overlay's directory.
    [[nodiscard]] const std::string& base_path() const {
        return base_path_;
    }
    [[nodiscard]] const std::string& resolved_base_path() const {
        return resolved_base_;
    }
    [[nodiscard]] std::uint64_t clusters() const {
        return (size_ + cluster_size - 1) / cluster_size;
    }
    [[nodiscard]] std::uint64_t clusters_held() const;

  private:
    CowBlockStore(int fd, bool writable, std::uint64_t size, std::uint64_t bitmap_offset,
                  std::uint64_t first_cluster, std::string base_path, std::string resolved_base,
                  std::unique_ptr<BlockStore> base, std::vector<std::uint8_t> bitmap);

    [[nodiscard]] bool held(std::uint64_t cluster) const {
        return (bitmap_[cluster / 8] >> (cluster % 8) & 1) != 0;
    }
    [[nodiscard]] std::uint64_t cluster_offset(std::uint64_t cluster) const {
        return first_cluster_ + cluster * cluster_size;
    }
    // The bytes of `cluster` that exist (the last cluster may be short).
    [[nodiscard]] std::uint64_t cluster_length(std::uint64_t cluster) const;

    int fd_;
    bool writable_;
    std::uint64_t size_;
    std::uint64_t bitmap_offset_;
    std::uint64_t first_cluster_;
    std::string base_path_;
    std::string resolved_base_;
    std::unique_ptr<BlockStore> base_;
    std::vector<std::uint8_t> bitmap_;
};

} // namespace ultraviolent::backends
