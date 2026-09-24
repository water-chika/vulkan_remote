#include "own_window.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/mman.h>

#include <poll.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

namespace {

const wl_registry_listener kRegistryListener = {OwnWindow::registry_global,
                                                 OwnWindow::registry_global_remove};
const xdg_wm_base_listener kWmBaseListener = {OwnWindow::wm_base_ping};
const xdg_surface_listener kSurfaceListener = {OwnWindow::surface_configure};
const xdg_toplevel_listener kToplevelListener = {OwnWindow::toplevel_configure,
                                                  OwnWindow::toplevel_close};
const wl_seat_listener kSeatListener = {OwnWindow::seat_capabilities, OwnWindow::seat_name};
#if defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
const wl_pointer_listener kPointerListener = {
    OwnWindow::pointer_enter, OwnWindow::pointer_leave, OwnWindow::pointer_motion,
    OwnWindow::pointer_button, OwnWindow::pointer_axis, OwnWindow::pointer_frame,
    OwnWindow::pointer_axis_source, OwnWindow::pointer_axis_stop,
    OwnWindow::pointer_axis_discrete, OwnWindow::pointer_axis_value120};
#else
const wl_pointer_listener kPointerListener = {
    OwnWindow::pointer_enter, OwnWindow::pointer_leave, OwnWindow::pointer_motion,
    OwnWindow::pointer_button, OwnWindow::pointer_axis, OwnWindow::pointer_frame,
    OwnWindow::pointer_axis_source, OwnWindow::pointer_axis_stop,
    OwnWindow::pointer_axis_discrete};
#endif
#if defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
constexpr uint32_t kSeatVersion = WL_POINTER_AXIS_VALUE120_SINCE_VERSION;
#else
constexpr uint32_t kSeatVersion = 5;
#endif
const wl_keyboard_listener kKeyboardListener = {
    OwnWindow::keyboard_keymap, OwnWindow::keyboard_enter, OwnWindow::keyboard_leave,
    OwnWindow::keyboard_key, OwnWindow::keyboard_modifiers, OwnWindow::keyboard_repeat_info};

}  // namespace

void OwnWindow::registry_global(void* data, wl_registry* registry, uint32_t name,
                                 const char* interface, uint32_t version) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        self->compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !self->seat_) {
        self->seat_name_ = name;
        self->seat_ = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface,
                             version < kSeatVersion ? version : kSeatVersion));
        wl_seat_add_listener(self->seat_, &kSeatListener, self);
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        self->wm_base_ =
            static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    }
}

void OwnWindow::registry_global_remove(void* data, wl_registry*, uint32_t name) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (name != self->seat_name_) return;
    // Input objects are released with the display connection below. Sending
    // release requests from a registry-removal callback can target an already
    // removed global; first synthesize local releases and drop our pointers.
    if (self->keyboard_) {
        for (uint32_t key : self->pressed_keys_) {
            self->record_input(InputType::Key, static_cast<int32_t>(key), 0);
        }
        self->pressed_keys_.clear();
        {
            std::lock_guard<std::mutex> lock(self->repeat_mutex_);
            self->repeating_key_ = 0;
        }
        self->repeat_cv_.notify_all();
        self->keyboard_focused_ = false;
        self->record_input(InputType::Focus, 0);
        self->keyboard_ = nullptr;
        self->keyboard_version_ = 0;
    }
    if (self->pointer_) {
        for (uint32_t bit = 0; bit < 5; ++bit) {
            if (self->pressed_pointer_buttons_ & (1u << bit)) {
                self->record_input(InputType::Button, static_cast<int32_t>(0x110 + bit), 0);
            }
        }
        self->pressed_pointer_buttons_ = 0;
        self->pointer_inside_ = false;
        self->pointer_ = nullptr;
        self->pointer_version_ = 0;
    }
    if (self->seat_) { wl_seat_destroy(self->seat_); self->seat_ = nullptr; }
    self->seat_name_ = 0;
}

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
    // Both exchanges must happen, so neither may sit on the right of a
    // short-circuiting ||: dragging a corner changes width and height at
    // once, and evaluating only the width would leave height_ holding the
    // old value - reporting a half-updated extent that looks to the client
    // like the window never resized at all.
    const bool width_changed = self->width_.exchange(w) != w;
    const bool height_changed = self->height_.exchange(h) != h;
    if (width_changed || height_changed) {
        self->resized_.store(true);
    }
}

