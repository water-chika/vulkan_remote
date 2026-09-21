#include "proxy_server.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <wayland-client.h>

#include "wire.hpp"
#include "wl_protocol_interfaces.inl"

namespace {

// Interfaces libwayland-client itself exports a wl_interface symbol for.
// generate_protocol.py's extension table only covers what it isn't already
// linked (xdg-shell etc); this fills in the rest by name.
const struct wl_interface* lookup_core_interface(const std::string& name) {
    static const std::pair<const char*, const struct wl_interface*> kCore[] = {
        {"wl_display", &wl_display_interface},
        {"wl_registry", &wl_registry_interface},
        {"wl_callback", &wl_callback_interface},
        {"wl_compositor", &wl_compositor_interface},
        {"wl_shm_pool", &wl_shm_pool_interface},
        {"wl_shm", &wl_shm_interface},
        {"wl_buffer", &wl_buffer_interface},
        {"wl_data_offer", &wl_data_offer_interface},
        {"wl_data_source", &wl_data_source_interface},
        {"wl_data_device", &wl_data_device_interface},
        {"wl_data_device_manager", &wl_data_device_manager_interface},
        {"wl_shell", &wl_shell_interface},
        {"wl_shell_surface", &wl_shell_surface_interface},
        {"wl_surface", &wl_surface_interface},
        {"wl_seat", &wl_seat_interface},
        {"wl_pointer", &wl_pointer_interface},
        {"wl_keyboard", &wl_keyboard_interface},
        {"wl_touch", &wl_touch_interface},
        {"wl_output", &wl_output_interface},
        {"wl_region", &wl_region_interface},
        {"wl_subcompositor", &wl_subcompositor_interface},
        {"wl_subsurface", &wl_subsurface_interface},
    };
    for (const auto& entry : kCore) {
        if (name == entry.first) return entry.second;
    }
    return nullptr;
}

}  // namespace

WaylandProxy::WaylandProxy() = default;

WaylandProxy::~WaylandProxy() {
    if (link_fd_ >= 0) ::close(link_fd_);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (real_display_) wl_display_disconnect(real_display_);
}

const wl_interface* WaylandProxy::lookup_wl_interface(const std::string& name) const {
    if (const wl_interface* core = lookup_core_interface(name)) return core;
    for (size_t i = 0; i < kExtensionInterfaceCount; ++i) {
        if (name == kExtensionInterfaces[i].name) return kExtensionInterfaces[i].iface;
    }
    return nullptr;
}

bool WaylandProxy::start(uint16_t tcp_port) {
    real_display_ = wl_display_connect(nullptr);
    if (!real_display_) {
        fprintf(stderr, "wayland_proxy_server: could not connect to the real compositor "
                        "(check WAYLAND_DISPLAY/XDG_RUNTIME_DIR)\n");
        return false;
    }

    // id 1 always names the display itself, exactly like a fresh client
    // connection; a wl_display* doubles as its own wl_proxy in libwayland.
    wl_proxy* display_proxy = reinterpret_cast<wl_proxy*>(real_display_);
    wl_proxy_add_dispatcher(display_proxy, &WaylandProxy::generic_dispatcher, this,
                             reinterpret_cast<void*>(static_cast<uintptr_t>(1)));
    objects_[1] = {"wl_display", 1, display_proxy};

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        fprintf(stderr, "wayland_proxy_server: socket: %s\n", strerror(errno));
        return false;
    }
    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(tcp_port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(listen_fd_, 1) < 0) {
        fprintf(stderr, "wayland_proxy_server: bind/listen on %u: %s\n", tcp_port, strerror(errno));
        return false;
    }

    const int flags = fcntl(listen_fd_, F_GETFL, 0);
    fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

    fprintf(stderr, "wayland_proxy_server: listening on port %u, relaying to the real compositor\n",
            tcp_port);
    return true;
}

bool WaylandProxy::accept_link() {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) return false;
    if (link_fd_ >= 0) {
        // Only one proxy_client is meant to exist per the design; a second
        // connection attempt just gets refused rather than confusing the
        // object table of the first.
        fprintf(stderr, "wayland_proxy_server: refusing second link, one is already active\n");
        ::close(fd);
        return false;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    link_fd_ = fd;
    fprintf(stderr, "wayland_proxy_server: proxy_client connected\n");
    return true;
}

