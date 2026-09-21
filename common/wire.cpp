#include "wire.hpp"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace remoting {
namespace {

bool write_all(int fd, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        // MSG_NOSIGNAL: a peer that disappeared must become an error here, not
        // a SIGPIPE that kills the process being traced.
        const ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
        if (n > 0) {
            p += n;
            size -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool read_all(int fd, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size > 0) {
        const ssize_t n = ::recv(fd, p, size, 0);
        if (n > 0) {
            p += n;
            size -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        // A client with SO_RCVTIMEO (see connect_to) sees EAGAIN/EWOULDBLOCK
        // here if the peer went quiet mid-message: worth telling apart from an
        // orderly shutdown (n == 0), because one means "wedged or dead" and
        // the other means "closed the connection on purpose".
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fprintf(stderr, "remoting: recv timed out waiting for the peer; treating it as dead\n");
        } else if (n == 0) {
            fprintf(stderr, "remoting: peer closed the connection\n");
        } else {
            fprintf(stderr, "remoting: recv failed: %s\n", strerror(errno));
        }
        return false;
    }
    return true;
}

}  // namespace

bool send_message(int fd, uint32_t opcode, const std::vector<char>& payload) {
    MessageHeader header{opcode, static_cast<uint32_t>(payload.size())};
    if (!write_all(fd, &header, sizeof(header))) return false;
    if (payload.empty()) return true;
    return write_all(fd, payload.data(), payload.size());
}

bool recv_message(int fd, MessageHeader* header, std::vector<char>* payload) {
    if (!read_all(fd, header, sizeof(*header))) return false;

    // A hostile or desynchronised peer could otherwise ask for a huge alloc.
    constexpr uint32_t kMaxPayload = 64u * 1024u * 1024u;
    if (header->payload_size > kMaxPayload) return false;

    payload->resize(header->payload_size);
    if (header->payload_size == 0) return true;
    return read_all(fd, payload->data(), payload->size());
}

int connect_to(const std::string& host, uint16_t port) {
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
        return -1;
    }

    int fd = -1;
    for (struct addrinfo* it = results; it != nullptr; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(results);

    if (fd < 0) {
        fprintf(stderr, "remoting: cannot connect to %s:%u: %s\n", host.c_str(),
                static_cast<unsigned>(port), strerror(errno));
        return -1;
    }

    // Without this, Nagle holds a small request back waiting to coalesce with a
    // follow-up that cannot arrive, because the caller is blocked on the reply.
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Only the client connects out (the server accepts), so this is the client's
    // socket. Without a receive timeout, a dead or wedged server leaves the
    // client blocked in recv() forever - every Vulkan call becomes an
    // unkillable hang instead of a reported error. 30s is generous enough that
    // a legitimately slow call over a real network still completes.
    struct timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}

}  // namespace remoting
