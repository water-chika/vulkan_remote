// Server-side WSI handlers: Wayland surface creation/destruction, every
// vkGetPhysicalDeviceSurface*KHR query, and swapchain create/destroy/
// get-images/acquire/present. See wayland/proxy_server.hpp for the embedded
// compositor proxy these surface calls talk to.

#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include "marshal.hpp"
#include "proxy_server.hpp"
#include "session.hpp"

namespace {

using remoting::Arena;
using remoting::Session;
using remoting::Status;
using remoting::mark_oneway_error;

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

void handle_DestroySurfaceKHR(Session& c) {
    const uint64_t id = c.reader.handle();
    VkSurfaceKHR surface = c.tables.surfaces.take(id);
    if (!c.reader.ok()) {
        mark_oneway_error(c);
        return;
    }
    if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(c.server.instance(), surface, nullptr);
}

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
    const VkResult result =
        vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, &image_index);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(image_index);
    c.reply();
}

void handle_QueuePresentKHR(Session& c) {
    const uint64_t queue_id = c.reader.handle();
    VkQueue queue = c.tables.queues.get(queue_id);
    const uint32_t wait_count = c.reader.u32();
    std::vector<VkSemaphore> waits(wait_count);
    for (uint32_t i = 0; i < wait_count; ++i) waits[i] = c.tables.semaphore(c.reader.handle());
    const uint32_t swp_count = c.reader.u32();
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

REGISTER_HANDLER(vkCreateWaylandSurfaceKHR, handle_CreateWaylandSurfaceKHR);
REGISTER_HANDLER(vkDestroySurfaceKHR, handle_DestroySurfaceKHR);
REGISTER_HANDLER(vkGetPhysicalDeviceWaylandPresentationSupportKHR,
                  handle_GetPhysicalDeviceWaylandPresentationSupportKHR);
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
