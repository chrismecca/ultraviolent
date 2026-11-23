#include "support/test.hpp"

#include <ultraviolent/backends/cow_block_store.hpp>
#include <ultraviolent/backends/file_block_store.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace ultraviolent;
using backends::CowBlockStore;

// A fresh directory, removed at the end of the test.
struct Scratch {
    Scratch() {
        std::string pattern = (std::filesystem::temp_directory_path() / "uv-cow-XXXXXX").string();
        path = ::mkdtemp(pattern.data()) != nullptr ? pattern : std::string{};
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    [[nodiscard]] std::string file(const std::string& name) const {
        return (std::filesystem::path{path} / name).string();
    }
    std::string path;
};

// A base image whose byte i is (i * 7 + i / 4096) & 0xff, of `size` bytes.
std::byte pattern_byte(std::uint64_t i) {
    return static_cast<std::byte>((i * 7 + i / 4096) & 0xff);
}
void write_base(const std::string& path, std::uint64_t size) {
    std::vector<char> bytes(size);
    for (std::uint64_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<char>(pattern_byte(i));
    }
    std::ofstream{path, std::ios::binary}.write(bytes.data(), static_cast<std::streamsize>(size));
}

std::vector<std::byte> read(BlockStore& store, std::uint64_t offset, std::size_t length) {
    std::vector<std::byte> bytes(length);
    if (!store.read(offset, bytes)) {
        bytes.clear();
    }
    return bytes;
}

bool matches_base(const std::vector<std::byte>& bytes, std::uint64_t offset) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != pattern_byte(offset + i)) {
            return false;
        }
    }
    return !bytes.empty();
}

// Three clusters and a short fourth.
constexpr std::uint64_t base_size = 3 * CowBlockStore::cluster_size + 5 * 512;

const test::Registration reads_through{
    "backends.cow.reads_the_base_until_written", [](test::Context& t) {
        Scratch s;
        write_base(s.file("base.img"), base_size);
        if (!t.check(CowBlockStore::create(s.file("a.cow"), "base.img").has_value(), "create")) {
            return;
        }
        t.check(CowBlockStore::is_overlay(s.file("a.cow")), "recognized");
        t.check(!CowBlockStore::is_overlay(s.file("base.img")), "a plain image is not");
        auto overlay = backends::open_disk_image(s.file("a.cow"), true);
        if (!t.check(overlay.has_value(), "open")) {
            return;
        }
        BlockStore& o = **overlay;
        t.check_equal(o.size(), base_size);
        // A read across cluster boundaries and into the short last cluster.
        t.check(matches_base(read(o, 512, static_cast<std::size_t>(base_size - 1024)), 512),
                "reads the base");
        t.check(read(o, base_size - 512, 1024).empty(), "past the end");
        // Creating over an existing file fails.
        t.check(!CowBlockStore::create(s.file("a.cow"), "base.img").has_value(), "exists");
    }};

const test::Registration writes_stay{
    "backends.cow.writes_go_to_the_overlay", [](test::Context& t) {
        Scratch s;
        write_base(s.file("base.img"), base_size);
        if (!t.check(CowBlockStore::create(s.file("a.cow"), s.file("base.img")).has_value())) {
            return;
        }
        // A write that straddles clusters 0 and 1, and one in the short last cluster.
        const std::uint64_t at = CowBlockStore::cluster_size - 512;
        const std::vector<std::byte> ones(1024, std::byte{0x11});
        {
            auto overlay = CowBlockStore::open(s.file("a.cow"), true);
            if (!t.check(overlay.has_value(), "open")) {
                return;
            }
            CowBlockStore& o = **overlay;
            t.check(o.write(at, ones), "write");
            t.check(o.write(base_size - 512, std::span{ones}.first(512)), "last cluster");
            t.check_equal(o.clusters_held(), std::uint64_t{3});
            t.check(read(o, at, 1024) == ones, "reads back");
            t.check(matches_base(read(o, 0, static_cast<std::size_t>(at)), 0),
                    "the rest of cluster 0 is the base's");
            t.check(matches_base(read(o, at + 1024, 4096), at + 1024),
                    "the rest of cluster 1 is the base's");
        }
        // The base is unchanged, and the overlay keeps the writes when opened again.
        auto base = backends::FileBlockStore::open(s.file("base.img"), false);
        t.check(matches_base(read(*base, 0, static_cast<std::size_t>(base_size)), 0),
                "base untouched");
        auto again = CowBlockStore::open(s.file("a.cow"), false);
        if (!t.check(again.has_value(), "reopen")) {
            return;
        }
        t.check(read(**again, at, 1024) == ones, "persisted");
        t.check(read(**again, base_size - 512, 512) == std::vector<std::byte>(512, std::byte{0x11}),
                "persisted in the last cluster");
        t.check(!(*again)->write(0, std::span{ones}.first(512)), "read-only open");
        // The overlay is sparse: it takes about the clusters written, not the base's size.
        const auto used = std::filesystem::file_size(s.file("a.cow"));
        t.check(used <= 5 * CowBlockStore::cluster_size, "file size");
    }};

const test::Registration chains{
    "backends.cow.overlays_chain_and_check_the_base", [](test::Context& t) {
        Scratch s;
        write_base(s.file("base.img"), base_size);
        const std::vector<std::byte> twos(512, std::byte{0x22});
        const std::vector<std::byte> threes(512, std::byte{0x33});
        if (!t.check(CowBlockStore::create(s.file("a.cow"), "base.img").has_value())) {
            return;
        }
        {
            auto a = CowBlockStore::open(s.file("a.cow"), true);
            t.check(a.has_value() && (*a)->write(4096, twos), "write a");
        }
        if (!t.check(CowBlockStore::create(s.file("b.cow"), "a.cow").has_value(), "on a")) {
            return;
        }
        {
            auto b = backends::open_disk_image(s.file("b.cow"), true);
            if (!t.check(b.has_value(), "open b")) {
                return;
            }
            t.check(read(**b, 4096, 512) == twos, "a's write through b");
            t.check((*b)->write(8192, threes), "write b");
        }
        auto a = CowBlockStore::open(s.file("a.cow"), false);
        t.check(a.has_value() && matches_base(read(**a, 8192, 512), 8192), "a unchanged by b");
        // A base whose size changed is refused.
        std::filesystem::resize_file(s.file("base.img"), base_size + 512);
        t.check(!CowBlockStore::open(s.file("a.cow"), false).has_value(), "size checked");
        t.check(!backends::open_disk_image(s.file("b.cow"), false).has_value(), "through b too");
    }};

} // namespace
