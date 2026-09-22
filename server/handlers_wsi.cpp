// Server-side WSI handlers: surface creation/destruction (Wayland on Linux,
// a server-owned Win32 window on Windows - see own_window.hpp), every
// vkGetPhysicalDeviceSurface*KHR query, and swapchain create/destroy/
// get-images/acquire/present. See wayland/proxy_server.hpp for the embedded
// compositor proxy the Linux surface calls talk to.

#include <cstring>
#include <vector>

// windows.h's default (non-lean) mode drags in the legacy winsock.h, which
// conflicts with wire.hpp's winsock2.h if windows.h is reached first in this
// translation unit (as it is here, via vulkan.h's VK_USE_PLATFORM_WIN32_KHR
// path below); defining this before that include keeps them from
// conflicting regardless of which of the two ends up included first (see
// client/wsi.cpp lines 20-31 for the same pattern on the client side).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif

#include <vulkan/vulkan.h>

#include "marshal.hpp"
#include "own_window.hpp"
#if !defined(_WIN32)
#include "proxy_server.hpp"
#endif
#include "session.hpp"

namespace {

using remoting::Arena;
using remoting::Session;
using remoting::Status;
using remoting::mark_oneway_error;

#if !defined(_WIN32)
void handle_CreateWaylandSurfaceKHR(Session& c) {
    const uint32_t app_object_id = c.reader.u32();
    if (!c.reader.ok()) {
        c.writer.u32(static_cast<uint32_t>(Status::Ok));
        c.writer.i32(VK_ERROR_INITIALIZATION_FAILED);
        c.writer.handle(0);
        return;
    }

    WaylandProxy* wayland = c.server.wayland();
    if (!wayland) {
        fprintf(stderr,
                "server: vkCreateWaylandSurfaceKHR requested but the server was not started "
                "with --wayland\n");
        c.writer.u32(static_cast<uint32_t>(Status::Ok));
        c.writer.i32(VK_ERROR_INITIALIZATION_FAILED);
        c.writer.handle(0);
        return;
    }

    wl_surface* surface = wayland->surface_for_client_id(app_object_id);
    if (!surface) {
        // Naming the wrong window is worse than refusing outright, so this
        // never falls back to inventing one.
        fprintf(stderr,
                "server: vkCreateWaylandSurfaceKHR: application object id %u is not a wl_surface "
                "the proxy has seen\n",
                app_object_id);
        c.writer.u32(static_cast<uint32_t>(Status::Ok));
        c.writer.i32(VK_ERROR_INITIALIZATION_FAILED);
        c.writer.handle(0);
        return;
    }

    VkWaylandSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = wayland->display();
    info.surface = surface;

    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    const VkResult result = vkCreateWaylandSurfaceKHR(c.server.instance(), &info, nullptr, &vk_surface);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.surfaces.add(vk_surface) : 0);
    c.reply();
}
#endif  // !defined(_WIN32)

