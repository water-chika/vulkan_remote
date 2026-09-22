// Server entry point: argument parsing, the listening socket, signal
// handling, and the accept loop that hands each connection its own thread
// (see session.hpp's Server::serve for why - a single process legitimately
// holds more than one VkInstance open at once, and serving connections one
// at a time meant the second one waited in the kernel's accept queue for as
// long as the first stayed open).

#define VK_USE_PLATFORM_WAYLAND_KHR

// windows.h's default (non-lean) mode drags in the legacy winsock.h, which
// conflicts with wire.hpp's winsock2.h if windows.h is reached first in this
// translation unit; defining this before any include - including wire.hpp
// itself, reached transitively through session.hpp below - keeps that from
// happening regardless of what pulls windows.h in first (see client/wsi.cpp
// lines 20-31 for the same reasoning on the client side).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <thread>

#if !defined(_WIN32)
#include "proxy_server.hpp"
#endif
#include "remoting_commands.inl"
#include "session.hpp"
#include "wire.hpp"

namespace {

void on_signal(int) { g_stop.store(true); }

// True for whatever this platform's accept() reports as "the call was
// interrupted, try again" - a signal on POSIX; Windows has no EINTR at all,
// so there is nothing to retry there.
bool accept_was_interrupted() {
#if defined(_WIN32)
    return false;
#else
    return errno == EINTR;
#endif
}

remoting::socket_t listen_on(const std::string& address, uint16_t port) {
    // main.cpp opens this listening socket directly, without ever going
    // through connect_to() first, so Winsock would otherwise never get
    // started on a server process (see wire.hpp's ensure_sockets_initialised).
    remoting::ensure_sockets_initialised();

    const remoting::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == remoting::kInvalidSocket) {
#if defined(_WIN32)
        fprintf(stderr, "server: socket failed: WSA error %d\n", WSAGetLastError());
#else
        fprintf(stderr, "server: socket failed: %s\n", strerror(errno));
#endif
        return remoting::kInvalidSocket;
    }

    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        fprintf(stderr, "server: bad address '%s'\n", address.c_str());
        remoting::close_socket(fd);
        return remoting::kInvalidSocket;
    }

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
#if defined(_WIN32)
        fprintf(stderr, "server: bind failed: WSA error %d\n", WSAGetLastError());
#else
        fprintf(stderr, "server: bind failed: %s\n", strerror(errno));
#endif
        remoting::close_socket(fd);
        return remoting::kInvalidSocket;
    }
    if (::listen(fd, 4) < 0) {
#if defined(_WIN32)
        fprintf(stderr, "server: listen failed: WSA error %d\n", WSAGetLastError());
#else
        fprintf(stderr, "server: listen failed: %s\n", strerror(errno));
#endif
        remoting::close_socket(fd);
        return remoting::kInvalidSocket;
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    // Loopback by default, deliberately. This protocol has no authentication
    // - the handshake only compares a command-set digest, which proves the
    // peer was built from the same generated table and nothing about who it
    // is - and every accepted connection gets a detached thread that can
    // reach the GPU. Binding 0.0.0.0 therefore offered that to the whole LAN.
    // The intended remote setup is an SSH tunnel (see the README): ssh
    // authenticates, encrypts and integrity-checks, and this port never
    // appears on the network at all. --address is still there for a
    // deliberate LAN-only run, which is now a choice rather than the default.
    std::string address = "127.0.0.1";
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
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif

    Server server;
    if (!server.init_vulkan(validate, want_wayland)) return 1;

#if defined(_WIN32)
    // The embedded Wayland proxy (see wayland/proxy_server.hpp) only ever
    // makes sense on the machine that owns a real compositor connection,
    // which a Windows server is not; CMakeLists.txt does not even build that
    // code into this target on Windows (see its `if(NOT WIN32)` block), so
    // there is nothing here for --wayland to start.
    if (want_wayland) {
        fprintf(stderr, "server: --wayland is not available in a Windows server build\n");
        return 1;
    }
#else
    // Without --wayland the server behaves exactly as it always has: no
    // VK_KHR_surface/VK_KHR_wayland_surface were even requested above, so the
    // offscreen path is untouched.
    if (want_wayland && !server.start_wayland(wayland_port)) return 1;
#endif

    const remoting::socket_t listen_fd = listen_on(address, port);
    if (listen_fd == remoting::kInvalidSocket) return 1;

    fprintf(stderr, "server: listening on %s:%u (command set %s)\n", address.c_str(),
            static_cast<unsigned>(port), remoting::kCommandSetDigest);

    while (!g_stop.load()) {
        const remoting::socket_t fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd == remoting::kInvalidSocket) {
            if (accept_was_interrupted()) continue;
            break;
        }
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                     sizeof(one));
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
            remoting::close_socket(fd);
        }).detach();
    }

#if !defined(_WIN32)
    server.stop_wayland();
#endif
    remoting::close_socket(listen_fd);
    fprintf(stderr, "server: stopped\n");
    return 0;
}
