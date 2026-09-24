#pragma once

// A window the server creates and owns itself, rather than one replayed from
// an application (see wayland/proxy_server.hpp for that other path). This is
// what backs a surface opcode whose source window system is not native to the
// server: the client's platform handles have no meaning in the server process,
// so there is nothing to adopt and the server has to make its own surface to
// present through.
//
// On Linux this deliberately opens its own wl_display connection instead of
// reusing WaylandProxy's, so this works even when the server was not
// started with --wayland. On Windows there is no compositor connection to
// share in the first place - every OwnWindow just owns a plain HWND - so
// the same reasoning does not apply there, but the "one per surface
// request" lifetime still does (see session.hpp's create_own_window).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)

// Forward-declared the same way <windows.h> itself defines them (HWND is
// `struct HWND__*`, HINSTANCE is `struct HINSTANCE__*`), so this header never
// has to pull in <windows.h> - and therefore never has to worry about
// WIN32_LEAN_AND_MEAN ordering - just to name these two handle types (see
// own_window_win32.cpp, which does include <windows.h>, for the real
// definitions this resolves against).
struct HWND__;
using HWND = HWND__*;
struct HINSTANCE__;
using HINSTANCE = HINSTANCE__*;
#else

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_array;
struct wl_seat;
struct wl_pointer;
struct wl_keyboard;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;
#endif

class OwnWindow {
   public:
    enum class InputType : uint32_t { Motion = 1, Button = 2, Axis = 3, Key = 4, Focus = 5 };
    struct InputEvent {
        uint64_t sequence = 0;
        InputType type = InputType::Motion;
        int32_t a = 0;
        int32_t b = 0;
        int32_t c = 0;
        int32_t d = 0;
    };

    OwnWindow() = default;
    ~OwnWindow();

    OwnWindow(const OwnWindow&) = delete;
    OwnWindow& operator=(const OwnWindow&) = delete;

#if defined(_WIN32)
    // Spawns a dedicated thread that registers the window class (once,
    // process-wide), creates the window, and then pumps its message queue
    // until the window is destroyed (see own_window_win32.cpp). A Win32
    // window's messages must be pumped on the thread that created it, and
    // the session thread calling create() cannot itself block in a message
    // loop - it still has a client connection to serve - so the pump has to
    // live on its own thread. create() blocks only long enough for that
    // thread to report the HWND exists (or that creation failed), never for
    // the whole pump lifetime. Returns false - cleanly, never crashing or
    // hanging - if window creation failed.
    bool create();

    HINSTANCE hinstance() const { return hinstance_; }
    HWND hwnd() const { return hwnd_.load(); }

    // Called by the window procedure on the pump thread. These are public so
    // the Win32 callback can stay outside the class without exposing windows.h
    // from this platform-neutral header.
    void record_size(uint32_t width, uint32_t height);
    void record_close() { closed_.store(true); }
    void record_destroyed() { hwnd_.store(nullptr); }

