#include "wire.hpp"

// windows.h's default (non-lean) mode drags in the legacy winsock.h, which
// conflicts with wire.hpp's winsock2.h if windows.h is reached first in this
// translation unit; defining this before any include - including wire.hpp
// itself - keeps that from happening regardless of what pulls windows.h in.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#endif

#if defined(_WIN32)
#include <mutex>
#else
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <string.h>

namespace remoting {
namespace {

#if defined(_WIN32)
// MSVC's send()/recv() return a plain int; ssize_t, which the POSIX branch
// below uses, does not exist there.
using io_result_t = int;
#else
using io_result_t = ssize_t;
#endif

// True for whatever this platform's send/recv report as "the call was
// interrupted, try again" - a signal on POSIX, WSAEINTR on Windows. Kept as
// one helper so the read/write loops never touch errno/WSAGetLastError
// directly.
bool interrupted() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

// True when the last failed send/recv reports the "would have blocked"
// family of errors, which is what a receive hitting SO_RCVTIMEO (see
// connect_to) looks like on either platform.
bool would_block_or_timed_out() {
#if defined(_WIN32)
    const int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

const char* last_socket_error_text() {
#if defined(_WIN32)
    // strerror(errno) does not describe Winsock failures, which are reported
    // through WSAGetLastError() instead of errno; a numeric code is enough
    // for the diagnostic messages below, which only ever go to stderr.
    static thread_local char buf[64];
    snprintf(buf, sizeof(buf), "WSA error %d", WSAGetLastError());
    return buf;
#else
    return strerror(errno);
#endif
}

bool write_all(socket_t fd, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
#if defined(_WIN32)
        // Windows has no SIGPIPE and no MSG_NOSIGNAL flag to guard against
        // one; a peer that disappeared is already reported as an ordinary
        // send() failure there.
        const io_result_t n = ::send(fd, p, static_cast<int>(size), 0);
#else
        // MSG_NOSIGNAL: a peer that disappeared must become an error here,
        // not a SIGPIPE that kills the process being traced.
        const io_result_t n = ::send(fd, p, size, MSG_NOSIGNAL);
#endif
        if (n > 0) {
            p += static_cast<size_t>(n);
            size -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && interrupted()) continue;
        return false;
    }
    return true;
}

bool read_all(socket_t fd, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size > 0) {
#if defined(_WIN32)
        const io_result_t n = ::recv(fd, p, static_cast<int>(size), 0);
#else
        const io_result_t n = ::recv(fd, p, size, 0);
#endif
        if (n > 0) {
            p += static_cast<size_t>(n);
            size -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && interrupted()) continue;
        // A client with SO_RCVTIMEO (see connect_to) sees a would-block/timeout
        // error here if the peer went quiet mid-message: worth telling apart
        // from an orderly shutdown (n == 0), because one means "wedged or
        // dead" and the other means "closed the connection on purpose".
        if (n < 0 && would_block_or_timed_out()) {
            fprintf(stderr, "remoting: recv timed out waiting for the peer; treating it as dead\n");
        } else if (n == 0) {
            fprintf(stderr, "remoting: peer closed the connection\n");
        } else {
            fprintf(stderr, "remoting: recv failed: %s\n", last_socket_error_text());
        }
        return false;
    }
    return true;
}

}  // namespace

bool send_message(socket_t fd, uint32_t opcode, const std::vector<char>& payload) {
    MessageHeader header{opcode, static_cast<uint32_t>(payload.size())};
    if (!write_all(fd, &header, sizeof(header))) return false;
    if (payload.empty()) return true;
    return write_all(fd, payload.data(), payload.size());
}

bool recv_message(socket_t fd, MessageHeader* header, std::vector<char>* payload) {
    if (!read_all(fd, header, sizeof(*header))) return false;

    // A hostile or desynchronised peer could otherwise ask for a huge alloc.
    constexpr uint32_t kMaxPayload = 64u * 1024u * 1024u;
    if (header->payload_size > kMaxPayload) return false;

    payload->resize(header->payload_size);
    if (header->payload_size == 0) return true;
    return read_all(fd, payload->data(), payload->size());
}

void close_socket(socket_t fd) {
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

void ensure_sockets_initialised() {
#if defined(_WIN32)
    // Winsock must be started once per process before any socket call, and
    // nothing else in this driver has a natural "process startup" hook to do
    // it from - the ICD is loaded into whatever process called into Vulkan,
    // and the server (see main.cpp) opens its listening socket before ever
    // calling connect_to(). A std::once_flag makes whichever call gets here
    // first double as that hook without every caller needing to know that.
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
    // No matching WSACleanup(): this driver is loaded for the lifetime of the
    // host process and has no reliable point to call it from at which no
    // other connection could still be using Winsock.
#endif
}

socket_t connect_to(const std::string& host, uint16_t port) {
    ensure_sockets_initialised();

    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%u", static_cast<unsigned>(port));

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* results = nullptr;
    const int err = ::getaddrinfo(host.c_str(), port_text, &hints, &results);
    if (err != 0) {
        fprintf(stderr, "remoting: cannot resolve %s: %s\n", host.c_str(), gai_strerror(err));
        return kInvalidSocket;
    }

    socket_t fd = kInvalidSocket;
    for (struct addrinfo* it = results; it != nullptr; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kInvalidSocket) continue;
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        close_socket(fd);
        fd = kInvalidSocket;
    }
    ::freeaddrinfo(results);

    if (fd == kInvalidSocket) {
        fprintf(stderr, "remoting: cannot connect to %s:%u: %s\n", host.c_str(),
                static_cast<unsigned>(port), last_socket_error_text());
        return kInvalidSocket;
    }

    // Without this, Nagle holds a small request back waiting to coalesce with a
    // follow-up that cannot arrive, because the caller is blocked on the reply.
    const int one = 1;
    // setsockopt's optval is a const void* on POSIX but a const char* on
    // Windows regardless of the option's real type; the cast is a no-op on
    // POSIX and required on Windows, so it is applied unconditionally.
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

    // Only the client connects out (the server accepts), so this is the client's
    // socket. Without a receive timeout, a dead or wedged server leaves the
    // client blocked in recv() forever - every Vulkan call becomes an
    // unkillable hang instead of a reported error. 30s is generous enough that
    // a legitimately slow call over a real network still completes.
#if defined(_WIN32)
    // SO_RCVTIMEO is the same option number on Windows, but it does NOT take a
    // struct timeval there: it takes a DWORD of milliseconds. Passing a
    // timeval on Windows does not fail to compile, it just misreads the
    // wrong bytes as the timeout, so the two branches must be kept at the
    // same 30-second value by hand rather than sharing one struct.
    const DWORD timeout_ms = 30 * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                 sizeof(timeout_ms));
#else
    struct timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    return fd;
}

}  // namespace remoting
