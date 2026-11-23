#include "support/test.hpp"

#include <ultraviolent/backends/console_socket.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace ultraviolent;

struct Recorder final : devices::SerialSink {
    void serial_transmit(std::uint8_t byte) override {
        text.push_back(static_cast<char>(byte));
    }
    std::string text;
};

int connect_to(const std::string& path) {
    sockaddr_un where{};
    where.sun_family = AF_UNIX;
    std::memcpy(where.sun_path, path.c_str(), path.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&where), sizeof where) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// What arrives on `fd` within a short while.
std::string drain(int fd) {
    std::string text;
    std::array<char, 8192> buffer{};
    for (int i = 0; i < 20; ++i) {
        const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (n > 0) {
            text.append(buffer.data(), static_cast<std::size_t>(n));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    return text;
}

// Polls the console until it yields a byte or gives up.
std::optional<std::uint8_t> poll(backends::ConsoleSocket& console) {
    for (int i = 0; i < 200; ++i) {
        if (const auto byte = console.serial_receive()) {
            return byte;
        }
        std::this_thread::sleep_for(std::chrono::microseconds{500});
    }
    return std::nullopt;
}

void print(backends::ConsoleSocket& console, std::string_view text) {
    for (const char c : text) {
        console.serial_transmit(static_cast<std::uint8_t>(c));
    }
}

const test::Registration attach{
    "console_socket.attach_replay_type_detach", [](test::Context& t) {
        std::string pattern = (std::filesystem::temp_directory_path() / "uv-con-XXXXXX").string();
        if (!t.check(::mkdtemp(pattern.data()) != nullptr, "scratch")) {
            return;
        }
        const std::string path = pattern + "/console";
        Recorder log;
        {
            auto opened = backends::ConsoleSocket::open(path, log, std::chrono::milliseconds{0});
            if (!t.check(opened.has_value(), "listen")) {
                return;
            }
            auto& console = **opened;
            print(console, "PROM> boot\nlogin: ");
            t.check(!poll(console), "no client, no input");
            const int client = connect_to(path);
            if (!t.check(client >= 0, "connect")) {
                return;
            }
            // Attaching replays recent output, then what follows arrives too.
            t.check(!poll(console), "nothing typed yet");
            t.check(console.attached(), "attached");
            t.check_equal(drain(client), std::string{"PROM> boot\nlogin: "});
            print(console, "root\n");
            t.check_equal(drain(client), std::string{"root\n"});
            // What the client types reaches the guest.
            t.check(::send(client, "ls\r", 3, 0) == 3, "type");
            t.check(poll(console) == std::uint8_t{'l'}, "l");
            t.check(poll(console) == std::uint8_t{'s'}, "s");
            t.check(poll(console) == std::uint8_t{'\r'}, "Enter");
            // The log saw everything.
            t.check_equal(log.text, std::string{"PROM> boot\nlogin: root\n"});
            // A client that leaves detaches; another may attach.
            ::close(client);
            poll(console);
            poll(console);
            t.check(!console.attached(), "detached");
            const int again = connect_to(path);
            poll(console);
            t.check(console.attached(), "reattached");
            t.check(drain(again).ends_with("login: root\n"), "replay again");
            ::close(again);
        }
        t.check(!std::filesystem::exists(path), "socket removed");
        std::filesystem::remove_all(pattern);
    }};

} // namespace
