#include "own_window.hpp"

#include <cstdio>
#include <cstring>

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

namespace {

const wl_registry_listener kRegistryListener = {OwnWindow::registry_global,
                                                 OwnWindow::registry_global_remove};
const xdg_wm_base_listener kWmBaseListener = {OwnWindow::wm_base_ping};
const xdg_surface_listener kSurfaceListener = {OwnWindow::surface_configure};
const xdg_toplevel_listener kToplevelListener = {OwnWindow::toplevel_configure,
                                                  OwnWindow::toplevel_close};

}  // namespace

void OwnWindow::registry_global(void* data, wl_registry* registry, uint32_t name,
                                 const char* interface, uint32_t version) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        self->compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        self->wm_base_ =
            static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    }
}

void OwnWindow::registry_global_remove(void*, wl_registry*, uint32_t) {}

void OwnWindow::wm_base_ping(void*, xdg_wm_base* wm_base, uint32_t serial) {
    // A compositor that never gets its ping answered kills the window, so
    // this has to be wired up before the round-trips below give it a chance
    // to arrive.
    xdg_wm_base_pong(wm_base, serial);
}

void OwnWindow::surface_configure(void* data, xdg_surface* xdg_surface_obj, uint32_t serial) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    xdg_surface_ack_configure(xdg_surface_obj, serial);
    self->configured_ = true;
}

void OwnWindow::toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                                   wl_array*) {
    // A zero width or height means "you choose" rather than a real size, and
    // the compositor sends exactly that for the initial configure, so it must
    // not be mistaken for a resize.
    if (width <= 0 || height <= 0) return;
    OwnWindow* self = static_cast<OwnWindow*>(data);
    const uint32_t w = static_cast<uint32_t>(width);
    const uint32_t h = static_cast<uint32_t>(height);
    if (self->width_.exchange(w) != w || self->height_.exchange(h) != h) {
        self->resized_.store(true);
    }
}
void OwnWindow::toplevel_close(void*, xdg_toplevel*) {}

void OwnWindow::pump() {
    if (!display_) return;
    // dispatch_pending only handles what has already been read off the
    // socket, so it cannot block; the driver's own WSI reading is what puts
    // events there. The flush pushes out the acks those handlers queued.
    wl_display_dispatch_pending(display_);
    wl_display_flush(display_);
}

OwnWindow::~OwnWindow() {
    if (toplevel_) xdg_toplevel_destroy(toplevel_);
    if (xdg_surface_) xdg_surface_destroy(xdg_surface_);
    if (surface_) wl_surface_destroy(surface_);
    if (wm_base_) xdg_wm_base_destroy(wm_base_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (display_) wl_display_disconnect(display_);
}

bool OwnWindow::create() {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        fprintf(stderr, "server: own_window: could not connect to a compositor (check "
                        "WAYLAND_DISPLAY)\n");
        return false;
    }

    wl_registry* registry = wl_display_get_registry(display_);
    wl_registry_add_listener(registry, &kRegistryListener, this);
    wl_display_roundtrip(display_);

    if (!compositor_ || !wm_base_) {
        fprintf(stderr, "server: own_window: compositor did not advertise wl_compositor/"
                        "xdg_wm_base\n");
        return false;
    }
    xdg_wm_base_add_listener(wm_base_, &kWmBaseListener, this);

    surface_ = wl_compositor_create_surface(compositor_);
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
    xdg_surface_add_listener(xdg_surface_, &kSurfaceListener, this);
    toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    xdg_toplevel_add_listener(toplevel_, &kToplevelListener, this);
    xdg_toplevel_set_app_id(toplevel_, "vulkan_remoting");
    xdg_toplevel_set_title(toplevel_, "vulkan_remoting");

    // The first commit carries no buffer; it only exists to trigger the
    // initial xdg_surface.configure, per the xdg-shell handshake. A second
    // commit after acking that configure is what actually maps the toplevel.
    wl_surface_commit(surface_);
    wl_display_roundtrip(display_);
    wl_surface_commit(surface_);
    wl_display_roundtrip(display_);

    if (!configured_) {
        fprintf(stderr, "server: own_window: compositor never configured the surface\n");
        return false;
    }
    return true;
}
