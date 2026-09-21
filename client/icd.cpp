// Client-side Vulkan ICD: a driver for a machine that has no GPU.
//
// Registers with the real Vulkan loader, so unmodified Vulkan programs drive
// it without knowing a socket is involved. Instance and physical-device
// queries are handled directly here; the whole device layer (vkCreateDevice
// onward) is implemented in icd_device.cpp using the RemoteInstance /
// RemoteDevice / RemoteQueue / RemoteCommandBuffer object model from
// remote_objects.hpp, and reached through vkGetDeviceProcAddr's table.
//
// WSI: this driver only ever knows about Wayland surfaces, and only because
// wayland/proxy_server.hpp's WaylandProxy on the server side can turn an
// application-side wl_surface object id into a real local wl_surface* on the
// machine that owns the GPU. A VkWaylandSurfaceCreateInfoKHR carries a
// wl_display*/wl_surface* that belong to the *application's* connection and
// are meaningless to send; only wl_proxy_get_id(pCreateInfo->surface)
// travels the wire, which is why this file links wayland-client at all.

#define VK_USE_PLATFORM_WAYLAND_KHR

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <wayland-client.h>

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "marshal.hpp"
#include "remote_objects.hpp"
#include "remoting_commands.inl"
#include "wire.hpp"

namespace {

using remoting::Opcode;
using remoting::Reader;
using remoting::RemoteInstance;
using remoting::RemotePhysicalDevice;
using remoting::Writer;

RemoteInstance* to_instance(VkInstance instance) {
    return reinterpret_cast<RemoteInstance*>(instance);
}

RemotePhysicalDevice* to_physical_device(VkPhysicalDevice device) {
    return reinterpret_cast<RemotePhysicalDevice*>(device);
}

bool round_trip(RemoteInstance* instance, Opcode opcode, const Writer& request,
                std::vector<char>* reply) {
    return instance->connection.round_trip(opcode, request, reply);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char* /*layer*/,
                                                                    uint32_t* count,
                                                                    VkExtensionProperties* props) {
    // The only WSI platform this driver ever offers is Wayland, because that
    // is the only one wayland/proxy_server.hpp's WaylandProxy can name a
    // window on.
    static const char* const kNames[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
    static const uint32_t kVersions[] = {VK_KHR_SURFACE_SPEC_VERSION,
                                        VK_KHR_WAYLAND_SURFACE_SPEC_VERSION};
    constexpr uint32_t kCount = 2;

    if (props == nullptr) {
        *count = kCount;
        return VK_SUCCESS;
    }
    const uint32_t to_write = *count < kCount ? *count : kCount;
    for (uint32_t i = 0; i < to_write; ++i) {
        memset(&props[i], 0, sizeof(props[i]));
        strncpy(props[i].extensionName, kNames[i], VK_MAX_EXTENSION_NAME_SIZE - 1);
        props[i].specVersion = kVersions[i];
    }
    *count = to_write;
    return to_write < kCount ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo*,
                                              const VkAllocationCallbacks*,
                                              VkInstance* pInstance) {
    const char* host = getenv("VK_REMOTING_HOST");
    if (host == nullptr || *host == '\0') host = "127.0.0.1";

    uint16_t port = 24680;
    if (const char* port_text = getenv("VK_REMOTING_PORT")) {
        const long parsed = strtol(port_text, nullptr, 10);
        if (parsed > 0 && parsed <= 65535) port = static_cast<uint16_t>(parsed);
    }

    auto* instance = new RemoteInstance();
    // Required of every dispatchable handle an ICD creates.
    set_loader_magic_value(instance);

    instance->connection.fd = remoting::connect_to(host, port);
    if (instance->connection.fd < 0) {
        fprintf(stderr, "vulkan-remoting: no server at %s:%u\n", host, static_cast<unsigned>(port));
        delete instance;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    {
        Writer request;
        request.string(remoting::kCommandSetDigest);
        std::vector<char> reply;
        if (!round_trip(instance, Opcode::Handshake, request, &reply)) {
            fprintf(stderr,
                    "vulkan-remoting: handshake refused; server was generated from a different "
                    "vk.xml (ours is %s)\n",
                    remoting::kCommandSetDigest);
            ::close(instance->connection.fd);
            delete instance;
            return VK_ERROR_INCOMPATIBLE_DRIVER;
        }
    }

    *pInstance = reinterpret_cast<VkInstance>(instance);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance handle, const VkAllocationCallbacks*) {
    if (handle == VK_NULL_HANDLE) return;
    RemoteInstance* instance = to_instance(handle);
    if (instance->connection.fd >= 0) ::close(instance->connection.fd);
    for (auto* device : instance->physical_devices) delete device;
    delete instance;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance handle, uint32_t* pCount,
                                                        VkPhysicalDevice* pDevices) {
    RemoteInstance* instance = to_instance(handle);

    if (instance->physical_devices.empty()) {
        Writer request;
        std::vector<char> reply;
        if (!round_trip(instance, Opcode::vkEnumeratePhysicalDevices, request, &reply)) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        Reader reader(reply.data(), reply.size());
        reader.u32();  // status
        reader.i32();  // remote VkResult
        const uint32_t count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t id = reader.handle();
            if (!reader.ok()) return VK_ERROR_INITIALIZATION_FAILED;
            auto* device = new RemotePhysicalDevice();
            set_loader_magic_value(device);
            device->instance = instance;
            device->remote_id = id;
            instance->physical_devices.push_back(device);
        }
    }

    const uint32_t available = static_cast<uint32_t>(instance->physical_devices.size());
    if (pDevices == nullptr) {
        *pCount = available;
        return VK_SUCCESS;
    }

    const uint32_t to_write = *pCount < available ? *pCount : available;
    for (uint32_t i = 0; i < to_write; ++i) {
        pDevices[i] = reinterpret_cast<VkPhysicalDevice>(instance->physical_devices[i]);
    }
    *pCount = to_write;
    return to_write < available ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice handle,
                                                       VkPhysicalDeviceProperties* pProperties) {
    memset(pProperties, 0, sizeof(*pProperties));

    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceProperties, request, &reply)) {
        return;
    }

    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    std::vector<char> raw;
    if (!reader.bytes(&raw) || raw.size() != sizeof(*pProperties)) return;
    memcpy(pProperties, raw.data(), sizeof(*pProperties));

