// Runs on machine A (no GPU). Presents a normal-looking Wayland socket to a
// local app and relays its protocol traffic to the WaylandProxy embedded in
// the server process on machine B, which replays it onto the real
// compositor. No graphics buffers ever cross this link — see the fd handling
// below for exactly what is and is not supported.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "wire.hpp"

namespace {

int make_listen_socket(const std::string& path) {
    ::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "proxy_client: socket: %s\n", strerror(errno));
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        fprintf(stderr, "proxy_client: socket path too long\n");
        ::close(fd);
        return -1;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(fd, 1) < 0) {
        fprintf(stderr, "proxy_client: bind/listen %s: %s\n", path.c_str(), strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

int connect_tcp(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        fprintf(stderr, "proxy_client: could not resolve %s\n", host.c_str());
        return -1;
    }
    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "proxy_client: could not connect to %s:%u: %s\n", host.c_str(), port,
                strerror(errno));
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Creates a sealed-read-only memfd holding `content`, the way the real
// compositor hands out a keymap: a plain mmapable file the app can map
// PROT_READ and never worry about someone else mutating underneath it.
int make_sealed_memfd(const std::vector<uint8_t>& content) {
    const int fd = ::memfd_create("wayland-remote-fd", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (fd < 0) return -1;
    size_t written = 0;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n <= 0) {
            ::close(fd);
            return -1;
        }
        written += static_cast<size_t>(n);
    }
    ::fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
    return fd;
}

// Wraps the app-facing unix socket: pumps app -> link and link -> app,
// applying the fd rules described in the module comment.
class ProxyClient {
    struct PendingAppMessage {
        std::vector<uint8_t> bytes;
        std::vector<int> fds;
        size_t offset = 0;
        bool fds_sent = false;
    };

   public:
    ~ProxyClient() {
        for (PendingAppMessage& message : pending_app_) {
            for (int fd : message.fds) ::close(fd);
        }
    }