void OwnWindow::seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_) {
        self->pointer_ = wl_seat_get_pointer(seat);
        self->pointer_version_ = wl_proxy_get_version(reinterpret_cast<wl_proxy*>(self->pointer_));
        wl_pointer_add_listener(self->pointer_, &kPointerListener, self);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && self->pointer_) {
        for (uint32_t bit = 0; bit < 5; ++bit) {
            if (self->pressed_pointer_buttons_ & (1u << bit)) {
                self->record_input(InputType::Button, static_cast<int32_t>(0x110 + bit), 0);
            }
        }
        self->pressed_pointer_buttons_ = 0;
        self->pointer_inside_ = false;
        if (self->pointer_version_ >= WL_POINTER_RELEASE_SINCE_VERSION) wl_pointer_release(self->pointer_);
        else wl_pointer_destroy(self->pointer_);
        self->pointer_ = nullptr;
        self->pointer_version_ = 0;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !self->keyboard_) {
        self->keyboard_ = wl_seat_get_keyboard(seat);
        self->keyboard_version_ = wl_proxy_get_version(reinterpret_cast<wl_proxy*>(self->keyboard_));
        wl_keyboard_add_listener(self->keyboard_, &kKeyboardListener, self);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && self->keyboard_) {
        for (uint32_t key : self->pressed_keys_) {
            self->record_input(InputType::Key, static_cast<int32_t>(key), 0);
        }
        self->pressed_keys_.clear();
        {
            std::lock_guard<std::mutex> lock(self->repeat_mutex_);
            self->repeating_key_ = 0;
        }
        self->repeat_cv_.notify_all();
        self->keyboard_focused_ = false;
        self->record_input(InputType::Focus, 0);
        if (self->keyboard_version_ >= WL_KEYBOARD_RELEASE_SINCE_VERSION) wl_keyboard_release(self->keyboard_);
        else wl_keyboard_destroy(self->keyboard_);
        self->keyboard_ = nullptr;
        self->keyboard_version_ = 0;
    }
}

void OwnWindow::pointer_enter(void* data, wl_pointer*, uint32_t, wl_surface* surface,
                              int32_t x, int32_t y) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    self->pointer_inside_ = surface == self->surface_;
    if (self->pointer_inside_) self->record_input(InputType::Motion, x >> 8, y >> 8);
}
void OwnWindow::pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    for (uint32_t bit = 0; bit < 5; ++bit) {
        if (self->pressed_pointer_buttons_ & (1u << bit)) {
            self->record_input(InputType::Button, static_cast<int32_t>(0x110 + bit), 0);
        }
    }
    self->pressed_pointer_buttons_ = 0;
    self->pointer_inside_ = false;
}
void OwnWindow::pointer_motion(void* data, wl_pointer*, uint32_t, int32_t x, int32_t y) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (self->pointer_inside_) self->record_input(InputType::Motion, x >> 8, y >> 8);
}
void OwnWindow::pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button,
                               uint32_t state) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (self->pointer_inside_) {
        uint32_t mask = button == 0x110 ? 1u : button == 0x111 ? 2u :
                        button == 0x112 ? 4u : button == 0x113 ? 8u :
                        button == 0x114 ? 16u : 0u;
        if (state) self->pressed_pointer_buttons_ |= mask;
        else self->pressed_pointer_buttons_ &= ~mask;
        self->record_input(InputType::Button, static_cast<int32_t>(button),
                           static_cast<int32_t>(state));
    }
}
void OwnWindow::pointer_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, int32_t value) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (!self->pointer_inside_) return;
    (axis == 0 ? self->pending_axis_vertical_ : self->pending_axis_horizontal_) += value;
    // wl_pointer.frame was introduced in version 5. Older pointers deliver each
    // axis callback as a complete logical event, so flush it immediately.
    if (self->pointer_version_ < 5) pointer_frame(self, self->pointer_);
}