void WaylandProxy::drop_link(const char* why) {
    fprintf(stderr, "wayland_proxy_server: dropping link: %s\n", why);
    if (link_fd_ >= 0) {
        ::close(link_fd_);
        link_fd_ = -1;
    }
    rx_buf_.clear();
    tx_buf_.clear();
}

wl_surface* WaylandProxy::surface_for_client_id(uint32_t client_object_id) const {
    auto it = objects_.find(client_object_id);
    if (it == objects_.end() || it->second.interface != "wl_surface") return nullptr;
    return reinterpret_cast<wl_surface*>(it->second.proxy);
}

// --- compositor -> app -------------------------------------------------

int WaylandProxy::generic_dispatcher(const void* implementation, void* target, uint32_t opcode,
                                      const wl_message* /*msg*/, wl_argument* args) {
    WaylandProxy* self = const_cast<WaylandProxy*>(static_cast<const WaylandProxy*>(implementation));
    wl_proxy* proxy = static_cast<wl_proxy*>(target);
    const char* iface_name = wl_proxy_get_class(proxy);

    const wire::InterfaceSpec* iface = wire::find_interface(iface_name);
    const wire::MessageSpec* spec = wire::find_message(iface, /*is_event=*/true, opcode);
    if (!spec) {
        // An event we have no table entry for means the generator was run
        // against different protocol XML than the peer expects — refuse to
        // guess at its shape.
        fprintf(stderr, "wayland_proxy_server: unknown event %s#%u, dropping link\n", iface_name, opcode);
        self->drop_link("unrecognised event shape");
        return 0;
    }

    const uint32_t client_object_id =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wl_proxy_get_user_data(proxy)));

    std::vector<wire::DecodedArg> decoded;
    decoded.reserve(spec->arg_count);
    std::vector<std::vector<uint8_t>> fd_blobs;

    for (uint16_t i = 0; i < spec->arg_count; ++i) {
        const wire::ArgSpec& arg_spec = spec->args[i];
        wire::DecodedArg out;
        out.type = arg_spec.type;
        switch (arg_spec.type) {
            case wire::ArgType::Int:
            case wire::ArgType::Fixed:
                out.i32 = args[i].i;
                break;
            case wire::ArgType::Uint:
                out.u32 = args[i].u;
                break;
            case wire::ArgType::String:
                out.str = args[i].s ? args[i].s : "";
                break;
            case wire::ArgType::Array:
                if (args[i].a) {
                    const uint8_t* p = static_cast<const uint8_t*>(args[i].a->data);
                    out.array.assign(p, p + args[i].a->size);
                }
                break;
            case wire::ArgType::Object: {
                wl_proxy* ref = reinterpret_cast<wl_proxy*>(args[i].o);
                out.u32 = ref ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wl_proxy_get_user_data(ref)))
                               : 0;
                break;
            }
            case wire::ArgType::NewId: {
                // Events only ever create statically-typed objects (bind is
                // a request), so the interface is always known already.
                wl_proxy* new_proxy = reinterpret_cast<wl_proxy*>(args[i].o);
                const uint32_t new_client_id = self->next_server_side_id_++;
                out.str = arg_spec.interface ? arg_spec.interface : "";
                out.u32 = new_client_id;
                wl_proxy_add_dispatcher(new_proxy, &WaylandProxy::generic_dispatcher, self,
                                         reinterpret_cast<void*>(static_cast<uintptr_t>(new_client_id)));
                self->objects_[new_client_id] = {out.str, wl_proxy_get_version(new_proxy), new_proxy};
                break;
            }
            case wire::ArgType::Fd: {
                const int fd = args[i].h;
                std::vector<uint8_t> blob;
                struct stat st{};
                if (fd >= 0 && ::fstat(fd, &st) == 0 && st.st_size > 0) {
                    blob.resize(static_cast<size_t>(st.st_size));
                    size_t got = 0;
                    while (got < blob.size()) {
                        const ssize_t n = ::pread(fd, blob.data() + got, blob.size() - got,
                                                   static_cast<off_t>(got));
                        if (n <= 0) break;
                        got += static_cast<size_t>(n);
                    }
                    blob.resize(got);
                }
                if (fd >= 0) ::close(fd);
                out.fd = -1;
                fd_blobs.push_back(std::move(blob));
                break;
            }
        }
        decoded.push_back(std::move(out));
    }

    std::vector<uint8_t> wire_bytes;
    if (!wire::encode_message(client_object_id, static_cast<uint16_t>(opcode), decoded, &wire_bytes)) {
        self->drop_link("failed to re-encode event");
        return 0;
    }

    if (iface_name == std::string("wl_display") && std::string(spec->name) == "delete_id" &&
        !decoded.empty()) {
        self->objects_.erase(decoded[0].u32);
    }

    wire::LinkFrame frame;
    frame.wire_bytes = std::move(wire_bytes);
    frame.fd_blobs = std::move(fd_blobs);
    wire::write_link_frame(frame, &self->tx_buf_);
    return 0;
}