    bool run(int app_fd, int link_fd) {
        app_fd_ = app_fd;
        link_fd_ = link_fd;
        set_nonblocking(app_fd_);
        set_nonblocking(link_fd_);

        for (;;) {
            pollfd pfds[2] = {{app_fd_, POLLIN, 0}, {link_fd_, POLLIN, 0}};
            if (!tx_link_.empty()) pfds[1].events |= POLLOUT;
            if (!pending_app_.empty()) pfds[0].events |= POLLOUT;
            const int rv = ::poll(pfds, 2, 1000);
            if (rv < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (pfds[0].revents & (POLLIN | POLLOUT)) {
                if (!pump_app(pfds[0].revents)) return false;
            }
            if (pfds[1].revents & (POLLIN | POLLOUT)) {
                if (!pump_link(pfds[1].revents)) return false;
            }
            if ((pfds[0].revents & (POLLHUP | POLLERR)) || (pfds[1].revents & (POLLHUP | POLLERR))) {
                return false;
            }
        }
    }

   private:
    // app -> link. Refuses (drops) the moment the app tries to pass a file
    // descriptor: this design never needs client-side buffers (wl_shm) or a
    // clipboard/dnd path (wl_data_offer/wl_data_source), so there is nothing
    // legitimate left for an fd to mean here.
    bool pump_app(short revents) {
        if (revents & POLLIN) {
            uint8_t buf[8192];
            char cmsgbuf[CMSG_SPACE(sizeof(int) * 16)];
            iovec iov{buf, sizeof(buf)};
            msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = cmsgbuf;
            msg.msg_controllen = sizeof(cmsgbuf);
            const ssize_t n = ::recvmsg(app_fd_, &msg, 0);
            if (n == 0) return false;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    // fall through to flush
                } else {
                    return false;
                }
            } else {
                for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                    const int count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                    const int* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
                    for (int i = 0; i < count; ++i) ::close(fds[i]);
                    fprintf(stderr,
                            "proxy_client: refusing: app passed a file descriptor (wl_shm buffers and "
                            "the clipboard/dnd path are not forwarded by this design)\n");
                    return false;
                }
                rx_app_.insert(rx_app_.end(), buf, buf + n);
            }
        }
        if (!extract_and_forward_requests()) return false;
        return flush_tx_link();
    }

    bool extract_and_forward_requests() {
        for (;;) {
            std::vector<uint8_t> whole;
            const wire::FrameResult r = wire::take_wire_message(&rx_app_, &whole);
            if (r == wire::FrameResult::NeedMoreData) return true;
            if (r == wire::FrameResult::Malformed) {
                fprintf(stderr, "proxy_client: malformed request from app\n");
                return false;
            }

            uint32_t object_id = 0, header2 = 0;
            memcpy(&object_id, whole.data(), 4);
            memcpy(&header2, whole.data() + 4, 4);
            const uint16_t opcode = static_cast<uint16_t>(header2 & 0xffff);

            const wire::ObjectInfo* obj = objects_.find(object_id);
            if (!obj) {
                fprintf(stderr, "proxy_client: request on unknown object %u\n", object_id);
                return false;
            }
            const wire::InterfaceSpec* iface = wire::find_interface(obj->interface);
            const wire::MessageSpec* spec = wire::find_message(iface, /*is_event=*/false, opcode);
            if (!spec) {
                fprintf(stderr, "proxy_client: unknown request %s#%u\n", obj->interface.c_str(), opcode);
                return false;
            }
            if (wire::count_fd_args(spec) > 0) {
                fprintf(stderr,
                        "proxy_client: refusing %s.%s: fd-carrying requests are never forwarded\n",
                        obj->interface.c_str(), spec->name);
                return false;
            }

            wire::DecodedMessage decoded;
            if (!wire::decode_message(spec, whole.data() + 8, whole.size() - 8, &decoded)) {
                fprintf(stderr, "proxy_client: malformed %s.%s\n", obj->interface.c_str(), spec->name);
                return false;
            }
            wire::track_object_lifetime(&objects_, obj->interface, /*is_event=*/false, spec, decoded);

            wire::LinkFrame frame;
            frame.wire_bytes = std::move(whole);
            wire::write_link_frame(frame, &tx_link_);
        }
    }

    // link -> app. The only fd case that matters here is a genuine one:
    // wl_keyboard.keymap (and, generically, any other event with an Fd arg)
    // arrives as reconstructed content, turned into a sealed memfd and
    // handed to the app exactly like the real compositor would.
    bool pump_link(short revents) {
        if (revents & POLLIN) {
            uint8_t buf[16384];
            const ssize_t n = ::recv(link_fd_, buf, sizeof(buf), 0);
            if (n == 0) return false;
            if (n < 0) {
                if (!(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return false;
            } else {
                rx_link_.insert(rx_link_.end(), buf, buf + n);
            }
        }
        if (!extract_and_forward_events()) return false;
        return flush_tx_app();
    }

    bool extract_and_forward_events() {
        for (;;) {
            wire::LinkFrame frame;
            const wire::FrameResult r = wire::take_link_frame(&rx_link_, &frame);
            if (r == wire::FrameResult::NeedMoreData) return true;
            if (r == wire::FrameResult::Malformed) {
                fprintf(stderr, "proxy_client: malformed frame from server\n");
                return false;
            }

            if (frame.wire_bytes.size() < 8) {
                fprintf(stderr, "proxy_client: truncated event from server\n");
                return false;
            }
            uint32_t object_id = 0, header2 = 0;
            memcpy(&object_id, frame.wire_bytes.data(), 4);
            memcpy(&header2, frame.wire_bytes.data() + 4, 4);
            const uint16_t opcode = static_cast<uint16_t>(header2 & 0xffff);

            const wire::ObjectInfo* obj = objects_.find(object_id);
            if (!obj) {
                fprintf(stderr, "proxy_client: event on unknown object %u\n", object_id);
                return false;
            }
            const wire::InterfaceSpec* iface = wire::find_interface(obj->interface);
            const wire::MessageSpec* spec = wire::find_message(iface, /*is_event=*/true, opcode);
            if (!spec) {
                fprintf(stderr, "proxy_client: unknown event %s#%u\n", obj->interface.c_str(), opcode);
                return false;
            }
            if (wire::count_fd_args(spec) != frame.fd_blobs.size()) {
                fprintf(stderr, "proxy_client: fd count mismatch on %s.%s\n", obj->interface.c_str(),
                        spec->name);
                return false;
            }

            wire::DecodedMessage decoded;
            if (!wire::decode_message(spec, frame.wire_bytes.data() + 8, frame.wire_bytes.size() - 8,
                                       &decoded)) {
                fprintf(stderr, "proxy_client: malformed event %s.%s\n", obj->interface.c_str(), spec->name);
                return false;
            }
            wire::track_object_lifetime(&objects_, obj->interface, /*is_event=*/true, spec, decoded);

            std::vector<int> fds;
            bool ok = true;
            for (const auto& blob : frame.fd_blobs) {
                const int fd = make_sealed_memfd(blob);
                if (fd < 0) ok = false;
                fds.push_back(fd);
            }
            if (!ok) {
                for (int fd : fds)
                    if (fd >= 0) ::close(fd);
                fprintf(stderr, "proxy_client: could not materialise fd for %s.%s\n",
                        obj->interface.c_str(), spec->name);
                return false;
            }

            queue_app_message(frame.wire_bytes, std::move(fds));
        }
    }

    // Queues one message for delivery to the app. Ownership of the reconstructed
    // fds stays here until the first bytes carrying them have actually been sent.
    void queue_app_message(const std::vector<uint8_t>& wire_bytes, std::vector<int> fds) {
        pending_app_.push_back({wire_bytes, std::move(fds), 0, false});
    }

    bool flush_tx_app() {
        while (!pending_app_.empty()) {
            PendingAppMessage& pending = pending_app_.front();
            iovec iov{pending.bytes.data() + pending.offset,
                      pending.bytes.size() - pending.offset};
            msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            char cmsgbuf[CMSG_SPACE(sizeof(int) * 16)];
            if (!pending.fds_sent && !pending.fds.empty()) {
                msg.msg_control = cmsgbuf;
                msg.msg_controllen = CMSG_SPACE(sizeof(int) * pending.fds.size());
                cmsghdr* c = CMSG_FIRSTHDR(&msg);
                c->cmsg_level = SOL_SOCKET;
                c->cmsg_type = SCM_RIGHTS;
                c->cmsg_len = CMSG_LEN(sizeof(int) * pending.fds.size());
                memcpy(CMSG_DATA(c), pending.fds.data(), sizeof(int) * pending.fds.size());
            }
            const ssize_t n = ::sendmsg(app_fd_, &msg, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;

            pending.offset += static_cast<size_t>(n);
            if (!pending.fds_sent) {
                pending.fds_sent = true;
                for (int fd : pending.fds) ::close(fd);
                pending.fds.clear();
            }
            if (pending.offset == pending.bytes.size()) pending_app_.pop_front();
        }
        return true;
    }

    bool flush_tx_link() {
        while (!tx_link_.empty()) {
            const ssize_t n = ::send(link_fd_, tx_link_.data(), tx_link_.size(), MSG_NOSIGNAL);
            if (n > 0) {
                tx_link_.erase(tx_link_.begin(), tx_link_.begin() + n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    int app_fd_ = -1;
    int link_fd_ = -1;
    wire::ObjectTable objects_;
    std::vector<uint8_t> rx_app_, rx_link_, tx_link_;
    std::deque<PendingAppMessage> pending_app_;
};

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path;
    if (const char* runtime_dir = getenv("XDG_RUNTIME_DIR")) {
        socket_path = std::string(runtime_dir) + "/wayland-remote";
    }
    std::string host = "127.0.0.1";
    uint16_t port = 24681;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (arg == "--help") {
            printf("usage: %s [--socket PATH] [--host HOST] [--port PORT]\n", argv[0]);
            return 0;
        }
    }
    if (socket_path.empty()) {
        fprintf(stderr, "proxy_client: --socket not given and XDG_RUNTIME_DIR is unset\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    const int listen_fd = make_listen_socket(socket_path);
    if (listen_fd < 0) return 1;
    fprintf(stderr, "proxy_client: listening on %s, will relay to %s:%u\n", socket_path.c_str(),
            host.c_str(), static_cast<unsigned>(port));

    const int app_fd = ::accept(listen_fd, nullptr, nullptr);
    if (app_fd < 0) {
        fprintf(stderr, "proxy_client: accept: %s\n", strerror(errno));
        return 1;
    }
    ::close(listen_fd);
    fprintf(stderr, "proxy_client: app connected\n");

    const int link_fd = connect_tcp(host, port);
    if (link_fd < 0) return 1;
    fprintf(stderr, "proxy_client: linked to server\n");

    ProxyClient client;
    const bool ok = client.run(app_fd, link_fd);
    fprintf(stderr, "proxy_client: %s\n", ok ? "stopped" : "link died");
    ::close(app_fd);
    ::close(link_fd);
    return ok ? 0 : 1;
}
