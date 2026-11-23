#include "support/test.hpp"

#include <ultraviolent/backends/ethernet_switch.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace ultraviolent;
using backends::SwitchTable;

// A frame from station `from` to `to` (0xff: broadcast), with one payload byte.
std::vector<std::byte> frame(unsigned to, unsigned from, unsigned payload = 0) {
    std::vector<std::byte> f(60);
    for (unsigned i = 0; i < 6; ++i) {
        f[i] = std::byte(to == 0xff ? 0xff : (i == 5 ? to : (i == 0 ? 0x08 : 0)));
        f[6 + i] = std::byte(i == 5 ? from : (i == 0 ? 0x08 : 0));
    }
    f[14] = std::byte(payload);
    return f;
}

const test::Registration learns{
    "backends.switch.learns_and_floods", [](test::Context& t) {
        SwitchTable table;
        const std::vector<unsigned> ports{1, 2, 3};
        const auto now = SwitchTable::Clock::now();
        // Unknown destination: every other port. The source is learned.
        t.check(table.forward(1, frame(2, 1), ports, now) == std::vector<unsigned>{2, 3}, "flood");
        // The reply goes only to the learned port.
        t.check(table.forward(2, frame(1, 2), ports, now) == std::vector<unsigned>{1}, "learned");
        // Broadcast floods even when known.
        t.check(table.forward(2, frame(0xff, 2), ports, now) == std::vector<unsigned>{1, 3},
                "broadcast");
        // A station on the sender's own port: nothing to forward.
        t.check(table.forward(1, frame(1, 1), ports, now).empty(), "same port");
        // Too short for its addresses: dropped.
        t.check(table.forward(1, std::vector<std::byte>(11), ports, now).empty(), "runt");
        // A port that went away is forgotten: frames to its stations flood again.
        table.forget(1);
        t.check(table.forward(2, frame(1, 2), std::vector<unsigned>{2, 3}, now) ==
                    std::vector<unsigned>{3},
                "forgotten");
        // Entries age out.
        SwitchTable aging{std::chrono::seconds{10}};
        aging.forward(1, frame(2, 1), ports, now);
        t.check(aging.forward(2, frame(1, 2), ports, now + std::chrono::seconds{11}) ==
                    std::vector<unsigned>{1, 3},
                "aged");
    }};

const test::Registration carries{
    "backends.switch.carries_frames_between_links", [](test::Context& t) {
        std::string pattern = (std::filesystem::temp_directory_path() / "uv-sw-XXXXXX").string();
        if (!t.check(::mkdtemp(pattern.data()) != nullptr, "scratch")) {
            return;
        }
        const std::string socket = pattern + "/switch";
        {
            auto server = backends::SwitchServer::open(socket, "");
            if (!t.check(server.has_value(), "listen")) {
                return;
            }
            t.check(!backends::SwitchServer::open(socket, "").has_value(), "socket in use");
            std::string error;
            auto a = backends::SwitchLink::open(socket, error);
            auto b = backends::SwitchLink::open(socket, error);
            auto c = backends::SwitchLink::open(socket, error);
            if (!t.check(a && b && c, "connect")) {
                return;
            }
            const auto pump = [&] {
                for (int i = 0; i < 4; ++i) {
                    (*server)->serve_once(std::chrono::milliseconds{20});
                }
            };
            pump();
            t.check_equal((*server)->ports(), std::size_t{3});
            // A's frame to an unknown station reaches B and C.
            a->send(frame(2, 1, 0x41));
            pump();
            auto at_b = b->receive();
            auto at_c = c->receive();
            t.check(at_b && *at_b == frame(2, 1, 0x41), "b got it");
            t.check(at_c && *at_c == frame(2, 1, 0x41), "c got it");
            t.check(!a->receive(), "not back to a");
            // B answers A: only A gets it.
            b->send(frame(1, 2, 0x42));
            pump();
            auto at_a = a->receive();
            t.check(at_a && *at_a == frame(1, 2, 0x42), "a got the reply");
            t.check(!c->receive(), "c did not");
            // A port that disconnects is dropped.
            c.reset();
            pump();
            t.check_equal((*server)->ports(), std::size_t{2});
        }
        t.check(!std::filesystem::exists(socket), "socket removed");
        // With the switch gone, a link cannot connect and says how to start one.
        std::string error;
        t.check(!backends::SwitchLink::open(socket, error) &&
                    error.find("ultraviolent switch") != std::string::npos,
                "no switch");
        std::filesystem::remove_all(pattern);
    }};

} // namespace