   private:
    HINSTANCE hinstance_ = nullptr;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> shown_{false};
    std::thread pump_thread_;
#else
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
    static void seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities);
    static void seat_name(void*, wl_seat*, const char*) {}
    static void pointer_enter(void* data, wl_pointer*, uint32_t, wl_surface*, int32_t, int32_t);
    static void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*);
    static void pointer_motion(void* data, wl_pointer*, uint32_t, int32_t, int32_t);
    static void pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t);
    static void pointer_axis(void* data, wl_pointer*, uint32_t, uint32_t, int32_t);
    static void pointer_frame(void* data, wl_pointer*);
    static void pointer_axis_source(void*, wl_pointer*, uint32_t) {}
    static void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
    static void pointer_axis_discrete(void* data, wl_pointer*, uint32_t, int32_t);
    static void pointer_axis_value120(void* data, wl_pointer*, uint32_t, int32_t);
    static void keyboard_keymap(void* data, wl_keyboard*, uint32_t, int32_t fd, uint32_t size);
    static void keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface*, wl_array*);
    static void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*);
    static void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t);
    static void keyboard_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t depressed,
                                   uint32_t latched, uint32_t locked, uint32_t group);
    static void keyboard_repeat_info(void* data, wl_keyboard*, int32_t rate, int32_t delay);

   private:
    wl_display* display_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_seat* seat_ = nullptr;
    uint32_t seat_name_ = 0;
    wl_pointer* pointer_ = nullptr;
    uint32_t pointer_version_ = 0;
    wl_keyboard* keyboard_ = nullptr;
    uint32_t keyboard_version_ = 0;
    xdg_wm_base* wm_base_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    bool configured_ = false;
    bool pointer_inside_ = false;
    bool keyboard_focused_ = false;
    uint32_t pressed_pointer_buttons_ = 0;
    int32_t axis_vertical_remainder_ = 0;
    int32_t axis_horizontal_remainder_ = 0;
    bool axis_vertical_discrete_ = false;
    bool axis_horizontal_discrete_ = false;
    int32_t pending_axis_vertical_ = 0;
    int32_t pending_axis_horizontal_ = 0;
    bool pending_axis_vertical_value120_ = false;
    bool pending_axis_horizontal_value120_ = false;
    std::unordered_set<uint32_t> pressed_keys_;
    xkb_context* xkb_context_ = nullptr;
    xkb_keymap* xkb_keymap_ = nullptr;
    xkb_state* xkb_state_ = nullptr;
    std::mutex repeat_mutex_;
    std::condition_variable repeat_cv_;
    std::thread repeat_thread_;
    bool stop_repeat_ = false;
    int32_t repeat_rate_ = 0;
    int32_t repeat_delay_ = 0;
    uint32_t repeating_key_ = 0;
    std::mutex display_mutex_;
    std::atomic<bool> stop_pump_{false};
    std::thread pump_thread_;
#endif

   public:
    // Resize tracking, and the reason it needs a pump at all.
    //
    // The compositor announces a new size through xdg_toplevel.configure,
    // which arrives on *this* connection - the server's - because the
    // server owns the window. The client is blind to it by construction, so
    // unless the size is recorded here and turned back into something the
    // Vulkan API can carry (see handlers_wsi.cpp: a VK_SUBOPTIMAL_KHR out of
    // vkAcquireNextImageKHR, plus a real currentExtent out of
    // vkGetPhysicalDeviceSurfaceCapabilitiesKHR), a resize can never reach
    // the application and the surface keeps its original size forever.
    //
    // pump() exists because nothing else dispatches this connection. The
    // Vulkan driver's WSI reads the socket, but dispatches its own event
    // queue, not the default one these listeners are on, so without an
    // explicit dispatch the configure sits in the queue unread. It must
    // never block: it runs on a session thread that still owes a client a
    // reply.
    void pump();

    // Test-and-clear, so one resize produces one VK_SUBOPTIMAL_KHR rather
    // than a permanent "suboptimal" state that would make the client
    // recreate its swapchain every single frame.
    bool take_resized() { return resized_.exchange(false); }

    // Not a take_/exchange like the resize flag: a closed window stays
    // closed, and every swapchain that asks afterwards needs the same
    // answer rather than only the first one to look.
    bool closed() const { return closed_.load(); }

    uint32_t width() const { return width_.load(); }
    uint32_t height() const { return height_.load(); }

    std::vector<InputEvent> take_input_events(uint32_t max_events, bool* overflowed);
    void record_input(InputType type, int32_t a = 0, int32_t b = 0,
                      int32_t c = 0, int32_t d = 0);

   private:
    // Written from whichever thread dispatches events, read from session
    // threads.
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    std::atomic<bool> resized_{false};
    std::atomic<bool> closed_{false};
    std::mutex input_mutex_;
    std::deque<InputEvent> input_events_;
    uint64_t next_input_sequence_ = 1;
    bool input_overflowed_ = false;
    static constexpr size_t kMaxInputEvents = 1024;
};

