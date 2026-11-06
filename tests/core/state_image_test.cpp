#include "support/test.hpp"

#include <ultraviolent/core/state_image.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {

using namespace ultraviolent;

const test::Registration round_trip{
    "state_image.round_trip", [](test::Context& t) {
        StateImage image;
        image.put("a", std::uint64_t{0x1122'3344'5566'7788});
        image.put("b", std::uint8_t{7});
        const auto parsed = StateImage::deserialize(image.serialize());
        if (!t.check(parsed.has_value())) {
            return;
        }
        std::uint64_t a = 0;
        std::uint8_t b = 0;
        t.check(parsed->get("a", a) && a == 0x1122'3344'5566'7788);
        t.check(parsed->get("b", b) && b == 7);
        std::uint32_t wrong_size = 0;
        t.check(!parsed->get("a", wrong_size), "a field of another size is not read");
        t.check(!parsed->get("missing", a), "missing fields are reported");
    }};

const test::Registration rejects_garbage{"state_image.rejects_garbage", [](test::Context& t) {
                                             const std::byte junk[] = {std::byte{'X'},
                                                                       std::byte{'Y'}};
                                             t.check(!StateImage::deserialize(junk).has_value());
                                             StateImage image;
                                             image.put("a", std::uint64_t{1});
                                             auto bytes = image.serialize();
                                             bytes.pop_back(); // truncated
                                             t.check(!StateImage::deserialize(bytes).has_value());
                                         }};

const test::Registration sparse{"state_image.sparse_round_trip", [](test::Context& t) {
                                    std::vector<std::byte> bytes(0x30000);
                                    bytes[0x10005] = std::byte{0x42};
                                    StateImage image;
                                    image.put_sparse("m", bytes);
                                    // Only the one nonzero 64 KiB page is kept.
                                    t.check_equal(image.get_bytes("m.contents")->size(),
                                                  std::size_t{0x10000});
                                    std::vector<std::byte> restored(0x30000, std::byte{0x7});
                                    t.check(image.get_sparse("m", restored));
                                    t.check(restored == bytes, "zero pages are cleared");
                                    std::vector<std::byte> small(0x10000);
                                    t.check(!image.get_sparse("m", small), "size mismatch");
                                    t.check(!image.get_sparse("missing", restored));
                                }};

} // namespace

namespace {

const test::Registration prefixed{
    "state_image.prefixed_images", [](test::Context& t) {
        StateImage part;
        part.put("hub.x", std::uint64_t{5});
        StateImage image;
        image.put("hub.x", std::uint64_t{1});
        image.put_image("node1.", part);
        std::uint64_t value = 0;
        t.check(image.get("node1.hub.x", value) && value == 5);
        const StateImage back = image.sub_image("node1.");
        t.check(back.get("hub.x", value) && value == 5, "the prefix is removed");
        t.check(!back.get("node1.hub.x", value), "only the prefixed fields");
        t.check(image.sub_image("node2.").serialize() == StateImage{}.serialize(), "none match");
    }};

} // namespace

namespace {

// Feeds `data` to the streaming deserialize in the pieces it asks for, counting the bytes.
std::optional<StateImage> deserialize_in_pieces(std::span<const std::byte> data, std::uint64_t size,
                                                std::uint64_t& read) {
    read = 0;
    return StateImage::deserialize(size, [&](std::span<std::byte> out) {
        if (data.size() - read < out.size()) {
            return false;
        }
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(read), out.size(), out.begin());
        read += out.size();
        return true;
    });
}

const test::Registration streamed{
    "state_image.streamed", [](test::Context& t) {
        StateImage image;
        image.put("a", std::uint64_t{0x1122'3344'5566'7788});
        std::vector<std::byte> memory(0x20000);
        memory[0x1'0001] = std::byte{9};
        image.put_sparse("m", memory);
        const auto bytes = image.serialize();
        std::uint64_t read = 0;
        const auto parsed = deserialize_in_pieces(bytes, bytes.size(), read);
        if (!t.check(parsed.has_value())) {
            return;
        }
        t.check(parsed->serialize() == bytes, "the same fields as the span form");
        t.check_equal(read, std::uint64_t{bytes.size()});

        t.check(!deserialize_in_pieces(bytes, bytes.size() - 1, read).has_value(),
                "a size that ends inside a field");
        t.check(!deserialize_in_pieces(std::span{bytes}.first(bytes.size() - 1), bytes.size(), read)
                     .has_value(),
                "a reader that runs out");
        // A field length past the declared size is rejected before its bytes are read.
        auto huge = bytes;
        huge[8 + 4 + 1 + 7] = std::byte{0x7f}; // the top byte of key "a"'s value length
        t.check(!deserialize_in_pieces(huge, huge.size(), read).has_value());
        t.check(read < 32, "only the header and the key were read");
    }};

} // namespace