void handle_CreateWin32SurfaceKHR(Session& c) {
    const uint64_t hinstance = c.reader.u64();
    const uint64_t hwnd = c.reader.u64();
#if defined(_WIN32)
    // On a Windows server there is no application HINSTANCE/HWND worth
    // trusting either - the client and server are different machines, so
    // the client's handles are meaningless in this process - hence the
    // server-owned window below rather than anything derived from these two
    // values. Both are still read, never used, purely to keep the wire
    // reader in sync with what the client sent.
    (void)hinstance;
    (void)hwnd;
#else
    // hinstance/hwnd name a window in the Win32 world only; there is nothing
    // to translate them into here, since a Win32 client has no Wayland
    // surface of its own to hand over. Both are still read, never used,
    // purely to keep the wire reader in sync with what the client sent.
    (void)hinstance;
    (void)hwnd;
#endif
    if (!c.reader.ok()) {
        c.writer.u32(static_cast<uint32_t>(Status::Ok));
        c.writer.i32(VK_ERROR_INITIALIZATION_FAILED);
        c.writer.handle(0);
        return;
    }

    OwnWindow* window = c.server.create_own_window();
    if (!window) {
        fprintf(stderr,
                "server: vkCreateWin32SurfaceKHR: could not create a server-owned window\n");
        c.writer.u32(static_cast<uint32_t>(Status::Ok));
        c.writer.i32(VK_ERROR_INITIALIZATION_FAILED);
        c.writer.handle(0);
        return;
    }

    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
#if defined(_WIN32)
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = window->hinstance();
    info.hwnd = window->hwnd();
    const VkResult result = vkCreateWin32SurfaceKHR(c.server.instance(), &info, nullptr, &vk_surface);
#else
    VkWaylandSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = window->display();
    info.surface = window->surface();
    const VkResult result = vkCreateWaylandSurfaceKHR(c.server.instance(), &info, nullptr, &vk_surface);
#endif
    if (result == VK_SUCCESS) c.server.associate_surface(vk_surface, window);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.surfaces.add(vk_surface) : 0);
    c.reply();
}

void handle_DestroySurfaceKHR(Session& c) {
    const uint64_t id = c.reader.handle();
    VkSurfaceKHR surface = c.tables.surfaces.take(id);
    if (!c.reader.ok()) {
        mark_oneway_error(c);
        return;
    }
    if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(c.server.instance(), surface, nullptr);
}

