// Server entry point: argument parsing, the listening socket, signal
// handling, and the accept loop that hands each connection its own thread
// (see session.hpp's Server::serve for why - a single process legitimately
// holds more than one VkInstance open at once, and serving connections one
// at a time meant the second one waited in the kernel's accept queue for as
// long as the first stayed open).

#define VK_USE_PLATFORM_WAYLAND_KHR

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

#include "proxy_server.hpp"
#include "remoting_commands.inl"
#include "session.hpp"

namespace {

void on_signal(int) { g_stop.store(true); }

int listen_on(const std::string& address, uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "server: socket failed: %s\n", strerror(errno));
        return -1;
    }

    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        fprintf(stderr, "server: bad address '%s'\n", address.c_str());
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "server: bind failed: %s\n", strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) < 0) {
        fprintf(stderr, "server: listen failed: %s\n", strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    std::string address = "0.0.0.0";
    uint16_t port = 24680;
    bool validate = false;
    bool want_wayland = false;
    uint16_t wayland_port = 24681;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--address" && i + 1 < argc) {
            address = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (arg == "--validate") {
            validate = true;
        } else if (arg == "--wayland") {
            want_wayland = true;
        } else if (arg == "--wayland-port" && i + 1 < argc) {
            wayland_port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (arg == "--help") {
            printf("usage: %s [--address ADDR] [--port PORT] [--validate] [--wayland] "
                   "[--wayland-port PORT]\n",
                   argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    Server server;
    if (!server.init_vulkan(validate, want_wayland)) return 1;

    // Without --wayland the server behaves exactly as it always has: no
    // VK_KHR_surface/VK_KHR_wayland_surface were even requested above, so the
    // offscreen path is untouched.
    if (want_wayland && !server.start_wayland(wayland_port)) return 1;

    const int listen_fd = listen_on(address, port);
    if (listen_fd < 0) return 1;

    fprintf(stderr, "server: listening on %s:%u (command set %s)\n", address.c_str(),
            static_cast<unsigned>(port), remoting::kCommandSetDigest);

    while (!g_stop.load()) {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        // One VkInstance is one connection, and a single process can hold
        // several open at once (CTS's CustomInstanceTest keeps its default
        // instance alive while asking for a differently-configured one for a
        // single test). Serving connections one at a time here meant the
        // second one sat in the kernel's accept queue forever while the
        // first stayed open, and the client blocked in vkCreateInstance
        // forever too - a wedge, not a crash, but just as fatal to a run.
        // ObjectTables are already per-connection (see objects.hpp) so
        // handing each connection its own thread costs nothing but a
        // detached std::thread.
        std::thread([&server, fd] {
            server.serve(fd);
            ::close(fd);
        }).detach();
    }

    server.stop_wayland();
    ::close(listen_fd);
    fprintf(stderr, "server: stopped\n");
    return 0;
}
