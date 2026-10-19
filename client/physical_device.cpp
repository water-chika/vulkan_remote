// Client-side physical-device queries: everything reachable from a
// VkPhysicalDevice before a VkDevice exists. See icd.cpp for instance
// creation and entry_table.cpp for how these get found by name.

#include <string.h>

#include <algorithm>
#include <string>

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "entry_table.hpp"
#include "extension_contract.hpp"
#include "marshal.hpp"
#include "memory_contract.hpp"
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
            device->api_version = instance->negotiated_api_version;
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
    if (reader.u32() != static_cast<uint32_t>(remoting::Status::Ok) ||
        !remoting::read_PhysicalDeviceProperties(reader, pProperties)) {
        memset(pProperties, 0, sizeof(*pProperties));
        return;
    }

    // The remoting implementation contract stops at Vulkan 1.2 even when the
    // real device is newer. Preserve a genuinely older device's lower version.
    pProperties->apiVersion = (std::min)(pProperties->apiVersion, VK_API_VERSION_1_2);
    device->api_version = (std::min)(device->instance->negotiated_api_version,
                                     pProperties->apiVersion);
    pProperties->apiVersion = device->api_version;
    device->map_alignment = pProperties->limits.minMemoryMapAlignment;
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
        // Sparse binding requires queue submission plus residency semantics that
        // are not yet covered by the deterministic acceptance suite. Keep the
        // core entry points callable, but do not invite applications onto that
        // optional path until the complete contract is proven.
        pFeatures->sparseBinding = VK_FALSE;
        pFeatures->sparseResidencyBuffer = VK_FALSE;
        pFeatures->sparseResidencyImage2D = VK_FALSE;
        pFeatures->sparseResidencyImage3D = VK_FALSE;
        pFeatures->sparseResidency2Samples = VK_FALSE;
        pFeatures->sparseResidency4Samples = VK_FALSE;
        pFeatures->sparseResidency8Samples = VK_FALSE;
        pFeatures->sparseResidency16Samples = VK_FALSE;
        pFeatures->sparseResidencyAliased = VK_FALSE;
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
    device->memory_type_flags.assign(pProperties->memoryTypeCount, 0);
    for (uint32_t i = 0; i < type_count; ++i) {
        const VkMemoryPropertyFlags flags = remoting::public_memory_property_flags(
            reader.u32(), device->instance->capabilities);
        const uint32_t heap = reader.u32();
        if (i < pProperties->memoryTypeCount) {
            pProperties->memoryTypes[i].propertyFlags = flags;
            pProperties->memoryTypes[i].heapIndex = heap;
            device->memory_type_flags[i] = flags;
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
    VkPhysicalDevice handle, VkFormat format, VkImageType type, VkSampleCountFlagBits samples,
    VkImageUsageFlags usage, VkImageTiling tiling, uint32_t* pCount,
    VkSparseImageFormatProperties* pProperties) {
    if (!pCount) return;
    RemotePhysicalDevice* device = to_physical_device(handle); Writer request;
    request.handle(device->remote_id); request.i32(static_cast<int32_t>(format));
    request.i32(static_cast<int32_t>(type)); request.i32(static_cast<int32_t>(samples));
    request.u32(usage); request.i32(static_cast<int32_t>(tiling));
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceSparseImageFormatProperties,
                    request, &reply)) { *pCount = 0; return; }
    Reader reader(reply.data(), reply.size()); reader.u32(); const uint32_t count = reader.u32();
    if (count > remoting::kMaxArrayElements || count > reader.remaining() / 20) {
        *pCount = 0; return;
    }
    std::vector<VkSparseImageFormatProperties> values(count);
    for (uint32_t i = 0; i < count; ++i)
        if (!read_SparseImageFormatProperties(reader, &values[i])) { *pCount = 0; return; }
    if (!pProperties) { *pCount = count; return; }
    const uint32_t n = *pCount < count ? *pCount : count;
    for (uint32_t i = 0; i < n; ++i) pProperties[i] = values[i]; *pCount = n;
}

