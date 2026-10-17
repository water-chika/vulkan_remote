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
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <exception>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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

enum class ListenWaitResult { Ready, Timeout, Error };

ListenWaitResult wait_for_connection(remoting::socket_t listen_fd) {
    // Periodically return to the main loop so completed workers are joined and
    // their sockets are closed even when no new client arrives. This also puts
    // a bound on how long Windows shutdown waits for accept readiness; unlike
    // POSIX, a console signal does not interrupt Winsock select().
#if defined(_WIN32)
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(listen_fd, &read_fds);
    struct timeval timeout {};
    timeout.tv_usec = 100 * 1000;
    const int result = ::select(0, &read_fds, nullptr, nullptr, &timeout);
#else
    struct pollfd poll_fd {};
    poll_fd.fd = listen_fd;
    poll_fd.events = POLLIN;
    const int result = ::poll(&poll_fd, 1, 100);
#endif
    if (result > 0) return ListenWaitResult::Ready;
    if (result == 0 || accept_was_interrupted()) return ListenWaitResult::Timeout;
    return ListenWaitResult::Error;
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
#if !defined(_WIN32)
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "server: making listener nonblocking failed: %s\n", strerror(errno));
        remoting::close_socket(fd);
        return remoting::kInvalidSocket;
    }
#endif
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

#if defined(_WIN32)
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#else
    // Deliberately sigaction() with sa_flags = 0 rather than signal().
    // glibc's signal() is the BSD flavour, which sets SA_RESTART, and a
    // restarting accept() never comes back to the top of the loop to notice
    // that on_signal() set g_stop - so Ctrl+C set the flag and the server
    // carried on waiting for a connection that was never going to arrive.
    // Without SA_RESTART the accept() fails with EINTR, and the loop below
    // re-tests g_stop on the way round.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
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

    struct ClientThread {
        remoting::socket_t fd;
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    constexpr size_t kMaxConcurrentClients = 64;
    std::vector<ClientThread> clients;
    clients.reserve(kMaxConcurrentClients);
    bool listener_failed = false;

    const auto reap_completed = [&clients] {
        for (auto it = clients.begin(); it != clients.end();) {
            if (!it->done->load(std::memory_order_acquire)) {
                ++it;
                continue;
            }
            it->thread.join();
            remoting::close_socket(it->fd);
            it = clients.erase(it);
        }
    };

    while (!g_stop.load()) {
        reap_completed();
        const ListenWaitResult wait_result = wait_for_connection(listen_fd);
        if (wait_result == ListenWaitResult::Timeout) continue;
        if (wait_result == ListenWaitResult::Error) {
#if defined(_WIN32)
            fprintf(stderr, "server: listener wait failed: WSA error %d\n", WSAGetLastError());
#else
            fprintf(stderr, "server: listener wait failed: %s\n", strerror(errno));
#endif
            listener_failed = true;
            break;
        }

        const remoting::socket_t fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd == remoting::kInvalidSocket) {
            if (accept_was_interrupted()) continue;
#if !defined(_WIN32)
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
#if defined(_WIN32)
            fprintf(stderr, "server: accept failed: WSA error %d\n", WSAGetLastError());
#else
            fprintf(stderr, "server: accept failed: %s\n", strerror(errno));
#endif
            listener_failed = true;
            break;
        }
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                     sizeof(one));
        if (clients.size() >= kMaxConcurrentClients) {
            fprintf(stderr, "server: rejecting client: concurrent session limit reached\n");
            remoting::close_socket(fd);
            continue;
        }
        // One VkInstance is one connection, and a single process can hold
        // several open at once (CTS's CustomInstanceTest keeps its default
        // instance alive while asking for a differently-configured one for a
        // single test). Keep the threads joinable: shutdown must not destroy
        // Server while a handler is still using its Vulkan instance or maps.
        // The main thread owns fd throughout. The worker publishes completion
        // only after its last use; main then joins it before closing fd. This
        // prevents shutdown from ever targeting a descriptor value the OS has
        // already recycled for an unrelated socket.
        auto done = std::make_shared<std::atomic<bool>>(false);
        try {
            std::thread worker([&server, fd, done] {
                try {
                    server.serve(fd);
                } catch (const std::exception& error) {
                    fprintf(stderr, "server: client worker exception: %s\n", error.what());
                } catch (...) {
                    fprintf(stderr, "server: client worker exception: unknown error\n");
                }
                done->store(true, std::memory_order_release);
            });
            clients.push_back({fd, std::move(worker), done});
        } catch (const std::system_error& error) {
            fprintf(stderr, "server: starting client worker failed: %s\n", error.what());
            remoting::close_socket(fd);
            listener_failed = true;
            break;
        }
    }

    remoting::close_socket(listen_fd);
    for (ClientThread& client : clients) {
#if defined(_WIN32)
        ::shutdown(client.fd, SD_BOTH);
#else
        ::shutdown(client.fd, SHUT_RDWR);
#endif
    }
    for (ClientThread& client : clients) {
        client.thread.join();
        remoting::close_socket(client.fd);
    }
#if !defined(_WIN32)
    server.stop_wayland();
#endif
    fprintf(stderr, "server: stopped\n");
    return listener_failed ? 1 : 0;
}
