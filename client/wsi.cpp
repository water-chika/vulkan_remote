// Client-side WSI: surface creation, surface queries, and swapchains.
//
// On Linux, a VkWaylandSurfaceCreateInfoKHR carries a wl_display*/wl_surface*
// that belong to the *application's* connection and are meaningless to send;
// only wl_proxy_get_id(pCreateInfo->surface) travels the wire, which is why
// this file links wayland-client at all there (see
// wayland/proxy_server.hpp's surface_for_client_id on the server side).
//
// On Windows there is no libwayland to link and nothing of the application's
// HINSTANCE/HWND that the server could use either (see
// server/handlers_wsi.cpp's handle_CreateWin32SurfaceKHR, which reads and
// discards both): the server answers by opening its own native window instead
// (Wayland on Linux, HWND on Windows). The inverse translation lets a Windows
// server accept a Wayland-source opcode in exactly the same way.

#include <string.h>

#include <algorithm>
#include <cstring>

// windows.h's default (non-lean) mode drags in the legacy winsock.h, which
// conflicts with wire.hpp's winsock2.h if windows.h is reached first in this
// translation unit (as it is here, via vulkan.h's VK_USE_PLATFORM_WIN32_KHR
// path below); defining this before that include keeps them from conflicting
// regardless of which of the two ends up included first.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <windowsx.h>
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <wayland-client.h>
#endif

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "entry_table.hpp"
#include "marshal.hpp"
#include "remote_objects.hpp"
#include "remoting_commands.inl"
#include "wire.hpp"

