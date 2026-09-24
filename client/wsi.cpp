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

#include <cstring>

// windows.h's default (non-lean) mode drags in the legacy winsock.h, which
// conflicts with wire.hpp's winsock2.h if windows.h is reached first in this
// translation unit (as it is here, via vulkan.h's VK_USE_PLATFORM_WIN32_KHR
// path below); defining this before that include keeps them from conflicting
// regardless of which of the two ends up included first.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
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
    Writer request;
    request.handle(id_from_handle(surface));
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
    if (result == VK_SUCCESS) *pSwapchain = handle_from_id<VkSwapchainKHR>(id);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice handle, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks*) {
    if (swapchain == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(swapchain));
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
