#pragma once

// A Wayland window the server creates and owns itself, rather than one
// replayed from an application (see wayland/proxy_server.hpp for that other
// path). This is what backs vkCreateWin32SurfaceKHR: a Win32 client has no
// Wayland connection at all, so there is nothing to adopt, and the server
// has to make its own surface to present through.
//
// Deliberately opens its own wl_display connection instead of reusing
// WaylandProxy's, so this works even when the server was not started with
// --wayland.

#include <cstdint>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_array;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

class OwnWindow {
   public:
    OwnWindow() = default;
    ~OwnWindow();

    OwnWindow(const OwnWindow&) = delete;
    OwnWindow& operator=(const OwnWindow&) = delete;

    // Connects to the compositor, creates a wl_surface -> xdg_surface ->
    // xdg_toplevel chain, and round-trips until the compositor has
    // configured it. Returns false - cleanly, never crashing or hanging -
    // if there is no compositor to talk to, or it never advertises
    // wl_compositor/xdg_wm_base. Safe to call on more than one OwnWindow
    // instance: each one gets its own connection, so a fresh instance per
    // surface request never fights an earlier one for state.
    bool create();

    wl_display* display() const { return display_; }
    wl_surface* surface() const { return surface_; }

    static void registry_global(void* data, wl_registry* registry, uint32_t name,
                                 const char* interface, uint32_t version);
    static void registry_global_remove(void* data, wl_registry* registry, uint32_t name);
    static void wm_base_ping(void* data, xdg_wm_base* wm_base, uint32_t serial);
    static void surface_configure(void* data, xdg_surface* xdg_surface_obj, uint32_t serial);
    static void toplevel_configure(void* data, xdg_toplevel* toplevel, int32_t width,
                                   int32_t height, wl_array* states);
    static void toplevel_close(void* data, xdg_toplevel* toplevel);

   private:
    wl_display* display_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    xdg_wm_base* wm_base_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    bool configured_ = false;
};