namespace {

using remoting::Opcode;
using remoting::Reader;
using remoting::RemoteDevice;
using remoting::RemoteInstance;
using remoting::RemotePhysicalDevice;
using remoting::RemoteQueue;
using remoting::Writer;
using remoting::handle_from_id;
using remoting::id_from_handle;

RemoteInstance* to_instance(VkInstance instance) {
    return reinterpret_cast<RemoteInstance*>(instance);
}

RemotePhysicalDevice* to_physical_device(VkPhysicalDevice device) {
    return reinterpret_cast<RemotePhysicalDevice*>(device);
}

RemoteDevice* to_device(VkDevice handle) { return reinterpret_cast<RemoteDevice*>(handle); }
RemoteQueue* to_queue(VkQueue handle) { return reinterpret_cast<RemoteQueue*>(handle); }

bool round_trip(RemoteInstance* instance, Opcode opcode, const Writer& request,
                std::vector<char>* reply) {
    return instance->connection.round_trip(opcode, request, reply);
}

// Every reply to a round trip begins with a status word already checked by
// Connection; call sites here only need the payload after it.
Reader payload_reader(const std::vector<char>& reply) {
    Reader r(reply.data(), reply.size());
    r.u32();
    return r;
}

#if defined(_WIN32)
WPARAM evdev_to_virtual_key(int32_t key) {
    if (key >= 2 && key <= 10) return static_cast<WPARAM>('1' + key - 2);
    if (key == 11) return '0';
    static constexpr char kLetters[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        'Q','W','E','R','T','Y','U','I','O','P',0,0,0,0,
        'A','S','D','F','G','H','J','K','L',0,0,0,0,0,
        'Z','X','C','V','B','N','M'
    };
    if (key >= 0 && static_cast<size_t>(key) < sizeof(kLetters) && kLetters[key]) {
        return static_cast<WPARAM>(kLetters[key]);
    }
    if (key >= 59 && key <= 68) return static_cast<WPARAM>(VK_F1 + key - 59);
    switch (key) {
    case 1: return VK_ESCAPE;
    case 12: return VK_OEM_MINUS;
    case 13: return VK_OEM_PLUS;
    case 14: return VK_BACK;
    case 15: return VK_TAB;
    case 26: return VK_OEM_4;
    case 27: return VK_OEM_6;
    case 28: return VK_RETURN;
    case 29: return VK_LCONTROL;
    case 39: return VK_OEM_1;
    case 40: return VK_OEM_7;
    case 41: return VK_OEM_3;
    case 42: return VK_LSHIFT;
    case 43: return VK_OEM_5;
    case 51: return VK_OEM_COMMA;
    case 52: return VK_OEM_PERIOD;
    case 53: return VK_OEM_2;
    case 54: return VK_RSHIFT;
    case 56: return VK_LMENU;
    case 57: return VK_SPACE;
    case 58: return VK_CAPITAL;
    case 87: return VK_F11;
    case 88: return VK_F12;
    case 97: return VK_RCONTROL;
    case 100: return VK_RMENU;
    case 102: return VK_HOME;
    case 103: return VK_UP;
    case 104: return VK_PRIOR;
    case 105: return VK_LEFT;
    case 106: return VK_RIGHT;
    case 107: return VK_END;
    case 108: return VK_DOWN;
    case 109: return VK_NEXT;
    case 55: return VK_MULTIPLY;
    case 69: return VK_NUMLOCK;
    case 70: return VK_SCROLL;
    case 71: return VK_NUMPAD7;
    case 72: return VK_NUMPAD8;
    case 73: return VK_NUMPAD9;
    case 74: return VK_SUBTRACT;
    case 75: return VK_NUMPAD4;
    case 76: return VK_NUMPAD5;
    case 77: return VK_NUMPAD6;
    case 78: return VK_ADD;
    case 79: return VK_NUMPAD1;
    case 80: return VK_NUMPAD2;
    case 81: return VK_NUMPAD3;
    case 82: return VK_NUMPAD0;
    case 83: return VK_DECIMAL;
    case 96: return VK_RETURN;  // keypad Enter, marked extended below
    case 98: return VK_DIVIDE;
    case 99: return VK_SNAPSHOT;
    case 110: return VK_INSERT;
    case 111: return VK_DELETE;
    case 125: return VK_LWIN;
    case 126: return VK_RWIN;
    default: return 0;
    }
}

bool extended_evdev_key(int32_t evdev, WPARAM key) {
    if (evdev == 96) return true;  // keypad Enter
    switch (key) {
    case VK_RCONTROL: case VK_RMENU: case VK_INSERT: case VK_DELETE:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_NUMLOCK: case VK_SNAPSHOT: case VK_DIVIDE: case VK_LWIN: case VK_RWIN:
        return true;
    default:
        return false;
    }
}

LPARAM key_lparam(int32_t evdev, WPARAM key, bool pressed, bool repeated,
                  bool alt_down = false) {
    UINT scan = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
    if (evdev == 96) scan = MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC);
    LPARAM result = 1 | (static_cast<LPARAM>(scan & 0xffu) << 16);
    if (extended_evdev_key(evdev, key)) result |= (1L << 24);
    if (alt_down) result |= (1L << 29);
    if (repeated || !pressed) result |= (1L << 30);
    if (!pressed) result |= (1L << 31);
    return result;
}

WPARAM mouse_wparam(uint32_t buttons, const std::vector<uint32_t>& pressed_keys) {
    WPARAM result = 0;
    if (buttons & 1u) result |= MK_LBUTTON;
    if (buttons & 2u) result |= MK_RBUTTON;
    if (buttons & 4u) result |= MK_MBUTTON;
    if (buttons & 8u) result |= MK_XBUTTON1;
    if (buttons & 16u) result |= MK_XBUTTON2;
    if (std::find(pressed_keys.begin(), pressed_keys.end(), VK_LSHIFT) != pressed_keys.end() ||
        std::find(pressed_keys.begin(), pressed_keys.end(), VK_RSHIFT) != pressed_keys.end()) {
        result |= MK_SHIFT;
    }
    if (std::find(pressed_keys.begin(), pressed_keys.end(), VK_LCONTROL) != pressed_keys.end() ||
        std::find(pressed_keys.begin(), pressed_keys.end(), VK_RCONTROL) != pressed_keys.end()) {
        result |= MK_CONTROL;
    }
    return result;
}