    // Report 1.0 regardless of what the remote device supports. Advertising
    // the remote 1.4 would invite the application to call 1.1+ entry points
    // this driver does not implement, and it would then index results that
    // were never filled in. A driver must not claim a version it cannot serve.
    pProperties->apiVersion = VK_API_VERSION_1_0;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice handle,
                                                     VkPhysicalDeviceFeatures* pFeatures) {
    memset(pFeatures, 0, sizeof(*pFeatures));

    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceFeatures, request, &reply)) {
        return;
    }

    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    std::vector<char> raw;
    if (reader.bytes(&raw) && raw.size() == sizeof(*pFeatures)) {
        memcpy(pFeatures, raw.data(), sizeof(*pFeatures));
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice handle, VkPhysicalDeviceMemoryProperties* pProperties) {
    memset(pProperties, 0, sizeof(*pProperties));

    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceMemoryProperties, request,
                    &reply)) {
        return;
    }

    Reader reader(reply.data(), reply.size());
    reader.u32();  // status

    const uint32_t type_count = reader.u32();
    pProperties->memoryTypeCount = type_count > VK_MAX_MEMORY_TYPES ? VK_MAX_MEMORY_TYPES : type_count;
    for (uint32_t i = 0; i < type_count; ++i) {
        const uint32_t flags = reader.u32();
        const uint32_t heap = reader.u32();
        if (i < pProperties->memoryTypeCount) {
            pProperties->memoryTypes[i].propertyFlags = flags;
            pProperties->memoryTypes[i].heapIndex = heap;
        }
    }

    const uint32_t heap_count = reader.u32();
    pProperties->memoryHeapCount = heap_count > VK_MAX_MEMORY_HEAPS ? VK_MAX_MEMORY_HEAPS : heap_count;
    for (uint32_t i = 0; i < heap_count; ++i) {
        const uint64_t size = reader.u64();
        const uint32_t flags = reader.u32();
        if (i < pProperties->memoryHeapCount) {
            pProperties->memoryHeaps[i].size = size;
            pProperties->memoryHeaps[i].flags = flags;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice handle, uint32_t* pCount, VkQueueFamilyProperties* pProperties) {
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceQueueFamilyProperties, request,
                    &reply)) {
        *pCount = 0;
        return;
    }

    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    const uint32_t count = reader.u32();

    std::vector<VkQueueFamilyProperties> families(count);
    for (uint32_t i = 0; i < count; ++i) {
        families[i].queueFlags = reader.u32();
        families[i].queueCount = reader.u32();
        families[i].timestampValidBits = reader.u32();
        families[i].minImageTransferGranularity.width = reader.u32();
        families[i].minImageTransferGranularity.height = reader.u32();
        families[i].minImageTransferGranularity.depth = reader.u32();
    }
    if (!reader.ok()) {
        *pCount = 0;
        return;
    }

    if (pProperties == nullptr) {
        *pCount = count;
        return;
    }
    const uint32_t to_write = *pCount < count ? *pCount : count;
    for (uint32_t i = 0; i < to_write; ++i) pProperties[i] = families[i];
    *pCount = to_write;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties(VkPhysicalDevice handle,
                                                             VkFormat format,
                                                             VkFormatProperties* pProperties) {
    memset(pProperties, 0, sizeof(*pProperties));

    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.i32(static_cast<int32_t>(format));
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceFormatProperties, request,
                    &reply)) {
        return;
    }

    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    std::vector<char> raw;
    if (reader.bytes(&raw) && raw.size() == sizeof(*pProperties)) {
        memcpy(pProperties, raw.data(), sizeof(*pProperties));
    }
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice handle, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* pProperties) {
    memset(pProperties, 0, sizeof(*pProperties));

    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.i32(static_cast<int32_t>(format));
    request.i32(static_cast<int32_t>(type));
    request.i32(static_cast<int32_t>(tiling));
    request.u32(static_cast<uint32_t>(usage));
    request.u32(static_cast<uint32_t>(flags));
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceImageFormatProperties, request,
                    &reply)) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    const VkResult result = static_cast<VkResult>(reader.i32());
    std::vector<char> raw;
    if (reader.bytes(&raw) && raw.size() == sizeof(*pProperties)) {
        memcpy(pProperties, raw.data(), sizeof(*pProperties));
    }
    if (!reader.ok()) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return result;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits, VkImageUsageFlags,
    VkImageTiling, uint32_t* pCount, VkSparseImageFormatProperties*) {
    *pCount = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice handle,
                                                                  const char* /*layer*/,
                                                                  uint32_t* pCount,
                                                                  VkExtensionProperties* pProps) {
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkEnumerateDeviceExtensionProperties, request,
                    &reply)) {
        *pCount = 0;
        return VK_SUCCESS;
    }

    Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    reader.i32();  // remote VkResult
    const uint32_t count = reader.u32();
    std::vector<VkExtensionProperties> exts(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<char> raw;
        if (!reader.bytes(&raw) || raw.size() != sizeof(VkExtensionProperties)) {
            *pCount = 0;
            return VK_SUCCESS;
        }
        memcpy(&exts[i], raw.data(), sizeof(VkExtensionProperties));
    }

    if (pProps == nullptr) {
        *pCount = count;
        return VK_SUCCESS;
    }
    const uint32_t to_write = *pCount < count ? *pCount : count;
    for (uint32_t i = 0; i < to_write; ++i) pProps[i] = exts[i];
    *pCount = to_write;
    return to_write < count ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pCount,
                                                              VkLayerProperties*) {
    *pCount = 0;
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// WSI: Wayland surfaces and surface queries.
// ---------------------------------------------------------------------------

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
    *pSurface = remoting::handle_from_id<VkSurfaceKHR>(id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance handle, VkSurfaceKHR surface,
                                             const VkAllocationCallbacks*) {
    if (surface == VK_NULL_HANDLE) return;
    RemoteInstance* instance = to_instance(handle);
    Writer request;
    request.handle(remoting::id_from_handle(surface));
    instance->connection.send_oneway(Opcode::vkDestroySurfaceKHR, request);
}

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

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice handle,
                                                                  uint32_t queueFamilyIndex,
                                                                  VkSurfaceKHR surface,
                                                                  VkBool32* pSupported) {
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.u32(queueFamilyIndex);
    request.handle(remoting::id_from_handle(surface));
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
    request.handle(remoting::id_from_handle(surface));
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
    request.handle(remoting::id_from_handle(surface));
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
    std::vector<VkSurfaceFormatKHR> formats(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<char> raw;
        if (!reader.bytes(&raw) || raw.size() != sizeof(VkSurfaceFormatKHR)) {
            *pCount = 0;
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        memcpy(&formats[i], raw.data(), sizeof(VkSurfaceFormatKHR));
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
    request.handle(remoting::id_from_handle(surface));
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

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice, const char* pName) {
    if (pName == nullptr) return nullptr;
    size_t count = 0;
    const remoting::DeviceEntry* entries = remoting::get_device_entries(&count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, pName) == 0) return entries[i].function;
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char* pName);

struct Entry {
    const char* name;
    PFN_vkVoidFunction function;
};

#define ENTRY(name) \
    { "vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name) }

const Entry kEntries[] = {
    ENTRY(CreateInstance),
    ENTRY(DestroyInstance),
    ENTRY(EnumerateInstanceExtensionProperties),
    ENTRY(EnumeratePhysicalDevices),
    ENTRY(GetPhysicalDeviceProperties),
    ENTRY(GetPhysicalDeviceFeatures),
    ENTRY(GetPhysicalDeviceMemoryProperties),
    ENTRY(GetPhysicalDeviceQueueFamilyProperties),
    ENTRY(GetPhysicalDeviceFormatProperties),
    ENTRY(GetPhysicalDeviceImageFormatProperties),
    ENTRY(GetPhysicalDeviceSparseImageFormatProperties),
    ENTRY(EnumerateDeviceExtensionProperties),
    ENTRY(EnumerateDeviceLayerProperties),
    ENTRY(CreateWaylandSurfaceKHR),
    ENTRY(DestroySurfaceKHR),
    ENTRY(GetPhysicalDeviceWaylandPresentationSupportKHR),
    ENTRY(GetPhysicalDeviceSurfaceSupportKHR),
    ENTRY(GetPhysicalDeviceSurfaceCapabilitiesKHR),
    ENTRY(GetPhysicalDeviceSurfaceFormatsKHR),
    ENTRY(GetPhysicalDeviceSurfacePresentModesKHR),
    ENTRY(GetDeviceProcAddr),
    ENTRY(GetInstanceProcAddr),
};

#undef ENTRY

PFN_vkVoidFunction lookup(const char* name) {
    if (name == nullptr) return nullptr;
    for (const Entry& entry : kEntries) {
        if (strcmp(entry.name, name) == 0) return entry.function;
    }
    // Device-level functions are reachable through vkGetInstanceProcAddr too:
    // some callers resolve them that way before they have a VkDevice, and the
    // loader tolerates a driver answering either way.
    size_t count = 0;
    const remoting::DeviceEntry* entries = remoting::get_device_entries(&count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) return entries[i].function;
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char* pName) {
    return lookup(pName);
}

}  // namespace

extern "C" {

__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion) {
    // Version 5 is the highest this driver implements; the loader lowers its
    // own expectation to whatever is written back here.
    if (*pVersion > 5) *pVersion = 5;
    return VK_SUCCESS;
}

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance, const char* pName) {
    return lookup(pName);
}

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance, const char* pName) {
    return lookup(pName);
}

}  // extern "C"