// The properties2 wire format currently carries the base structures only. Keep
// every caller-owned pNext chain intact so the bounded chain codec can fill it
// in without changing these command envelopes when it lands.
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice handle,
                                                       VkPhysicalDeviceFeatures2* pFeatures) {
    if (pFeatures == nullptr) return;
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    if (!write_pnext_output_topology(request, remoting::PNextContext::QueryFeaturesOutput,
                                     pFeatures->pNext)) return;
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceFeatures2, request, &reply)) return;
    Reader reader(reply.data(), reply.size());
    if (reader.u32() == static_cast<uint32_t>(remoting::Status::Ok) &&
        read_PhysicalDeviceFeatures(reader, &pFeatures->features)) {
        pFeatures->features.sparseBinding = VK_FALSE;
        pFeatures->features.sparseResidencyBuffer = VK_FALSE;
        pFeatures->features.sparseResidencyImage2D = VK_FALSE;
        pFeatures->features.sparseResidencyImage3D = VK_FALSE;
        pFeatures->features.sparseResidency2Samples = VK_FALSE;
        pFeatures->features.sparseResidency4Samples = VK_FALSE;
        pFeatures->features.sparseResidency8Samples = VK_FALSE;
        pFeatures->features.sparseResidency16Samples = VK_FALSE;
        pFeatures->features.sparseResidencyAliased = VK_FALSE;
        (void)apply_pnext_output_values(reader, remoting::PNextContext::QueryFeaturesOutput,
                                        pFeatures->pNext);
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(
    VkPhysicalDevice handle, VkPhysicalDeviceProperties2* pProperties) {
    if (pProperties == nullptr) return;
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    if (!write_pnext_output_topology(request, remoting::PNextContext::QueryPropertiesOutput,
                                     pProperties->pNext)) return;
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceProperties2, request, &reply)) return;
    Reader reader(reply.data(), reply.size());
    VkPhysicalDeviceProperties properties{};
    if (reader.u32() == static_cast<uint32_t>(remoting::Status::Ok) &&
        remoting::read_PhysicalDeviceProperties(reader, &properties) &&
        apply_pnext_output_values(reader, remoting::PNextContext::QueryPropertiesOutput,
                                  pProperties->pNext)) {
        properties.apiVersion = (std::min)(properties.apiVersion, VK_API_VERSION_1_2);
        device->api_version = (std::min)(device->instance->negotiated_api_version,
                                         properties.apiVersion);
        properties.apiVersion = device->api_version;
        pProperties->properties = properties;
        device->map_alignment = properties.limits.minMemoryMapAlignment;
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice handle, VkFormat format, VkFormatProperties2* pProperties) {
    if (pProperties == nullptr || pProperties->pNext != nullptr) return;
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.i32(static_cast<int32_t>(format));
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceFormatProperties2, request, &reply)) return;
    Reader reader(reply.data(), reply.size());
    if (reader.u32() == static_cast<uint32_t>(remoting::Status::Ok))
        (void)read_FormatProperties(reader, &pProperties->formatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice handle, const VkPhysicalDeviceImageFormatInfo2* pInfo,
    VkImageFormatProperties2* pProperties) {
    if (pInfo == nullptr || pProperties == nullptr) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.i32(static_cast<int32_t>(pInfo->format));
    request.i32(static_cast<int32_t>(pInfo->type));
    request.i32(static_cast<int32_t>(pInfo->tiling));
    request.u32(pInfo->usage);
    request.u32(pInfo->flags);
    if (!write_pnext_chain(request, remoting::PNextContext::ImageCreate, pInfo->pNext) ||
        !write_pnext_output_topology(request, remoting::PNextContext::ImageFormatPropertiesOutput,
                                     pProperties->pNext))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceImageFormatProperties2, request,
                    &reply)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    Reader reader(reply.data(), reply.size());
    if (reader.u32() != static_cast<uint32_t>(remoting::Status::Ok))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const VkResult result = static_cast<VkResult>(reader.i32());
    if (!read_ImageFormatProperties(reader, &pProperties->imageFormatProperties) ||
        !apply_pnext_output_values(reader, remoting::PNextContext::ImageFormatPropertiesOutput,
                                   pProperties->pNext))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return result;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice handle, uint32_t* pCount, VkQueueFamilyProperties2* pProperties) {
    if (pCount == nullptr) return;
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceQueueFamilyProperties2, request,
                    &reply)) {
        *pCount = 0;
        return;
    }
    Reader reader(reply.data(), reply.size());
    if (reader.u32() != static_cast<uint32_t>(remoting::Status::Ok)) {
        *pCount = 0;
        return;
    }
    const uint32_t count = reader.u32();
    if (count > remoting::kMaxArrayElements || count > reader.remaining() / (3 * sizeof(uint32_t) + sizeof(VkExtent3D))) {
        *pCount = 0;
        return;
    }
    std::vector<VkQueueFamilyProperties> values(count);
    for (uint32_t i = 0; i < count; ++i)
        if (!read_QueueFamilyProperties(reader, &values[i])) {
            *pCount = 0;
            return;
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
    for (uint32_t i = 0; i < to_write; ++i) pProperties[i].queueFamilyProperties = values[i];
    *pCount = to_write;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice handle, VkPhysicalDeviceMemoryProperties2* pProperties) {
    if (pProperties == nullptr) return;
    VkPhysicalDeviceMemoryProperties& out = pProperties->memoryProperties;
    memset(&out, 0, sizeof(out));
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceMemoryProperties2, request, &reply)) return;
    Reader reader(reply.data(), reply.size());
    if (reader.u32() != static_cast<uint32_t>(remoting::Status::Ok) ||
        !read_PhysicalDeviceMemoryProperties(reader, &out)) return;
    device->memory_type_flags.resize(out.memoryTypeCount);
    for (uint32_t i = 0; i < out.memoryTypeCount; ++i) {
        out.memoryTypes[i].propertyFlags = remoting::public_memory_property_flags(
            out.memoryTypes[i].propertyFlags, device->instance->capabilities);
        device->memory_type_flags[i] = out.memoryTypes[i].propertyFlags;
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice handle, const VkPhysicalDeviceSparseImageFormatInfo2* pInfo,
    uint32_t* pCount, VkSparseImageFormatProperties2* pProperties) {
    if (pInfo == nullptr || pCount == nullptr) return;
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.i32(static_cast<int32_t>(pInfo->format));
    request.i32(static_cast<int32_t>(pInfo->type));
    request.i32(static_cast<int32_t>(pInfo->samples));
    request.u32(pInfo->usage);
    request.i32(static_cast<int32_t>(pInfo->tiling));
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceSparseImageFormatProperties2,
                    request, &reply)) {
        *pCount = 0;
        return;
    }
    Reader reader(reply.data(), reply.size());
    if (reader.u32() != static_cast<uint32_t>(remoting::Status::Ok)) {
        *pCount = 0;
        return;
    }
    const uint32_t count = reader.u32();
    if (count > remoting::kMaxArrayElements || count > reader.remaining() / 20) {
        *pCount = 0;
        return;
    }
    std::vector<VkSparseImageFormatProperties> values(count);
    for (uint32_t i = 0; i < count; ++i)
        if (!read_SparseImageFormatProperties(reader, &values[i])) {
            *pCount = 0;
            return;
        }
    if (pProperties == nullptr) {
        *pCount = count;
        return;
    }
    const uint32_t to_write = *pCount < count ? *pCount : count;
    for (uint32_t i = 0; i < to_write; ++i) pProperties[i].properties = values[i];
    *pCount = to_write;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceFragmentShadingRatesKHR(
    VkPhysicalDevice handle, uint32_t* pCount,
    VkPhysicalDeviceFragmentShadingRateKHR* pRates) {
    if (pCount == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    RemotePhysicalDevice* device = to_physical_device(handle);
    Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, Opcode::vkGetPhysicalDeviceFragmentShadingRatesKHR,
                    request, &reply)) return VK_ERROR_INITIALIZATION_FAILED;
    Reader reader(reply.data(), reply.size());
    if (reader.u32() != static_cast<uint32_t>(remoting::Status::Ok))
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult remote_result = static_cast<VkResult>(reader.i32());
    const uint32_t count = reader.u32();
    if (count > remoting::kMaxArrayElements || count > reader.remaining() / 12)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct Rate { VkSampleCountFlags samples; VkExtent2D size; };
    std::vector<Rate> values(count);
    for (uint32_t i = 0; i < count; ++i) {
        values[i].samples = reader.u32();
        values[i].size.width = reader.u32();
        values[i].size.height = reader.u32();
    }
    if (!reader.ok()) return VK_ERROR_INITIALIZATION_FAILED;
    if (remote_result != VK_SUCCESS) return remote_result;
    std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(
        count, VkPhysicalDeviceFragmentShadingRateKHR{
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR});
    for (uint32_t i = 0; i < count; ++i) {
        rates[i].sampleCounts = values[i].samples;
        rates[i].fragmentSize = values[i].size;
    }
    return remoting::copy_fragment_shading_rates(rates.data(), count, pCount, pRates);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice d, VkPhysicalDeviceFeatures2* p) { GetPhysicalDeviceFeatures2(d, p); }
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2KHR(VkPhysicalDevice d, VkPhysicalDeviceProperties2* p) { GetPhysicalDeviceProperties2(d, p); }
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice d, VkFormat f, VkFormatProperties2* p) { GetPhysicalDeviceFormatProperties2(d, f, p); }
VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice d, const VkPhysicalDeviceImageFormatInfo2* i, VkImageFormatProperties2* p) { return GetPhysicalDeviceImageFormatProperties2(d, i, p); }
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice d, uint32_t* c, VkQueueFamilyProperties2* p) { GetPhysicalDeviceQueueFamilyProperties2(d, c, p); }
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties2* p) { GetPhysicalDeviceMemoryProperties2(d, p); }
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSparseImageFormatProperties2KHR(VkPhysicalDevice d, const VkPhysicalDeviceSparseImageFormatInfo2* i, uint32_t* c, VkSparseImageFormatProperties2* p) { GetPhysicalDeviceSparseImageFormatProperties2(d, i, c, p); }

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDeviceGroups(
    VkInstance handle, uint32_t* pCount, VkPhysicalDeviceGroupProperties* pGroups) {
    if (!pCount) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count=0; VkResult result=EnumeratePhysicalDevices(handle,&count,nullptr);
    if(result!=VK_SUCCESS)return result;
    std::vector<VkPhysicalDevice> devices(count);if(count)result=EnumeratePhysicalDevices(handle,&count,devices.data());
    if(!pGroups){*pCount=count;return result;}
    uint32_t n=*pCount<count?*pCount:count;
    for(uint32_t i=0;i<n;++i){void* next=pGroups[i].pNext;pGroups[i]={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES};pGroups[i].pNext=next;pGroups[i].physicalDeviceCount=1;pGroups[i].physicalDevices[0]=devices[i];pGroups[i].subsetAllocation=VK_FALSE;}
    *pCount=n;return n<count?VK_INCOMPLETE:result;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceExternalBufferProperties(
    VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo*, VkExternalBufferProperties* out) {
    if(!out)return;out->externalMemoryProperties={};
}
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceExternalFenceProperties(
    VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo*, VkExternalFenceProperties* out) {
    if(!out)return;out->exportFromImportedHandleTypes=0;out->compatibleHandleTypes=0;out->externalFenceFeatures=0;
}
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo*, VkExternalSemaphoreProperties* out) {
    if(!out)return;out->exportFromImportedHandleTypes=0;out->compatibleHandleTypes=0;out->externalSemaphoreFeatures=0;
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
    if (reader.u32() != static_cast<uint32_t>(remoting::Status::Ok)) {
        *pCount = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult remote_result = static_cast<VkResult>(reader.i32());
    const uint32_t count = reader.u32();
    if (remote_result != VK_SUCCESS || count > remoting::kMaxArrayElements) {
        *pCount = 0;
        return remote_result != VK_SUCCESS ? remote_result : VK_ERROR_INITIALIZATION_FAILED;
    }
    std::vector<VkExtensionProperties> exts(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string name;
        if (!reader.string(&name) || name.size() >= VK_MAX_EXTENSION_NAME_SIZE) {
            *pCount = 0;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        memcpy(exts[i].extensionName, name.data(), name.size());
        exts[i].specVersion = reader.u32();
    }
    if (!reader.ok()) {
        *pCount = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
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
        D(EnumeratePhysicalDeviceGroups),
        {"vkEnumeratePhysicalDeviceGroupsKHR", reinterpret_cast<PFN_vkVoidFunction>(EnumeratePhysicalDeviceGroups)},
        D(GetPhysicalDeviceProperties),
        D(GetPhysicalDeviceFeatures),
        D(GetPhysicalDeviceMemoryProperties),
        D(GetPhysicalDeviceQueueFamilyProperties),
        D(GetPhysicalDeviceFormatProperties),
        D(GetPhysicalDeviceImageFormatProperties),
        D(GetPhysicalDeviceSparseImageFormatProperties),
        D(GetPhysicalDeviceFeatures2),
        D(GetPhysicalDeviceFeatures2KHR),
        D(GetPhysicalDeviceProperties2),
        D(GetPhysicalDeviceProperties2KHR),
        D(GetPhysicalDeviceFormatProperties2),
        D(GetPhysicalDeviceFormatProperties2KHR),
        D(GetPhysicalDeviceImageFormatProperties2),
        D(GetPhysicalDeviceImageFormatProperties2KHR),
        D(GetPhysicalDeviceQueueFamilyProperties2),
        D(GetPhysicalDeviceQueueFamilyProperties2KHR),
        D(GetPhysicalDeviceMemoryProperties2),
        D(GetPhysicalDeviceMemoryProperties2KHR),
        D(GetPhysicalDeviceSparseImageFormatProperties2),
        D(GetPhysicalDeviceSparseImageFormatProperties2KHR),
        D(GetPhysicalDeviceFragmentShadingRatesKHR),
        D(GetPhysicalDeviceExternalBufferProperties),
        {"vkGetPhysicalDeviceExternalBufferPropertiesKHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceExternalBufferProperties)},
        D(GetPhysicalDeviceExternalFenceProperties),
        {"vkGetPhysicalDeviceExternalFencePropertiesKHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceExternalFenceProperties)},
        D(GetPhysicalDeviceExternalSemaphoreProperties),
        {"vkGetPhysicalDeviceExternalSemaphorePropertiesKHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceExternalSemaphoreProperties)},
        D(EnumerateDeviceExtensionProperties),
        D(EnumerateDeviceLayerProperties),
#undef D
    };
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

}  // namespace remoting