void post_window_event(HWND hwnd, uint32_t type, int32_t a, int32_t b,
                       uint32_t* buttons, uint32_t* position,
                       std::vector<uint32_t>* pressed_keys) {
    if (!hwnd || !IsWindow(hwnd)) return;
    switch (type) {
    case 1:
        *position = static_cast<uint32_t>(MAKELPARAM(a, b));
        PostMessageW(hwnd, WM_MOUSEMOVE, mouse_wparam(*buttons, *pressed_keys), static_cast<LPARAM>(*position));
        break;
    case 2: {
        UINT msg = 0;
        uint32_t mask = 0;
        if (a == 0x110) { msg = b ? WM_LBUTTONDOWN : WM_LBUTTONUP; mask = 1u; }
        else if (a == 0x111) { msg = b ? WM_RBUTTONDOWN : WM_RBUTTONUP; mask = 2u; }
        else if (a == 0x112) { msg = b ? WM_MBUTTONDOWN : WM_MBUTTONUP; mask = 4u; }
        else if (a == 0x113 || a == 0x114) {
            msg = b ? WM_XBUTTONDOWN : WM_XBUTTONUP;
            const uint32_t xmask = a == 0x113 ? 8u : 16u;
            if (b) *buttons |= xmask; else *buttons &= ~xmask;
            const WORD xbutton = a == 0x113 ? XBUTTON1 : XBUTTON2;
            PostMessageW(hwnd, msg, MAKEWPARAM(mouse_wparam(*buttons, *pressed_keys), xbutton),
                         static_cast<LPARAM>(*position));
            break;
        }
        if (msg) {
            if (b) *buttons |= mask; else *buttons &= ~mask;
            PostMessageW(hwnd, msg, mouse_wparam(*buttons, *pressed_keys), static_cast<LPARAM>(*position));
        }
        break;
    }
    case 3: {
        const int delta = (a == 0 ? -b : b) * WHEEL_DELTA;
        POINT point{GET_X_LPARAM(static_cast<LPARAM>(*position)),
                    GET_Y_LPARAM(static_cast<LPARAM>(*position))};
        ClientToScreen(hwnd, &point);
        PostMessageW(hwnd, a == 0 ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL,
                     MAKEWPARAM(mouse_wparam(*buttons, *pressed_keys), static_cast<short>(delta)),
                     MAKELPARAM(point.x, point.y));
        break;
    }
    case 4: {
        const WPARAM key = evdev_to_virtual_key(a);
        if (key) {
            const bool pressed = b != 0;
            const bool repeated = b == 2;
            const bool alt_down = std::find(pressed_keys->begin(), pressed_keys->end(),
                                            static_cast<uint32_t>(VK_LMENU)) != pressed_keys->end() ||
                                  std::find(pressed_keys->begin(), pressed_keys->end(),
                                            static_cast<uint32_t>(VK_RMENU)) != pressed_keys->end();
            const bool system = alt_down || key == VK_LMENU || key == VK_RMENU;
            PostMessageW(hwnd,
                         pressed ? (system ? WM_SYSKEYDOWN : WM_KEYDOWN)
                                 : (system ? WM_SYSKEYUP : WM_KEYUP),
                         key, key_lparam(a, key, pressed, repeated, alt_down));
            const uint32_t value = static_cast<uint32_t>(key);
            const auto it = std::find(pressed_keys->begin(), pressed_keys->end(), value);
            if (pressed && it == pressed_keys->end()) pressed_keys->push_back(value);
            if (!pressed && it != pressed_keys->end()) pressed_keys->erase(it);
        }
        break;
    }
    case 5: PostMessageW(hwnd, a ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0); break;
    default: break;
    }
}

