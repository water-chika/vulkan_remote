// Client-side Vulkan ICD: a driver for a machine that has no GPU.
//
// The probe proved the wire worked, but it was my own client calling my own
// functions. This instead registers with the real Vulkan loader, so unmodified
// Vulkan programs (vulkaninfo, and in principle anything else) drive it
// without knowing a socket is involved. That is a much stronger test: the
// loader enforces the ICD interface, and the application decides what to call.
//
// Implemented: instance creation and the physical device queries. Device
// creation deliberately fails with a clear message rather than pretending,
// because a VkDevice implies queues, command buffers and memory that can be
// mapped into the caller's address space, and mapped memory cannot exist at
// the far end of a socket.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "remoting_commands.inl"
#include "wire.hpp"

namespace {

struct RemotePhysicalDevice {
    // Must be first: the loader reads this magic to verify that a dispatchable
    // handle really came from an ICD. Getting this wrong crashes the loader
    // rather than producing a clean error.
    VK_LOADER_DATA loader_data;
    struct RemoteInstance* instance;
    uint64_t remote_id;
};

struct RemoteInstance {
    VK_LOADER_DATA loader_data;
    int fd = -1;
    std::vector<RemotePhysicalDevice*> physical_devices;
    std::mutex mutex;  // One socket, so calls must not interleave.
};

RemoteInstance* to_instance(VkInstance instance) {
    return reinterpret_cast<RemoteInstance*>(instance);
}

RemotePhysicalDevice* to_physical_device(VkPhysicalDevice device) {
    return reinterpret_cast<RemotePhysicalDevice*>(device);
}

// Sends a request and waits for its reply. Returns false if the link died or
// the server refused; the caller then reports a Vulkan error rather than
// handing back uninitialised data.
bool round_trip(RemoteInstance* instance, remoting::Opcode opcode,
                const remoting::Writer& request, std::vector<char>* reply) {
    std::lock_guard<std::mutex> lock(instance->mutex);

    if (instance->fd < 0) return false;
    if (!remoting::send_message(instance->fd, static_cast<uint32_t>(opcode), request.data())) {
        return false;
    }

    remoting::MessageHeader header{};
    if (!remoting::recv_message(instance->fd, &header, reply)) return false;
    if (header.opcode != static_cast<uint32_t>(opcode)) return false;

    // Every reply begins with a status word; a non-Ok one means the server
    // could not or would not perform the call.
    if (reply->size() < sizeof(uint32_t)) return false;
    uint32_t status = 0;
    memcpy(&status, reply->data(), sizeof(status));
    return status == static_cast<uint32_t>(remoting::Status::Ok);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char* /*layer*/,
                                                                    uint32_t* count,
                                                                    VkExtensionProperties*) {
    *count = 0;
    return VK_SUCCESS;
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

    instance->fd = remoting::connect_to(host, port);
    if (instance->fd < 0) {
        fprintf(stderr, "vulkan-remoting: no server at %s:%u\n", host, static_cast<unsigned>(port));
        delete instance;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    {
        remoting::Writer request;
        request.string(remoting::kCommandSetDigest);
        std::vector<char> reply;
        if (!round_trip(instance, remoting::Opcode::Handshake, request, &reply)) {
            fprintf(stderr,
                    "vulkan-remoting: handshake refused; server was generated from a different "
                    "vk.xml (ours is %s)\n",
                    remoting::kCommandSetDigest);
            ::close(instance->fd);
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
    if (instance->fd >= 0) ::close(instance->fd);
    for (auto* device : instance->physical_devices) delete device;
    delete instance;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance handle, uint32_t* pCount,
                                                        VkPhysicalDevice* pDevices) {
    RemoteInstance* instance = to_instance(handle);

    if (instance->physical_devices.empty()) {
        remoting::Writer request;
        std::vector<char> reply;
        if (!round_trip(instance, remoting::Opcode::vkEnumeratePhysicalDevices, request, &reply)) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        remoting::Reader reader(reply.data(), reply.size());
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
    remoting::Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, remoting::Opcode::vkGetPhysicalDeviceProperties, request,
                    &reply)) {
        return;
    }

    remoting::Reader reader(reply.data(), reply.size());
    reader.u32();  // status
    const uint32_t remote_api_version = reader.u32();

    // Report 1.0 regardless of what the remote device supports. Advertising the
    // remote 1.4 would invite the application to call 1.1+ entry points this
    // driver does not implement, and it would then index results that were
    // never filled in. A driver must not claim a version it cannot serve.
    (void)remote_api_version;
    pProperties->apiVersion = VK_API_VERSION_1_0;
    pProperties->driverVersion = reader.u32();
    pProperties->vendorID = reader.u32();
    pProperties->deviceID = reader.u32();
    pProperties->deviceType = static_cast<VkPhysicalDeviceType>(reader.u32());
    std::string name;
    reader.string(&name);
    snprintf(pProperties->deviceName, sizeof(pProperties->deviceName), "%s", name.c_str());

    // Limits are not remoted yet; leaving them zeroed is visibly wrong rather
    // than plausibly wrong, which is the safer failure for a partial driver.
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice handle,
                                                     VkPhysicalDeviceFeatures* pFeatures) {
    memset(pFeatures, 0, sizeof(*pFeatures));

    RemotePhysicalDevice* device = to_physical_device(handle);
    remoting::Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, remoting::Opcode::vkGetPhysicalDeviceFeatures, request,
                    &reply)) {
        return;
    }

    remoting::Reader reader(reply.data(), reply.size());
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
    remoting::Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, remoting::Opcode::vkGetPhysicalDeviceMemoryProperties,
                    request, &reply)) {
        return;
    }

    remoting::Reader reader(reply.data(), reply.size());
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
    remoting::Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!round_trip(device->instance, remoting::Opcode::vkGetPhysicalDeviceQueueFamilyProperties,
                    request, &reply)) {
        *pCount = 0;
        return;
    }

    remoting::Reader reader(reply.data(), reply.size());
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

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat,
                                                             VkFormatProperties* pProperties) {
    // Not remoted: reporting no format support is honest for a driver that
    // cannot create a device anyway.
    memset(pProperties, 0, sizeof(*pProperties));
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags,
    VkImageFormatProperties* pProperties) {
    memset(pProperties, 0, sizeof(*pProperties));
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits, VkImageUsageFlags,
    VkImageTiling, uint32_t* pCount, VkSparseImageFormatProperties*) {
    *pCount = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*,
                                                                  uint32_t* pCount,
                                                                  VkExtensionProperties*) {
    *pCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pCount,
                                                              VkLayerProperties*) {
    *pCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                            const VkAllocationCallbacks*, VkDevice*) {
    // Refused on purpose, see the file header. A VkDevice promises mappable
    // memory, and a socket cannot deliver one.
    fprintf(stderr,
            "vulkan-remoting: vkCreateDevice is not supported. This driver remotes physical "
            "device queries only; rendering requires memory the caller can map, which cannot "
            "cross a socket.\n");
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice, const char*) {
    // Required of every ICD even though this one never creates a device: the
    // loader refuses to load a driver that does not export it.
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
    ENTRY(CreateDevice),
    ENTRY(GetDeviceProcAddr),
    ENTRY(GetInstanceProcAddr),
};

#undef ENTRY

PFN_vkVoidFunction lookup(const char* name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char* pName) {
    return lookup(pName);
}

PFN_vkVoidFunction lookup(const char* name) {
    if (name == nullptr) return nullptr;
    for (const Entry& entry : kEntries) {
        if (strcmp(entry.name, name) == 0) return entry.function;
    }
    return nullptr;
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
