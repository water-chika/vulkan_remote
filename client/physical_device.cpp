// Client-side physical-device queries: everything reachable from a
// VkPhysicalDevice before a VkDevice exists. See icd.cpp for instance
// creation and entry_table.cpp for how these get found by name.

#include <string.h>

#include <cstring>

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
using remoting::RemoteInstance;
using remoting::RemotePhysicalDevice;
using remoting::Writer;

RemotePhysicalDevice* to_physical_device(VkPhysicalDevice device) {
    return reinterpret_cast<RemotePhysicalDevice*>(device);
}

bool round_trip(RemoteInstance* instance, Opcode opcode, const Writer& request,
                std::vector<char>* reply) {
    return instance->connection.round_trip(opcode, request, reply);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance handle, uint32_t* pCount,
                                                        VkPhysicalDevice* pDevices) {
    RemoteInstance* instance = reinterpret_cast<RemoteInstance*>(handle);

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

}  // namespace

namespace remoting {

const DeviceEntry* get_physical_device_entries(size_t* count) {
    static const DeviceEntry kEntries[] = {
#define D(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name)}
        D(EnumeratePhysicalDevices),
        D(GetPhysicalDeviceProperties),
        D(GetPhysicalDeviceFeatures),
        D(GetPhysicalDeviceMemoryProperties),
        D(GetPhysicalDeviceQueueFamilyProperties),
        D(GetPhysicalDeviceFormatProperties),
        D(GetPhysicalDeviceImageFormatProperties),
        D(GetPhysicalDeviceSparseImageFormatProperties),
        D(EnumerateDeviceExtensionProperties),
        D(EnumerateDeviceLayerProperties),
#undef D
    };
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

}  // namespace remoting