void poll_window_events(RemoteInstance* instance, uint64_t surface_id) {
    uintptr_t hwnd_value = 0;
    {
        std::lock_guard<std::mutex> lock(instance->surface_input_mutex);
        const auto hwnd_it = instance->surface_windows.find(surface_id);
        if (hwnd_it == instance->surface_windows.end()) return;
        hwnd_value = hwnd_it->second;
    }
    Writer request;
    request.handle(surface_id);
    request.u32(128);
    std::vector<char> reply;
    if (!instance->connection.try_round_trip(Opcode::PollWindowEvents, request, &reply)) return;
    Reader r = payload_reader(reply);
    const bool overflowed = r.u32() != 0;
    const uint32_t count = r.u32();
    if (count > 128) return;
    std::lock_guard<std::mutex> lock(instance->surface_input_mutex);
    const auto hwnd_it = instance->surface_windows.find(surface_id);
    if (hwnd_it == instance->surface_windows.end() || hwnd_it->second != hwnd_value) return;
    HWND hwnd = reinterpret_cast<HWND>(hwnd_value);
    uint32_t& buttons = instance->surface_mouse_buttons[surface_id];
    uint32_t& position = instance->surface_mouse_positions[surface_id];
    std::vector<uint32_t>& pressed_keys = instance->surface_pressed_keys[surface_id];
    if (overflowed) {
        if (buttons & 1u) PostMessageW(hwnd, WM_LBUTTONUP, 0, static_cast<LPARAM>(position));
        if (buttons & 2u) PostMessageW(hwnd, WM_RBUTTONUP, 0, static_cast<LPARAM>(position));
        if (buttons & 4u) PostMessageW(hwnd, WM_MBUTTONUP, 0, static_cast<LPARAM>(position));
        if (buttons & 8u) PostMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1),
                                     static_cast<LPARAM>(position));
        if (buttons & 16u) PostMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2),
                                      static_cast<LPARAM>(position));
        buttons = 0;
        for (uint32_t key : pressed_keys) {
            const bool system = key == VK_LMENU || key == VK_RMENU;
            PostMessageW(hwnd, system ? WM_SYSKEYUP : WM_KEYUP, key,
                         key_lparam(0, key, false, false,
                                    system));
        }
        pressed_keys.clear();
    }
    for (uint32_t i = 0; i < count; ++i) {
        r.u64();
        const uint32_t type = r.u32();
        const int32_t a = r.i32();
        const int32_t b = r.i32();
        r.i32();
        r.i32();
        if (!r.ok()) return;
        post_window_event(hwnd, type, a, b, &buttons, &position, &pressed_keys);
    }
}
#endif

// ---------------------------------------------------------------------------
// Instance / physical device: Wayland surface, surface queries.
// ---------------------------------------------------------------------------

