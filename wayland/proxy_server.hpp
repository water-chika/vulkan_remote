#pragma once

// Embeddable Wayland protocol proxy for the machine that owns the GPU.
//
// It maintains a real libwayland-client connection to the compositor and
// replays whatever the app on the other end of the TCP link does onto it,
// object for object. That replay is what makes surface_for_client_id useful:
// the wl_surface it returns is a real object on this connection, so whatever
// process embeds WaylandProxy can hand it straight to a Vulkan
// VK_KHR_wayland_surface call.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct wl_display;
struct wl_proxy;
struct wl_surface;
struct wl_interface;
struct wl_message;
union wl_argument;

namespace wire {
struct MessageSpec;
}

class WaylandProxy {
   public:
    WaylandProxy();
    ~WaylandProxy();

    WaylandProxy(const WaylandProxy&) = delete;
    WaylandProxy& operator=(const WaylandProxy&) = delete;

    // Connects to the real compositor (respecting WAYLAND_DISPLAY /
    // XDG_RUNTIME_DIR the way any Wayland client does) and starts listening
    // for one proxy_client on tcp_port.
    bool start(uint16_t tcp_port);

    // Non-blocking pump: drives both the real compositor connection and the
    // TCP link one step. Call this from the embedding process's own loop.
    // Returns false once the TCP link has died (the compositor connection
    // failing is fatal too and also yields false).
    bool poll();

    wl_display* display() const { return real_display_; }

    // Only meaningful once the app has actually created that surface; nullptr
    // before then or if client_object_id never named a wl_surface.
    wl_surface* surface_for_client_id(uint32_t client_object_id) const;

   private:
    struct ObjectEntry {
        std::string interface;
        uint32_t version = 0;
        wl_proxy* proxy = nullptr;  // null only for id 0 placeholders, never stored
    };

    bool accept_link();
    void pump_compositor();
    void pump_link();
    bool handle_request_frame(const std::vector<uint8_t>& wire_bytes,
                               const std::vector<std::vector<uint8_t>>& fd_blobs);
    const wl_interface* lookup_wl_interface(const std::string& name) const;
    void drop_link(const char* why);

    // The generic event dispatcher installed on every replayed proxy. `data`
    // carries the app's object id for that proxy (see start()/marshal code),
    // boxed as a pointer-sized integer — cheaper and simpler than a reverse
    // proxy->id map.
    static int generic_dispatcher(const void* implementation, void* target, uint32_t opcode,
                                   const wl_message* msg, wl_argument* args);

    wl_display* real_display_ = nullptr;
    int listen_fd_ = -1;
    int link_fd_ = -1;

    std::unordered_map<uint32_t, ObjectEntry> objects_;  // client_object_id -> local proxy
    uint32_t next_server_side_id_ = 0xff000000;           // for new_id args inside events

    std::vector<uint8_t> rx_buf_;
    std::vector<uint8_t> tx_buf_;
};
