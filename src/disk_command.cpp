#include "disk_command.hpp"

#include <ultraviolent/backends/cow_block_store.hpp>
#include <ultraviolent/backends/file_block_store.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace ultraviolent;

namespace {

void disk_usage() {
    std::fputs("usage: ultraviolent disk create OVERLAY BASE\n"
               "       ultraviolent disk info IMAGE\n"
               "       ultraviolent disk flatten IMAGE OUTPUT\n"
               "\n"
               "create makes a copy-on-write overlay on BASE (a disk image or another overlay):\n"
               "--disk OVERLAY then reads BASE's blocks until the guest writes them, and writes\n"
               "only to OVERLAY. BASE is stored as given; a relative path is taken from\n"
               "OVERLAY's directory. BASE must not change while overlays use it.\n"
               "info describes an image, and each base under an overlay.\n"
               "flatten writes IMAGE's contents (an overlay with its bases) to a new plain\n"
               "image OUTPUT, sparse where the contents are zero.\n",
               stderr);
}

int create(const std::string& overlay, const std::string& base) {
    if (auto made = backends::CowBlockStore::create(overlay, base); !made) {
        std::fprintf(stderr, "%s\n", made.error().c_str());
        return 1;
    }
    return 0;
}

int info(std::string path) {
    for (unsigned depth = 0;; ++depth) {
        const std::string indent(2 * depth, ' ');
        if (!backends::CowBlockStore::is_overlay(path)) {
            auto plain = backends::FileBlockStore::open(path, false);
            if (!plain) {
                std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), std::strerror(errno));
                return 1;
            }
            std::printf("%s%s: plain image, %llu bytes\n", indent.c_str(), path.c_str(),
                        static_cast<unsigned long long>(plain->size()));
            return 0;
        }
        auto overlay = backends::CowBlockStore::open(path, false);
        if (!overlay) {
            std::fprintf(stderr, "%s\n", overlay.error().c_str());
            return 1;
        }
        const auto& o = **overlay;
        std::printf("%s%s: copy-on-write overlay, %llu bytes, %llu of %llu clusters of %u bytes "
                    "held, on %s\n",
                    indent.c_str(), path.c_str(), static_cast<unsigned long long>(o.size()),
                    static_cast<unsigned long long>(o.clusters_held()),
                    static_cast<unsigned long long>(o.clusters()),
                    backends::CowBlockStore::cluster_size, o.base_path().c_str());
        path = o.resolved_base_path();
    }
}

int flatten(const std::string& input, const std::string& output) {
    auto image = backends::open_disk_image(input, false);
    if (!image) {
        std::fprintf(stderr, "%s\n", image.error().c_str());
        return 1;
    }
    const int fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "cannot create %s: %s\n", output.c_str(), std::strerror(errno));
        return 1;
    }
    const std::uint64_t size = (*image)->size();
    bool ok = ::ftruncate(fd, static_cast<off_t>(size)) == 0;
    std::vector<std::byte> chunk(backends::CowBlockStore::cluster_size);
    for (std::uint64_t offset = 0; ok && offset < size; offset += chunk.size()) {
        const auto part = std::span{chunk}.first(
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), size - offset)));
        ok = (*image)->read(offset, part);
        // Zero chunks stay holes.
        if (ok && std::ranges::any_of(part, [](std::byte b) { return b != std::byte{}; })) {
            ok = backends::write_fully(fd, offset, part);
        }
    }
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) {
        std::fprintf(stderr, "cannot write %s: %s\n", output.c_str(), std::strerror(errno));
        ::unlink(output.c_str());
        return 1;
    }
    return 0;
}

} // namespace

int disk_command(std::span<char*> arguments) {
    const auto word = [&](std::size_t i) { return std::string{arguments[i]}; };
    if (arguments.size() == 3 && std::string_view{arguments[0]} == "create") {
        return create(word(1), word(2));
    }
    if (arguments.size() == 2 && std::string_view{arguments[0]} == "info") {
        return info(word(1));
    }
    if (arguments.size() == 3 && std::string_view{arguments[0]} == "flatten") {
        return flatten(word(1), word(2));
    }
    disk_usage();
    return 2;
}