void OwnWindow::pointer_frame(void* data, wl_pointer*) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    auto emit = [self](uint32_t axis, int32_t& pending, bool& value120,
                       bool& discrete, int32_t& remainder) {
        if (!pending) { value120 = false; discrete = false; return; }
        if (value120) {
            remainder += pending;
            const int32_t detents = remainder / 120;
            remainder %= 120;
            if (detents) self->record_input(InputType::Axis, static_cast<int32_t>(axis), detents);
        } else if (discrete) {
            self->record_input(InputType::Axis, static_cast<int32_t>(axis), pending);
        } else {
            remainder += pending;
            constexpr int32_t kPixelsPerDetent = 15 * 256;
            const int32_t detents = remainder / kPixelsPerDetent;
            remainder %= kPixelsPerDetent;
            if (detents) self->record_input(InputType::Axis, static_cast<int32_t>(axis), detents);
        }
        pending = 0; value120 = false; discrete = false;
    };
    emit(0, self->pending_axis_vertical_, self->pending_axis_vertical_value120_,
         self->axis_vertical_discrete_, self->axis_vertical_remainder_);
    emit(1, self->pending_axis_horizontal_, self->pending_axis_horizontal_value120_,
         self->axis_horizontal_discrete_, self->axis_horizontal_remainder_);
}

void OwnWindow::pointer_axis_discrete(void* data, wl_pointer*, uint32_t axis, int32_t discrete) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (!self->pointer_inside_ || discrete == 0) return;
    (axis == 0 ? self->axis_vertical_discrete_ : self->axis_horizontal_discrete_) = true;
    (axis == 0 ? self->pending_axis_vertical_ : self->pending_axis_horizontal_) = discrete;
}

void OwnWindow::pointer_axis_value120(void* data, wl_pointer*, uint32_t axis, int32_t value120) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (!self->pointer_inside_ || value120 == 0) return;
    (axis == 0 ? self->pending_axis_vertical_ : self->pending_axis_horizontal_) = value120;
    (axis == 0 ? self->pending_axis_vertical_value120_
               : self->pending_axis_horizontal_value120_) = true;
    (axis == 0 ? self->axis_vertical_discrete_ : self->axis_horizontal_discrete_) = true;
}
void OwnWindow::keyboard_keymap(void* data, wl_keyboard*, uint32_t format,
                                int32_t fd, uint32_t size) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (fd < 0) return;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0) {
        void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            if (!self->xkb_context_) self->xkb_context_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            xkb_keymap* keymap = self->xkb_context_
                ? xkb_keymap_new_from_string(self->xkb_context_, static_cast<const char*>(map),
                                             XKB_KEYMAP_FORMAT_TEXT_V1,
                                             XKB_KEYMAP_COMPILE_NO_FLAGS)
                : nullptr;
            if (keymap) {
                xkb_state* state = xkb_state_new(keymap);
                if (state) {
                    if (self->xkb_state_) xkb_state_unref(self->xkb_state_);
                    if (self->xkb_keymap_) xkb_keymap_unref(self->xkb_keymap_);
                    self->xkb_keymap_ = keymap;
                    self->xkb_state_ = state;
                } else {
                    xkb_keymap_unref(keymap);
                }
            }
            munmap(map, size);
        }
    }
    ::close(fd);
}

void OwnWindow::keyboard_modifiers(void* data, wl_keyboard*, uint32_t,
                                   uint32_t depressed, uint32_t latched,
                                   uint32_t locked, uint32_t group) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (self->xkb_state_) {
        xkb_state_update_mask(self->xkb_state_, depressed, latched, locked, 0, 0, group);
    }
}
void OwnWindow::keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface* surface,
                               wl_array* keys) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    self->keyboard_focused_ = surface == self->surface_;
    self->record_input(InputType::Focus, self->keyboard_focused_ ? 1 : 0);
    if (!self->keyboard_focused_ || !keys) return;
    for (void* item = keys->data; item < static_cast<char*>(keys->data) + keys->size;
         item = static_cast<char*>(item) + sizeof(uint32_t)) {
        const uint32_t key = *static_cast<uint32_t*>(item);
        if (self->pressed_keys_.insert(key).second) {
            self->record_input(InputType::Key, static_cast<int32_t>(key), 1);
        }
    }
}
void OwnWindow::keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    for (uint32_t key : self->pressed_keys_) {
        self->record_input(InputType::Key, static_cast<int32_t>(key), 0);
    }
    self->pressed_keys_.clear();
    { std::lock_guard<std::mutex> lock(self->repeat_mutex_); self->repeating_key_ = 0; }
    self->repeat_cv_.notify_all();
    self->keyboard_focused_ = false;
    self->record_input(InputType::Focus, 0);
}
void OwnWindow::keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
                             uint32_t state) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    if (self->keyboard_focused_) {
        if (state) self->pressed_keys_.insert(key); else self->pressed_keys_.erase(key);
        self->record_input(InputType::Key, static_cast<int32_t>(key),
                           static_cast<int32_t>(state));
        std::lock_guard<std::mutex> lock(self->repeat_mutex_);
        bool repeats = true;
        if (self->xkb_keymap_) {
            repeats = xkb_keymap_key_repeats(self->xkb_keymap_, key + 8) != 0;
        }
        if (state && repeats) self->repeating_key_ = key;
        else if (!state && self->repeating_key_ == key) self->repeating_key_ = 0;
        self->repeat_cv_.notify_all();
    }
}