#if !defined(_WIN32)
VKAPI_ATTR VkResult VKAPI_CALL CreateWaylandSurfaceKHR(VkInstance handle,
                                                       const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks*,
                                                       VkSurfaceKHR* pSurface) {
    RemoteInstance* instance = to_instance(handle);

    // pCreateInfo->display/->surface belong to the application's own Wayland
    // connection; sending those pointers would be meaningless on the server.
    // What travels instead is the application-side object id, which the
    // server's WaylandProxy can turn back into a real, local wl_surface* (see
    // wayland/proxy_server.hpp's surface_for_client_id).
    const uint32_t app_object_id =
        wl_proxy_get_id(reinterpret_cast<struct wl_proxy*>(pCreateInfo->surface));

    // wl_compositor.create_surface only enqueues a request in the app's local
    // libwayland write buffer; nothing guarantees it has even reached this
    // process's socket yet, let alone been replayed by the proxy chain and
    // applied by the real compositor. A round trip forces the request out
    // and blocks until the (real, remote) compositor has processed every
    // request before it, so the server's WaylandProxy is guaranteed to know
    // about this surface id by the time our RPC below reaches it.
    wl_display_roundtrip(pCreateInfo->display);

    Writer request;
    request.u32(app_object_id);
    std::vector<char> reply;
    if (!round_trip(instance, Opcode::vkCreateWaylandSurfaceKHR, request, &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    const VkResult result = static_cast<VkResult>(reader.i32());
    const uint64_t id = reader.handle();
    if (!reader.ok()) return VK_ERROR_INITIALIZATION_FAILED;
    if (result != VK_SUCCESS) return result;
    *pSurface = handle_from_id<VkSurfaceKHR>(id);
    return VK_SUCCESS;
}
#endif  // !defined(_WIN32)

#if defined(_WIN32)
VKAPI_ATTR VkResult VKAPI_CALL CreateWin32SurfaceKHR(VkInstance handle,
                                                     const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                                     const VkAllocationCallbacks*,
                                                     VkSurfaceKHR* pSurface) {
    RemoteInstance* instance = to_instance(handle);

    // hinstance/hwnd name a window in the Win32 world only, and the server
    // has no way to turn either back into anything real (there is no Win32
    // window on the GPU machine); both still travel the wire because the
    // server's handler (see server/handlers_wsi.cpp's
    // handle_CreateWin32SurfaceKHR) must read exactly what this call writes
    // to stay in step, even though it discards them and opens its own
    // server-owned Wayland window instead.
    Writer request;
    request.u64(reinterpret_cast<uint64_t>(pCreateInfo->hinstance));
    request.u64(reinterpret_cast<uint64_t>(pCreateInfo->hwnd));
    std::vector<char> reply;
    if (!round_trip(instance, Opcode::vkCreateWin32SurfaceKHR, request, &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    const VkResult result = static_cast<VkResult>(reader.i32());
    const uint64_t id = reader.handle();
    if (!reader.ok()) return VK_ERROR_INITIALIZATION_FAILED;
    if (result != VK_SUCCESS) return result;
    *pSurface = handle_from_id<VkSurfaceKHR>(id);
    {
        std::lock_guard<std::mutex> lock(instance->surface_input_mutex);
        instance->surface_windows[id] = reinterpret_cast<uintptr_t>(pCreateInfo->hwnd);
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkBool32 VKAPI_CALL GetPhysicalDeviceWin32PresentationSupportKHR(
    VkPhysicalDevice handle, uint32_t queueFamilyIndex) {
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.u32(queueFamilyIndex);
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceWin32PresentationSupportKHR,
                    request, &reply)) {
        return VK_FALSE;
    }
    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    const uint32_t supported = reader.u32();
    return reader.ok() && supported != 0 ? VK_TRUE : VK_FALSE;
}
#endif  // defined(_WIN32)

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance handle, VkSurfaceKHR surface,
                                             const VkAllocationCallbacks*) {
    if (surface == VK_NULL_HANDLE) return;
    RemoteInstance* instance = to_instance(handle);
    const uint64_t id = id_from_handle(surface);
#if defined(_WIN32)
    {
        std::lock_guard<std::mutex> lock(instance->surface_input_mutex);
        instance->surface_windows.erase(id);
        instance->surface_mouse_buttons.erase(id);
        instance->surface_mouse_positions.erase(id);
        instance->surface_pressed_keys.erase(id);
    }
#endif
    Writer request;
    request.handle(id);
    instance->connection.send_oneway(Opcode::vkDestroySurfaceKHR, request);
}

#if !defined(_WIN32)
VKAPI_ATTR VkBool32 VKAPI_CALL GetPhysicalDeviceWaylandPresentationSupportKHR(
    VkPhysicalDevice handle, uint32_t queueFamilyIndex, struct wl_display*) {
    // The wl_display* argument names the application's own connection, which
    // the server cannot use either; the question this answers is really "can
    // the remote device present to *a* Wayland surface at all", which the
    // server can answer from its own compositor connection (if it has one).
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.u32(queueFamilyIndex);
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceWaylandPresentationSupportKHR,
                    request, &reply)) {
        return VK_FALSE;
    }
    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    return reader.u32() != 0 ? VK_TRUE : VK_FALSE;
}
#endif  // !defined(_WIN32)

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice handle,
                                                                  uint32_t queueFamilyIndex,
                                                                  VkSurfaceKHR surface,
                                                                  VkBool32* pSupported) {
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.u32(queueFamilyIndex);
    request.handle(id_from_handle(surface));
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceSurfaceSupportKHR, request,
                    &reply)) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    const VkResult result = static_cast<VkResult>(reader.i32());
    *pSupported = reader.u32() != 0 ? VK_TRUE : VK_FALSE;
    return reader.ok() ? result : VK_ERROR_SURFACE_LOST_KHR;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice handle, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pCapabilities) {
    memset(pCapabilities, 0, sizeof(*pCapabilities));
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(surface));
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceSurfaceCapabilitiesKHR, request,
                    &reply)) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    const VkResult result = static_cast<VkResult>(reader.i32());
    std::vector<char> raw;
    if (!reader.bytes(&raw) || raw.size() != sizeof(*pCapabilities)) return VK_ERROR_SURFACE_LOST_KHR;
    memcpy(pCapabilities, raw.data(), sizeof(*pCapabilities));
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice handle,
                                                                  VkSurfaceKHR surface,
                                                                  uint32_t* pCount,
                                                                  VkSurfaceFormatKHR* pFormats) {
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(surface));
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceSurfaceFormatsKHR, request,
                    &reply)) {
        *pCount = 0;
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    const VkResult remote_result = static_cast<VkResult>(reader.i32());
    const uint32_t count = reader.u32();
    if (count > reader.remaining() / (2 * sizeof(uint32_t))) {
        *pCount = 0;
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    std::vector<VkSurfaceFormatKHR> formats(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!read_SurfaceFormatKHR(reader, &formats[i])) {
            *pCount = 0;
            return VK_ERROR_SURFACE_LOST_KHR;
        }
    }
    if (!reader.ok()) {
        *pCount = 0;
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    if (pFormats == nullptr) {
        *pCount = count;
        return remote_result;
    }
    const uint32_t to_write = *pCount < count ? *pCount : count;
    for (uint32_t i = 0; i < to_write; ++i) pFormats[i] = formats[i];
    *pCount = to_write;
    return to_write < count ? VK_INCOMPLETE : remote_result;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice handle,
                                                                       VkSurfaceKHR surface,
                                                                       uint32_t* pCount,
                                                                       VkPresentModeKHR* pModes) {
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(surface));
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceSurfacePresentModesKHR, request,
                    &reply)) {
        *pCount = 0;
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    const VkResult remote_result = static_cast<VkResult>(reader.i32());
    const uint32_t count = reader.u32();
    std::vector<VkPresentModeKHR> modes(count);
    for (uint32_t i = 0; i < count; ++i) modes[i] = static_cast<VkPresentModeKHR>(reader.i32());
    if (!reader.ok()) {
        *pCount = 0;
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    if (pModes == nullptr) {
        *pCount = count;
        return remote_result;
    }
    const uint32_t to_write = *pCount < count ? *pCount : count;
    for (uint32_t i = 0; i < to_write; ++i) pModes[i] = modes[i];
    *pCount = to_write;
    return to_write < count ? VK_INCOMPLETE : remote_result;
}

// ---------------------------------------------------------------------------
// Device: swapchain.
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice handle,
                                                  const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                  const VkAllocationCallbacks*,
                                                  VkSwapchainKHR* pSwapchain) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    write_SwapchainCreateInfoKHR(request, *pCreateInfo);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkCreateSwapchainKHR, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint64_t id = r.handle();
    if (!r.ok()) return VK_ERROR_DEVICE_LOST;
    if (result == VK_SUCCESS) {
        *pSwapchain = handle_from_id<VkSwapchainKHR>(id);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice handle, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks*) {
    if (swapchain == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    const uint64_t id = id_from_handle(swapchain);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id);
    device->instance->connection.send_oneway(Opcode::vkDestroySwapchainKHR, request);
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice handle, VkSwapchainKHR swapchain,
                                                     uint32_t* pCount, VkImage* pImages) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(swapchain));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetSwapchainImagesKHR, request,
                                                 &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint32_t count = r.u32();
    std::vector<uint64_t> ids(count);
    for (uint32_t i = 0; i < count; ++i) ids[i] = r.handle();
    if (!r.ok()) return VK_ERROR_DEVICE_LOST;

    // These ids name real driver images the client never created and must
    // never destroy (the spec says so); they behave like any other image id
    // to the rest of this file, which is why they need no bookkeeping beyond
    // being handed back as VkImage handles.
    if (pImages == nullptr) {
        *pCount = count;
        return result;
    }
    const uint32_t to_write = *pCount < count ? *pCount : count;
    for (uint32_t i = 0; i < to_write; ++i) pImages[i] = handle_from_id<VkImage>(ids[i]);
    *pCount = to_write;
    return to_write < count ? VK_INCOMPLETE : result;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice handle, VkSwapchainKHR swapchain,
                                                   uint64_t timeout, VkSemaphore semaphore,
                                                   VkFence fence, uint32_t* pImageIndex) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(swapchain));
    request.u64(timeout);
    request.handle(id_from_handle(semaphore));
    request.handle(id_from_handle(fence));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkAcquireNextImageKHR, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    *pImageIndex = r.u32();
