// Server-side instance-level handlers: the handshake and every physical
// device query. These run before any VkDevice exists, so unlike most other
// handlers they read directly off the Server (via Session::server) rather
// than an ObjectTables entry.

#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

#include "marshal.hpp"
#include "session.hpp"

namespace {

using remoting::Session;
using remoting::Status;

void handle_Handshake(Session& s) {
    std::string client_schema;
    std::string client_registry;
    std::string client_abi;
    std::string client_digest;
    s.reader.string(&client_schema);
    s.reader.string(&client_registry);
    s.reader.string(&client_abi);
    s.reader.string(&client_digest);
    const bool match = s.reader.ok() && client_schema == remoting::kWireSchemaRevision &&
                       client_registry == remoting::kRegistrySha256 &&
                       client_abi == remoting::kWireAbi &&
                       client_digest == remoting::kCommandSetDigest;
    if (!match) {
        fprintf(stderr,
                "server: rejecting client: schema=%s/%s registry=%s/%s ABI=%s/%s "
                "commands=%s/%s\n",
                client_schema.c_str(), remoting::kWireSchemaRevision, client_registry.c_str(),
                remoting::kRegistrySha256, client_abi.c_str(), remoting::kWireAbi,
                client_digest.c_str(), remoting::kCommandSetDigest);
    }
    s.writer.u32(match ? static_cast<uint32_t>(Status::Ok)
                       : static_cast<uint32_t>(Status::DecodeError));
    s.writer.string(remoting::kCommandSetDigest);
    s.writer.u32(static_cast<uint32_t>(s.server.physical_device_count()));
    s.reply();
    if (!match) s.close_connection = true;
}

void handle_vkEnumeratePhysicalDevices(Session& s) {
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.i32(VK_SUCCESS);
    s.writer.u32(static_cast<uint32_t>(s.server.physical_device_count()));
    for (size_t i = 0; i < s.server.physical_device_count(); ++i) {
        s.writer.handle(static_cast<uint64_t>(i + 1));
    }
    s.reply();
}

void handle_vkGetPhysicalDeviceProperties(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    // Marshal each field explicitly: client and server ABIs may differ (most
    // notably size_t inside limits), but applications still need every limit
    // for valid create calls.
    remoting::write_PhysicalDeviceProperties(s.writer, props);
    s.reply();
}

void handle_vkGetPhysicalDeviceMemoryProperties(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(device, &props);
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.u32(props.memoryTypeCount);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        s.writer.u32(props.memoryTypes[i].propertyFlags);
        s.writer.u32(props.memoryTypes[i].heapIndex);
    }
    s.writer.u32(props.memoryHeapCount);
    for (uint32_t i = 0; i < props.memoryHeapCount; ++i) {
        s.writer.u64(props.memoryHeaps[i].size);
        s.writer.u32(props.memoryHeaps[i].flags);
    }
    s.reply();
}

void handle_vkGetPhysicalDeviceQueueFamilyProperties(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    if (count) vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        s.writer.u32(families[i].queueFlags);
        s.writer.u32(families[i].queueCount);
        s.writer.u32(families[i].timestampValidBits);
        s.writer.u32(families[i].minImageTransferGranularity.width);
        s.writer.u32(families[i].minImageTransferGranularity.height);
        s.writer.u32(families[i].minImageTransferGranularity.depth);
    }
    s.reply();
}

void handle_vkGetPhysicalDeviceFeatures(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(device, &features);
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.bytes(&features, sizeof(features));
    s.reply();
}

void handle_vkGetPhysicalDeviceFormatProperties(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    const int32_t format = s.reader.i32();
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device, static_cast<VkFormat>(format), &props);
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.bytes(&props, sizeof(props));
    s.reply();
}

void handle_vkGetPhysicalDeviceImageFormatProperties(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    const int32_t format = s.reader.i32();
    const int32_t type = s.reader.i32();
    const int32_t tiling = s.reader.i32();
    const uint32_t usage = s.reader.u32();
    const uint32_t flags = s.reader.u32();
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    VkImageFormatProperties props{};
    const VkResult result = vkGetPhysicalDeviceImageFormatProperties(
        device, static_cast<VkFormat>(format), static_cast<VkImageType>(type),
        static_cast<VkImageTiling>(tiling), static_cast<VkImageUsageFlags>(usage),
        static_cast<VkImageCreateFlags>(flags), &props);
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.i32(static_cast<int32_t>(result));
    s.writer.bytes(&props, sizeof(props));
    s.reply();
}

void handle_vkEnumerateDeviceExtensionProperties(Session& s) {
    const uint64_t physdev_id = s.reader.handle();
    VkPhysicalDevice physdev = s.server.physical_device_from_id(physdev_id);
    if (!s.reader.ok() || physdev == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }

    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physdev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    if (count) vkEnumerateDeviceExtensionProperties(physdev, nullptr, &count, exts.data());

    // Only advertise what this project actually remotes. WSI is separate
    // work landing in wayland/; report VK_KHR_swapchain only if the real
    // device supports it, so that other agent's code has something truthful
    // to build on, and report nothing else.
    const VkExtensionProperties* swapchain = nullptr;
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, "VK_KHR_swapchain") == 0) {
            swapchain = &e;
            break;
        }
    }

    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.i32(VK_SUCCESS);
    s.writer.u32(swapchain ? 1 : 0);
    if (swapchain) remoting::write_ExtensionProperties(s.writer, *swapchain);
    s.reply();
}

}  // namespace

REGISTER_HANDLER(Handshake, handle_Handshake);
REGISTER_HANDLER(vkEnumeratePhysicalDevices, handle_vkEnumeratePhysicalDevices);
REGISTER_HANDLER(vkGetPhysicalDeviceProperties, handle_vkGetPhysicalDeviceProperties);
REGISTER_HANDLER(vkGetPhysicalDeviceMemoryProperties, handle_vkGetPhysicalDeviceMemoryProperties);
REGISTER_HANDLER(vkGetPhysicalDeviceQueueFamilyProperties,
                  handle_vkGetPhysicalDeviceQueueFamilyProperties);
REGISTER_HANDLER(vkGetPhysicalDeviceFeatures, handle_vkGetPhysicalDeviceFeatures);
REGISTER_HANDLER(vkGetPhysicalDeviceFormatProperties, handle_vkGetPhysicalDeviceFormatProperties);
REGISTER_HANDLER(vkGetPhysicalDeviceImageFormatProperties,
                  handle_vkGetPhysicalDeviceImageFormatProperties);
REGISTER_HANDLER(vkEnumerateDeviceExtensionProperties,
                  handle_vkEnumerateDeviceExtensionProperties);