void OwnWindow::keyboard_repeat_info(void* data, wl_keyboard*, int32_t rate, int32_t delay) {
    OwnWindow* self = static_cast<OwnWindow*>(data);
    std::lock_guard<std::mutex> lock(self->repeat_mutex_);
    self->repeat_rate_ = std::max(rate, 0);
    self->repeat_delay_ = std::max(delay, 0);
    self->repeat_cv_.notify_all();
}

void OwnWindow::toplevel_close(void* data, xdg_toplevel*) {
    // The compositor is relaying "the user closed this window". There is no
    // way to push that to the client, which only ever asks us things, so it
    // is recorded here and reported at the next vkAcquireNextImageKHR as
    // VK_ERROR_SURFACE_LOST_KHR - the Vulkan-native way to say the surface
    // is gone, and fatal enough that a client exits rather than spinning.
    static_cast<OwnWindow*>(data)->closed_.store(true);
}

void OwnWindow::pump() {
    // The dedicated pump thread owns this display after create() succeeds.
}

OwnWindow::~OwnWindow() {
    stop_pump_.store(true);
    { std::lock_guard<std::mutex> lock(repeat_mutex_); stop_repeat_ = true; }
    repeat_cv_.notify_all();
    if (repeat_thread_.joinable()) repeat_thread_.join();
    if (pump_thread_.joinable()) pump_thread_.join();
    if (xkb_state_) xkb_state_unref(xkb_state_);
    if (xkb_keymap_) xkb_keymap_unref(xkb_keymap_);
    if (xkb_context_) xkb_context_unref(xkb_context_);
    if (keyboard_) {
        if (keyboard_version_ >= WL_KEYBOARD_RELEASE_SINCE_VERSION) wl_keyboard_release(keyboard_);
        else wl_keyboard_destroy(keyboard_);
    }
    if (pointer_) {
        if (pointer_version_ >= WL_POINTER_RELEASE_SINCE_VERSION) wl_pointer_release(pointer_);
        else wl_pointer_destroy(pointer_);
    }
    if (seat_) wl_seat_destroy(seat_);
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

    // The real driver's WSI can block in vkQueuePresentKHR waiting for the
    // compositor. Keep this connection moving independently of the session
    // thread, rather than relying on the next vkAcquireNextImageKHR to pump it.
    repeat_thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(repeat_mutex_);
        while (!stop_repeat_) {
            repeat_cv_.wait(lock, [this] { return stop_repeat_ || (repeating_key_ && repeat_rate_ > 0); });
            if (stop_repeat_) break;
            const uint32_t key = repeating_key_;
            const auto delay = std::chrono::milliseconds(repeat_delay_);
            if (repeat_cv_.wait_for(lock, delay, [this, key] { return stop_repeat_ || repeating_key_ != key; })) continue;
            while (!stop_repeat_ && repeating_key_ == key && repeat_rate_ > 0) {
                lock.unlock();
                record_input(InputType::Key, static_cast<int32_t>(key), 2);
                lock.lock();
                const int32_t interval_ms = std::max<int32_t>(1, 1000 / repeat_rate_);
                repeat_cv_.wait_for(lock, std::chrono::milliseconds(interval_ms),
                                    [this, key] { return stop_repeat_ || repeating_key_ != key; });
            }
        }
    });
    pump_thread_ = std::thread([this] {
        while (!stop_pump_.load()) {
            std::lock_guard<std::mutex> lock(display_mutex_);
            while (wl_display_prepare_read(display_) != 0) {
                wl_display_dispatch_pending(display_);
            }
            wl_display_flush(display_);
            pollfd pfd{wl_display_get_fd(display_), POLLIN, 0};
            if (::poll(&pfd, 1, 1) > 0 && (pfd.revents & POLLIN)) {
                wl_display_read_events(display_);
            } else {
                wl_display_cancel_read(display_);
            }
            wl_display_dispatch_pending(display_);
        }
    });
    return true;
}
