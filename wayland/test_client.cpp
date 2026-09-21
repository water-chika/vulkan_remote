// Minimal standard Wayland client used to exercise the proxy end to end
// (verification (a)/(b) in the task). Deliberately avoids wl_shm: this
// design never forwards buffers from the app, so proving a window appears
// is done by having the host-side test harness attach a buffer directly via
// WaylandProxy::surface_for_client_id, exactly the mechanism the Vulkan side
// will use later. This client only has to get a toplevel mapped and stay
// alive long enough for a screenshot.
//
// Uses the standard wayland-scanner-generated xdg-shell client header (a
// build-time convenience for this throwaway verification tool only — the
// proxy itself never depends on scanner output, see wayland/generate_protocol.py).

#include <stdio.h>
#include <unistd.h>

#include <string>

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

namespace {

struct State {
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                      uint32_t version) {
    State* state = static_cast<State*>(data);
    fprintf(stderr, "test_client: interface %s (v%u)", interface, version);
    if (std::string(interface) == wl_compositor_interface.name) {
        state->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4));
    } else if (std::string(interface) == xdg_wm_base_interface.name) {
        state->wm_base =
            static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    } else {
        fprintf(stderr, " (not bound)");
    }
    fprintf(stderr, "\n");
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = {registry_global, registry_global_remove};

void wm_base_ping(void*, xdg_wm_base* wm_base, uint32_t serial) { xdg_wm_base_pong(wm_base, serial); }
const xdg_wm_base_listener kWmBaseListener = {wm_base_ping};

void surface_configure(void*, xdg_surface* xsurface, uint32_t serial) {
    xdg_surface_ack_configure(xsurface, serial);
}
const xdg_surface_listener kSurfaceListener = {surface_configure};

void toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}
const xdg_toplevel_listener kToplevelListener = {toplevel_configure, toplevel_close};

}  // namespace

int main() {
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        fprintf(stderr, "test_client: could not connect (check WAYLAND_DISPLAY)\n");
        return 1;
    }

    State state;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &state);
    wl_display_roundtrip(display);

    if (!state.compositor || !state.wm_base) {
        fprintf(stderr, "test_client: compositor did not advertise wl_compositor/xdg_wm_base\n");
        return 1;
    }
    xdg_wm_base_add_listener(state.wm_base, &kWmBaseListener, nullptr);

    wl_surface* surface = wl_compositor_create_surface(state.compositor);
    xdg_surface* xsurface = xdg_wm_base_get_xdg_surface(state.wm_base, surface);
    xdg_surface_add_listener(xsurface, &kSurfaceListener, nullptr);
    xdg_toplevel* toplevel = xdg_surface_get_toplevel(xsurface);
    xdg_toplevel_add_listener(toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(toplevel, "vulkan_remoting wayland proxy test");

    // The first commit carries no buffer; it only exists to trigger the
    // initial xdg_surface.configure, per the xdg-shell handshake.
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    fprintf(stderr, "test_client: surface object id %u mapped, waiting for host to attach a buffer\n",
            wl_proxy_get_id(reinterpret_cast<wl_proxy*>(surface)));

    for (int i = 0; i < 300 && wl_display_dispatch(display) >= 0; ++i) {
        usleep(50 * 1000);
    }

    wl_display_disconnect(display);
    return 0;
}
