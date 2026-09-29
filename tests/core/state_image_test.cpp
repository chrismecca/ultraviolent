#include "support/test.hpp"

#include <ultraviolent/core/state_image.hpp>

#include <cstdint>

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