#if defined(_WIN32)
    std::vector<uint64_t> surfaces;
    {
        std::lock_guard<std::mutex> lock(device->instance->surface_input_mutex);
        for (const auto& item : device->instance->surface_windows) surfaces.push_back(item.first);
    }
    for (uint64_t surface_id : surfaces) poll_window_events(device->instance, surface_id);
#endif
    return r.ok() ? result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue handle,
                                               const VkPresentInfoKHR* pPresentInfo) {
    RemoteQueue* queue = to_queue(handle);
    RemoteDevice* device = queue->device;

    // Same reason as vkQueueSubmit: the server must see whatever the
    // application wrote through its mappings before this present can show
    // it, and a present is exactly the kind of call an application waits on
    // the timing of - it must not be turned into a fire-and-forget one.
    device->flush_mapped();

    Writer request;
    request.handle(queue->remote_id);
    request.u32(pPresentInfo->waitSemaphoreCount);
    for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; ++i) {
        request.handle(id_from_handle(pPresentInfo->pWaitSemaphores[i]));
    }
    request.u32(pPresentInfo->swapchainCount);
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
        request.handle(id_from_handle(pPresentInfo->pSwapchains[i]));
        request.u32(pPresentInfo->pImageIndices[i]);
    }

    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkQueuePresentKHR, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count; ++i) {
        const VkResult per_swapchain = static_cast<VkResult>(r.i32());
        if (pPresentInfo->pResults) pPresentInfo->pResults[i] = per_swapchain;
    }