void WaylandProxy::pump_compositor() {
    while (wl_display_prepare_read(real_display_) != 0) {
        wl_display_dispatch_pending(real_display_);
    }
    wl_display_flush(real_display_);

    pollfd pfd{wl_display_get_fd(real_display_), POLLIN, 0};
    const int rv = ::poll(&pfd, 1, 0);
    if (rv > 0 && (pfd.revents & POLLIN)) {
        wl_display_read_events(real_display_);
    } else {
        wl_display_cancel_read(real_display_);
    }
    wl_display_dispatch_pending(real_display_);
}

// --- app -> compositor ---------------------------------------------------

bool WaylandProxy::handle_request_frame(const std::vector<uint8_t>& wire_bytes,
                                         const std::vector<std::vector<uint8_t>>& fd_blobs) {
    if (wire_bytes.size() < 8) return false;
    uint32_t object_id = 0, header2 = 0;
    memcpy(&object_id, wire_bytes.data(), 4);
    memcpy(&header2, wire_bytes.data() + 4, 4);
    const uint16_t opcode = static_cast<uint16_t>(header2 & 0xffff);
    const uint16_t size = static_cast<uint16_t>(header2 >> 16);
    if (size != wire_bytes.size()) return false;

    auto obj_it = objects_.find(object_id);
    if (obj_it == objects_.end()) {
        fprintf(stderr, "wayland_proxy_server: request on unknown object %u\n", object_id);
        return false;
    }
    const wire::InterfaceSpec* iface = wire::find_interface(obj_it->second.interface);
    const wire::MessageSpec* spec = wire::find_message(iface, /*is_event=*/false, opcode);
    if (!spec) {
        fprintf(stderr, "wayland_proxy_server: unknown request %s#%u\n",
                obj_it->second.interface.c_str(), opcode);
        return false;
    }

    wire::DecodedMessage decoded;
    if (!wire::decode_message(spec, wire_bytes.data() + 8, wire_bytes.size() - 8, &decoded)) {
        fprintf(stderr, "wayland_proxy_server: malformed request %s.%s\n",
                obj_it->second.interface.c_str(), spec->name);
        return false;
    }
    if (!fd_blobs.empty() || wire::count_fd_args(spec) > 0) {
        // proxy_client is expected to have refused these already; seeing one
        // here means the peer disagrees with us about the protocol shape.
        fprintf(stderr, "wayland_proxy_server: unexpected fd-carrying request %s.%s\n",
                obj_it->second.interface.c_str(), spec->name);
        return false;
    }

    // Build the wl_argument[] libwayland expects. A new_id arg still needs a
    // slot in this array (see the NewId case below) even though its content
    // is filled in by wl_proxy_marshal_array_flags itself.
    std::vector<wl_argument> args;
    std::vector<wl_array> arrays;  // kept alive until after the marshal call
    arrays.reserve(decoded.args.size());
    const wl_interface* new_obj_interface = nullptr;
    uint32_t new_obj_version = wl_proxy_get_version(obj_it->second.proxy);
    uint32_t app_new_id = 0;

    for (const wire::DecodedArg& arg : decoded.args) {
        wl_argument wa{};
        switch (arg.type) {
            case wire::ArgType::Int:
            case wire::ArgType::Fixed:
                wa.i = arg.i32;
                args.push_back(wa);
                break;
            case wire::ArgType::Uint:
                wa.u = arg.u32;
                args.push_back(wa);
                break;
            case wire::ArgType::String:
                wa.s = arg.str.c_str();
                args.push_back(wa);
                break;
            case wire::ArgType::Array: {
                arrays.push_back({arg.array.size(), arg.array.size(), const_cast<uint8_t*>(arg.array.data())});
                wa.a = &arrays.back();
                args.push_back(wa);
                break;
            }
            case wire::ArgType::Object: {
                auto ref_it = objects_.find(arg.u32);
                wa.o = (arg.u32 != 0 && ref_it != objects_.end())
                           ? reinterpret_cast<wl_object*>(ref_it->second.proxy)
                           : nullptr;
                args.push_back(wa);
                break;
            }
            case wire::ArgType::NewId: {
                app_new_id = arg.u32;
                const bool dynamic_bind = arg.new_id_version != 0;
                new_obj_interface = lookup_wl_interface(arg.str);
                if (!new_obj_interface) {
                    fprintf(stderr, "wayland_proxy_server: unknown target interface '%s' for %s.%s\n",
                            arg.str.c_str(), obj_it->second.interface.c_str(), spec->name);
                    return false;
                }
                new_obj_version = dynamic_bind ? arg.new_id_version : wl_proxy_get_version(obj_it->second.proxy);
                if (dynamic_bind) {
                    // wl_registry.bind's wire shape keeps the (name, string,
                    // version) fields explicit even though the new_id slot
                    // itself is implicit to libwayland's marshal call. The
                    // `name` uint was already appended above as its own arg;
                    // only the interface string and version remain to add.
                    wl_argument iface_arg{};
                    iface_arg.s = arg.str.c_str();
                    args.push_back(iface_arg);
                    wl_argument ver_arg{};
                    ver_arg.u = arg.new_id_version;
                    args.push_back(ver_arg);
                }
                // libwayland still expects a slot for the new_id itself in
                // args[] (create_outgoing_proxy writes the new wl_object*
                // into it); the *contents* are ignored on the way in.
                wl_argument new_id_slot{};
                new_id_slot.o = nullptr;
                args.push_back(new_id_slot);
                break;
            }
            case wire::ArgType::Fd:
                break;  // unreachable, checked above
        }
    }

    wl_proxy* result = wl_proxy_marshal_array_flags(
        obj_it->second.proxy, opcode, new_obj_interface, new_obj_version,
        spec->is_destructor ? WL_MARSHAL_FLAG_DESTROY : 0, args.data());

    if (new_obj_interface) {
        if (!result) {
            fprintf(stderr, "wayland_proxy_server: failed to create %s\n", obj_it->second.interface.c_str());
            return false;
        }
        wl_proxy_add_dispatcher(result, &WaylandProxy::generic_dispatcher, this,
                                 reinterpret_cast<void*>(static_cast<uintptr_t>(app_new_id)));
        objects_[app_new_id] = {new_obj_interface->name, new_obj_version, result};
    }
    if (spec->is_destructor) {
        objects_.erase(object_id);
    }
    return true;
}

void WaylandProxy::pump_link() {
    for (;;) {
        uint8_t buf[8192];
        const ssize_t n = ::recv(link_fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            rx_buf_.insert(rx_buf_.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            drop_link("proxy_client disconnected");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        drop_link(strerror(errno));
        return;
    }

    for (;;) {
        wire::LinkFrame frame;
        const wire::FrameResult r = wire::take_link_frame(&rx_buf_, &frame);
        if (r == wire::FrameResult::NeedMoreData) break;
        if (r == wire::FrameResult::Malformed) {
            drop_link("malformed link frame");
            return;
        }
        if (!handle_request_frame(frame.wire_bytes, frame.fd_blobs)) {
            drop_link("could not apply request");
            return;
        }
    }

    while (!tx_buf_.empty()) {
        const ssize_t n = ::send(link_fd_, tx_buf_.data(), tx_buf_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            tx_buf_.erase(tx_buf_.begin(), tx_buf_.begin() + n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        drop_link(n < 0 ? strerror(errno) : "short send");
        return;
    }
}

bool WaylandProxy::poll() {
    if (!real_display_) return false;
    pump_compositor();

    if (link_fd_ < 0) {
        accept_link();
    } else {
        pump_link();
    }
    return true;
}
