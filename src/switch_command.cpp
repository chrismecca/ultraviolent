#include "switch_command.hpp"

#include <ultraviolent/backends/ethernet_switch.hpp>

#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>

using namespace ultraviolent;

namespace {

volatile bool stop_requested = false;

extern "C" void request_switch_stop(int) {
    stop_requested = true;
}

void switch_usage() {
    std::fputs("usage: ultraviolent switch SOCKET [--tap IFNAME]\n"
               "\n"
               "Runs a learning Ethernet switch on the Unix socket SOCKET (created; it must not\n"
               "exist) until SIGINT or SIGTERM. Machines join it with --ethernet switch:SOCKET.\n"
               "--tap IFNAME adds a port on an existing TAP interface you own, which joins the\n"
               "switch's machines to the host (and, through a Linux bridge, to a network).\n",
               stderr);
}

} // namespace

int switch_command(std::span<char*> arguments) {
    std::string socket;
    std::string tap;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view word{arguments[i]};
        if (word == "--tap" && i + 1 < arguments.size()) {
            tap = arguments[++i];
        } else if (socket.empty() && !word.starts_with("-")) {
            socket = word;
        } else {
            switch_usage();
            return 2;
        }
    }
    if (socket.empty()) {
        switch_usage();
        return 2;
    }
    auto server = backends::SwitchServer::open(socket, tap);
    if (!server) {
        std::fprintf(stderr, "%s\n", server.error().c_str());
        return 1;
    }
    struct sigaction action{};
    action.sa_handler = request_switch_stop;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    std::fprintf(stderr, "switch on %s%s%s\n", socket.c_str(), tap.empty() ? "" : ", TAP ",
                 tap.c_str());
    (*server)->run(stop_requested);
    return 0;
}
