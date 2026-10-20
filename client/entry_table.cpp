// The only file that does string comparison / lookup for the client-side
// ICD's name -> function tables.
//
// Every other client file only ever exposes a static table of its own
// {name, function pointer} pairs (see entry_table.hpp); this file concatenates
// them, once, into the two tables the loader actually walks:
// GetInstanceProcAddr's (covering instance and physical-device level
// functions) and GetDeviceProcAddr's (covering everything reachable once a
// VkDevice exists). Concatenation order does not affect behaviour - lookup is
// a linear scan by name, and no two providers define the same name - so the
// grouping below simply follows the file split rather than the original
// single-table order.

#include <cstring>
#include <vector>

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <vulkan/vulkan.h>

#include "entry_table.hpp"
#include "extension_contract.hpp"
#include "remote_objects.hpp"

namespace remoting {
namespace {

void append(std::vector<DeviceEntry>* out, const DeviceEntry* entries, size_t count) {
    out->insert(out->end(), entries, entries + count);
}

}  // namespace

bool is_swapchain_command(const char* name) {
    return strcmp(name, "vkCreateSwapchainKHR") == 0 ||
           strcmp(name, "vkDestroySwapchainKHR") == 0 ||
           strcmp(name, "vkGetSwapchainImagesKHR") == 0 ||
           strcmp(name, "vkAcquireNextImageKHR") == 0 ||
           strcmp(name, "vkQueuePresentKHR") == 0;
}

uint32_t core_version_for_command(const char* name) {
    static const char* const v13[] = {
        "vkCmdPipelineBarrier2","vkCmdResetEvent2","vkCmdSetEvent2",
        "vkCmdWaitEvents2","vkCmdWriteTimestamp2","vkQueueSubmit2"};
    static const char* const v12[] = {
        "vkCmdBeginRenderPass2","vkCmdDrawIndexedIndirectCount","vkCmdDrawIndirectCount",
        "vkCmdEndRenderPass2","vkCmdNextSubpass2","vkCreateRenderPass2",
        "vkGetBufferDeviceAddress","vkGetBufferOpaqueCaptureAddress",
        "vkGetDeviceMemoryOpaqueCaptureAddress","vkGetSemaphoreCounterValue",
        "vkResetQueryPool","vkSignalSemaphore","vkWaitSemaphores"};
    static const char* const v11[] = {
        "vkBindBufferMemory2","vkBindImageMemory2","vkCmdDispatchBase","vkCmdSetDeviceMask",
        "vkCreateDescriptorUpdateTemplate","vkCreateSamplerYcbcrConversion",
        "vkDestroyDescriptorUpdateTemplate","vkDestroySamplerYcbcrConversion",
        "vkGetBufferMemoryRequirements2","vkGetDescriptorSetLayoutSupport",
        "vkGetDeviceGroupPeerMemoryFeatures","vkGetDeviceQueue2","vkGetImageMemoryRequirements2",
        "vkGetImageSparseMemoryRequirements2","vkTrimCommandPool","vkUpdateDescriptorSetWithTemplate"};
    for (const char* command : v13) if (strcmp(name, command) == 0) return VK_API_VERSION_1_3;
    for (const char* command : v12) if (strcmp(name, command) == 0) return VK_API_VERSION_1_2;
    for (const char* command : v11) if (strcmp(name, command) == 0) return VK_API_VERSION_1_1;
    return VK_API_VERSION_1_0;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice handle, const char* pName) {
    if (pName == nullptr || handle == VK_NULL_HANDLE) return nullptr;
    auto* device = reinterpret_cast<RemoteDevice*>(handle);
    if (core_version_for_command(pName) > device->api_version) return nullptr;
    if (is_swapchain_command(pName) &&
        device->enabled_extensions.count(VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
        return nullptr;
    }
    if (const char* extension = device_extension_for_command(pName);
        extension != nullptr && device->enabled_extensions.count(extension) == 0) {
        return nullptr;
    }
    size_t count = 0;
    const DeviceEntry* entries = get_device_entries(&count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, pName) == 0) return entries[i].function;
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char* pName);

namespace {

#define ENTRY(name) \
    { "vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name) }

const std::vector<DeviceEntry>& instance_entries() {
    static const std::vector<DeviceEntry> table = [] {
        std::vector<DeviceEntry> v;
        size_t count = 0;
        const DeviceEntry* entries = get_icd_core_entries(&count);
        append(&v, entries, count);
        entries = get_physical_device_entries(&count);
        append(&v, entries, count);
        entries = get_wsi_instance_entries(&count);
        append(&v, entries, count);
        v.push_back(ENTRY(GetDeviceProcAddr));
        v.push_back(ENTRY(GetInstanceProcAddr));
        return v;
    }();
    return table;
}

#undef ENTRY

}  // namespace

PFN_vkVoidFunction lookup(const char* name) {
    if (name == nullptr) return nullptr;
    for (const DeviceEntry& entry : instance_entries()) {
        if (strcmp(entry.name, name) == 0) return entry.function;
    }
    // Device-level functions are reachable through vkGetInstanceProcAddr too:
    // some callers resolve them that way before they have a VkDevice, and the
    // loader tolerates a driver answering either way.
    size_t count = 0;
    const DeviceEntry* entries = get_device_entries(&count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) return entries[i].function;
    }
    return nullptr;
}

PFN_vkVoidFunction lookup_instance(VkInstance handle, const char* name) {
    if (name == nullptr) return nullptr;
    PFN_vkVoidFunction function = lookup(name);
    if (!function || handle == VK_NULL_HANDLE) return function;
    auto* instance = reinterpret_cast<RemoteInstance*>(handle);
    if (core_version_for_command(name) > instance->negotiated_api_version &&
        std::strstr(name, "KHR") == nullptr && std::strstr(name, "EXT") == nullptr) return nullptr;
    if (is_properties2_command(name) && std::strstr(name, "KHR") != nullptr &&
        instance->enabled_extensions.count(
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) return nullptr;
#if defined(_WIN32)
    if (strcmp(name, "vkCreateWin32SurfaceKHR") == 0 &&
        instance->enabled_extensions.count(VK_KHR_WIN32_SURFACE_EXTENSION_NAME) == 0) return nullptr;
#else
    if (strcmp(name, "vkCreateWaylandSurfaceKHR") == 0 &&
        instance->enabled_extensions.count(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) == 0) return nullptr;
#endif
    if ((strncmp(name, "vkGetPhysicalDeviceSurface", 26) == 0 ||
         strcmp(name, "vkDestroySurfaceKHR") == 0) &&
        instance->enabled_extensions.count(VK_KHR_SURFACE_EXTENSION_NAME) == 0) return nullptr;
    return function;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance,
                                                              const char* pName) {
    return lookup_instance(instance, pName);
}

const DeviceEntry* get_device_entries(size_t* count) {
    static const std::vector<DeviceEntry> table = [] {
        std::vector<DeviceEntry> v;
        size_t n = 0;
        const DeviceEntry* entries = get_device_core_entries(&n);
        append(&v, entries, n);
        entries = get_memory_entries(&n);
        append(&v, entries, n);
        entries = get_command_entries(&n);
        append(&v, entries, n);
        entries = get_wsi_device_entries(&n);
        append(&v, entries, n);
        return v;
    }();
    *count = table.size();
    return table.data();
}

}  // namespace remoting