#if !defined(_WIN32)
void handle_GetPhysicalDeviceWaylandPresentationSupportKHR(Session& c) {
    const uint64_t pd_id = c.reader.handle();
    VkPhysicalDevice physdev = c.server.physical_device_from_id(pd_id);
    const uint32_t family = c.reader.u32();
    if (!c.reader.ok() || physdev == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    // The application's own wl_display is meaningless here (see icd.cpp);
    // the real question - can this device present to *a* Wayland surface at
    // all - is answered against the proxy's own compositor connection, if
    // there is one.
    VkBool32 supported = VK_FALSE;
    WaylandProxy* wayland = c.server.wayland();
    if (wayland) {
        supported = vkGetPhysicalDeviceWaylandPresentationSupportKHR(physdev, family,
                                                                      wayland->display());
    }
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.u32(supported ? 1 : 0);
    c.reply();
}
#endif  // !defined(_WIN32)

void handle_GetPhysicalDeviceSurfaceSupportKHR(Session& c) {
    const uint64_t pd_id = c.reader.handle();
    VkPhysicalDevice physdev = c.server.physical_device_from_id(pd_id);
    const uint32_t family = c.reader.u32();
    const uint64_t surf_id = c.reader.handle();
    VkSurfaceKHR surface = c.tables.surface(surf_id);
    if (!c.reader.ok() || physdev == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkBool32 supported = VK_FALSE;
    const VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(physdev, family, surface, &supported);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(supported ? 1 : 0);
    c.reply();
}

void handle_GetPhysicalDeviceSurfaceCapabilitiesKHR(Session& c) {
    const uint64_t pd_id = c.reader.handle();
    VkPhysicalDevice physdev = c.server.physical_device_from_id(pd_id);
    const uint64_t surf_id = c.reader.handle();
    VkSurfaceKHR surface = c.tables.surface(surf_id);
    if (!c.reader.ok() || physdev == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkSurfaceCapabilitiesKHR caps{};
    const VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physdev, surface, &caps);

    // For a server-owned window, answer with the size the compositor gave
    // it. The driver reports currentExtent as 0xFFFFFFFF on Wayland - "the
    // client chooses" - which is true for a local application that knows how
    // big its own window is, and useless for a remote one that has no window
    // at all. Left alone, the client would keep picking its original size
    // and a resize could never take effect. Only the extent is overridden:
    // min/maxImageExtent and the rest still come from the driver.
    if (OwnWindow* window = c.server.window_for_surface(surface)) {
        const uint32_t w = window->width();
        const uint32_t h = window->height();
        if (w != 0 && h != 0) {
            caps.currentExtent.width = w;
            caps.currentExtent.height = h;
        }
    }

    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    remoting::write_SurfaceCapabilitiesKHR(c.writer, caps);
    c.reply();
}

void handle_GetPhysicalDeviceSurfaceFormatsKHR(Session& c) {
    const uint64_t pd_id = c.reader.handle();
    VkPhysicalDevice physdev = c.server.physical_device_from_id(pd_id);
    const uint64_t surf_id = c.reader.handle();
    VkSurfaceKHR surface = c.tables.surface(surf_id);
    if (!c.reader.ok() || physdev == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    uint32_t count = 0;
    VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(physdev, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (result == VK_SUCCESS && count > 0) {
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physdev, surface, &count, formats.data());
    }
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) remoting::write_SurfaceFormatKHR(c.writer, formats[i]);
    c.reply();
}

void handle_GetPhysicalDeviceSurfacePresentModesKHR(Session& c) {
    const uint64_t pd_id = c.reader.handle();
    VkPhysicalDevice physdev = c.server.physical_device_from_id(pd_id);
    const uint64_t surf_id = c.reader.handle();
    VkSurfaceKHR surface = c.tables.surface(surf_id);
    if (!c.reader.ok() || physdev == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    uint32_t count = 0;
    VkResult result = vkGetPhysicalDeviceSurfacePresentModesKHR(physdev, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    if (result == VK_SUCCESS && count > 0) {
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(physdev, surface, &count, modes.data());
    }
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) c.writer.i32(static_cast<int32_t>(modes[i]));
    c.reply();
}

void handle_CreateSwapchainKHR(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkSwapchainCreateInfoKHR info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_SwapchainCreateInfoKHR(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult result = vkCreateSwapchainKHR(device, &info, nullptr, &swapchain);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.swapchains.add(swapchain) : 0);
    c.reply();
}

void handle_DestroySwapchainKHR(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t swp_id = c.reader.handle();
    VkSwapchainKHR swapchain = c.tables.swapchains.take(swp_id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain, nullptr);
    c.tables.swapchain_image_ids.erase(swp_id);
}

void handle_GetSwapchainImagesKHR(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t swp_id = c.reader.handle();
    VkSwapchainKHR swapchain = c.tables.swapchain(swp_id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    uint32_t count = 0;
    VkResult result = vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    if (result == VK_SUCCESS && count > 0) {
        result = vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
    }

    // Idempotent on the real driver, but Table::add is not - see objects.hpp.
    auto& cached = c.tables.swapchain_image_ids[swp_id];
    if (cached.empty()) {
        for (VkImage img : images) cached.push_back(c.tables.images.add(img));
    }

    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) c.writer.handle(i < cached.size() ? cached[i] : 0);
    c.reply();
}

void handle_AcquireNextImageKHR(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t swp_id = c.reader.handle();
    VkSwapchainKHR swapchain = c.tables.swapchain(swp_id);
    const uint64_t timeout = c.reader.u64();
    const uint64_t sem_id = c.reader.handle();
    VkSemaphore semaphore = c.tables.semaphore(sem_id);
    const uint64_t fence_id = c.reader.handle();
    VkFence fence = c.tables.fence(fence_id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    uint32_t image_index = 0;
    VkResult result =
        vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, &image_index);

    // This is the only moment a resize of a server-owned window can be told
    // to the client. The window belongs to the server, so the compositor's
    // configure never reaches the application, and the driver has no reason
    // to report the swapchain out of date - as far as it is concerned
    // nothing changed. Reporting VK_SUBOPTIMAL_KHR is what makes a client
    // recreate its swapchain, at which point it asks for surface
    // capabilities again and gets the new extent. Only a success is
    // downgraded: a real error, or an out-of-date the driver raised itself,
    // already says at least as much and must not be weakened to advice.
    if (result == VK_SUCCESS && c.server.poll_windows_resized()) {
        result = VK_SUBOPTIMAL_KHR;
    }
    // Checked after the resize, and allowed to override it: a window the
    // user has closed is not merely the wrong size, and SUBOPTIMAL would
    // only send the client round the swapchain-recreation loop again
    // against a surface that is never coming back.
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        if (c.server.poll_windows_closed()) result = VK_ERROR_SURFACE_LOST_KHR;
    }

    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(image_index);
    c.reply();
}

void handle_QueuePresentKHR(Session& c) {
    const uint64_t queue_id = c.reader.handle();
    VkQueue queue = c.tables.queues.get(queue_id);
    const uint32_t wait_count = c.reader.u32();
    if (!count_fits(c.reader, wait_count, 8)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    std::vector<VkSemaphore> waits(wait_count);
    for (uint32_t i = 0; i < wait_count; ++i) waits[i] = c.tables.semaphore(c.reader.handle());
    const uint32_t swp_count = c.reader.u32();
    if (!count_fits(c.reader, swp_count, 12)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    std::vector<VkSwapchainKHR> swapchains(swp_count);
    std::vector<uint32_t> indices(swp_count);
    for (uint32_t i = 0; i < swp_count; ++i) {
        swapchains[i] = c.tables.swapchain(c.reader.handle());
        indices[i] = c.reader.u32();
    }
    if (!c.reader.ok() || queue == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    std::vector<VkResult> per_swapchain(swp_count, VK_SUCCESS);
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = wait_count;
    info.pWaitSemaphores = wait_count ? waits.data() : nullptr;
    info.swapchainCount = swp_count;
    info.pSwapchains = swp_count ? swapchains.data() : nullptr;
    info.pImageIndices = swp_count ? indices.data() : nullptr;
    info.pResults = swp_count ? per_swapchain.data() : nullptr;

    const VkResult result = vkQueuePresentKHR(queue, &info);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(swp_count);
    for (uint32_t i = 0; i < swp_count; ++i) c.writer.i32(static_cast<int32_t>(per_swapchain[i]));
    c.reply();
}


}  // namespace

#if !defined(_WIN32)
REGISTER_HANDLER(vkCreateWaylandSurfaceKHR, handle_CreateWaylandSurfaceKHR);
#endif
REGISTER_HANDLER(vkCreateWin32SurfaceKHR, handle_CreateWin32SurfaceKHR);
REGISTER_HANDLER(vkDestroySurfaceKHR, handle_DestroySurfaceKHR);
#if !defined(_WIN32)
REGISTER_HANDLER(vkGetPhysicalDeviceWaylandPresentationSupportKHR,
                  handle_GetPhysicalDeviceWaylandPresentationSupportKHR);
#endif
REGISTER_HANDLER(vkGetPhysicalDeviceSurfaceSupportKHR, handle_GetPhysicalDeviceSurfaceSupportKHR);
REGISTER_HANDLER(vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
                  handle_GetPhysicalDeviceSurfaceCapabilitiesKHR);
REGISTER_HANDLER(vkGetPhysicalDeviceSurfaceFormatsKHR, handle_GetPhysicalDeviceSurfaceFormatsKHR);
REGISTER_HANDLER(vkGetPhysicalDeviceSurfacePresentModesKHR,
                  handle_GetPhysicalDeviceSurfacePresentModesKHR);
REGISTER_HANDLER(vkCreateSwapchainKHR, handle_CreateSwapchainKHR);
REGISTER_HANDLER(vkDestroySwapchainKHR, handle_DestroySwapchainKHR);
REGISTER_HANDLER(vkGetSwapchainImagesKHR, handle_GetSwapchainImagesKHR);
REGISTER_HANDLER(vkAcquireNextImageKHR, handle_AcquireNextImageKHR);
REGISTER_HANDLER(vkQueuePresentKHR, handle_QueuePresentKHR);