#if defined(_WIN32)
    std::vector<uint64_t> surfaces;
    {
        std::lock_guard<std::mutex> lock(device->instance->surface_input_mutex);
        for (const auto& item : device->instance->surface_windows) surfaces.push_back(item.first);
    }
    for (uint64_t surface_id : surfaces) poll_window_events(device->instance, surface_id);
#endif
    return r.ok() ? result : VK_ERROR_DEVICE_LOST;
}

}  // namespace

namespace remoting {

const DeviceEntry* get_wsi_instance_entries(size_t* count) {
    static const DeviceEntry kEntries[] = {
#define D(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name)}
#if defined(_WIN32)
        D(CreateWin32SurfaceKHR),
        D(GetPhysicalDeviceWin32PresentationSupportKHR),
#else
        D(CreateWaylandSurfaceKHR),
        D(GetPhysicalDeviceWaylandPresentationSupportKHR),
#endif
        D(DestroySurfaceKHR),
        D(GetPhysicalDeviceSurfaceSupportKHR),
        D(GetPhysicalDeviceSurfaceCapabilitiesKHR),
        D(GetPhysicalDeviceSurfaceFormatsKHR),
        D(GetPhysicalDeviceSurfacePresentModesKHR),
#undef D
    };
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

const DeviceEntry* get_wsi_device_entries(size_t* count) {
    static const DeviceEntry kEntries[] = {
#define D(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name)}
        D(CreateSwapchainKHR),
        D(DestroySwapchainKHR),
        D(GetSwapchainImagesKHR),
        D(AcquireNextImageKHR),
        D(QueuePresentKHR),
#undef D
    };
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

}  // namespace remoting
