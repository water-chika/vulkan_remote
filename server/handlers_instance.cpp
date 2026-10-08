// Server-side instance-level handlers: the handshake and every physical
// device query. These run before any VkDevice exists, so unlike most other
// handlers they read directly off the Server (via Session::server) rather
// than an ObjectTables entry.

#include <algorithm>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

#include "extension_contract.hpp"
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
    s.writer.u32(s.server.loader_api_version());
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

void handle_vkGetPhysicalDeviceSparseImageFormatProperties(Session& s) {
    VkPhysicalDevice device = s.server.physical_device_from_id(s.reader.handle());
    const VkFormat format = static_cast<VkFormat>(s.reader.i32());
    const VkImageType type = static_cast<VkImageType>(s.reader.i32());
    const VkSampleCountFlagBits samples = static_cast<VkSampleCountFlagBits>(s.reader.i32());
    const VkImageUsageFlags usage = s.reader.u32();
    const VkImageTiling tiling = static_cast<VkImageTiling>(s.reader.i32());
    if (!s.reader.ok() || device == VK_NULL_HANDLE) { s.reply_status(Status::DecodeError); return; }
    uint32_t count = 0;
    vkGetPhysicalDeviceSparseImageFormatProperties(device, format, type, samples, usage, tiling,
                                                    &count, nullptr);
    if (count > remoting::kMaxArrayElements) { s.reply_status(Status::DecodeError); return; }
    std::vector<VkSparseImageFormatProperties> values(count);
    if (count) vkGetPhysicalDeviceSparseImageFormatProperties(device, format, type, samples, usage,
                                                               tiling, &count, values.data());
    s.writer.u32(static_cast<uint32_t>(Status::Ok)); s.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i)
        remoting::write_SparseImageFormatProperties(s.writer, values[i]);
    s.reply();
}

void handle_vkGetPhysicalDeviceFeatures2(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    remoting::Arena arena;
    void* chain = nullptr;
    if (!remoting::read_pnext_output_topology(s.reader, arena,
                                               remoting::PNextContext::QueryFeaturesOutput,
                                               &chain)) {
        s.reply_status(Status::DecodeError);
        return;
    }
    VkPhysicalDeviceFeatures2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain};
    vkGetPhysicalDeviceFeatures2(device, &properties);
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    remoting::write_PhysicalDeviceFeatures(s.writer, properties.features);
    remoting::write_pnext_output_values(s.writer,
                                        remoting::PNextContext::QueryFeaturesOutput, chain);
    s.reply();
}

void handle_vkGetPhysicalDeviceProperties2(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    remoting::Arena arena;
    void* chain = nullptr;
    if (!remoting::read_pnext_output_topology(s.reader, arena,
                                               remoting::PNextContext::QueryPropertiesOutput,
                                               &chain)) {
        s.reply_status(Status::DecodeError);
        return;
    }
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, chain};
    vkGetPhysicalDeviceProperties2(device, &properties);
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    remoting::write_PhysicalDeviceProperties(s.writer, properties.properties);
    remoting::write_pnext_output_values(s.writer,
                                        remoting::PNextContext::QueryPropertiesOutput, chain);
    s.reply();
}

void handle_vkGetPhysicalDeviceFormatProperties2(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    const VkFormat format = static_cast<VkFormat>(s.reader.i32());
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    VkFormatProperties2 properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    vkGetPhysicalDeviceFormatProperties2(device, format, &properties);
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    remoting::write_FormatProperties(s.writer, properties.formatProperties);
    s.reply();
}

void handle_vkGetPhysicalDeviceImageFormatProperties2(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    info.format = static_cast<VkFormat>(s.reader.i32());
    info.type = static_cast<VkImageType>(s.reader.i32());
    info.tiling = static_cast<VkImageTiling>(s.reader.i32());
    info.usage = static_cast<VkImageUsageFlags>(s.reader.u32());
    info.flags = static_cast<VkImageCreateFlags>(s.reader.u32());
    remoting::Arena arena;
    const void* input_chain = nullptr;
    if (!s.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_pnext_chain(s.reader, arena, s.tables,
                                    remoting::PNextContext::ImageCreate,
                                    &input_chain)) {
        s.reply_status(Status::DecodeError);
        return;
    }
    info.pNext = input_chain;
    void* output_chain=nullptr;
    if(!remoting::read_pnext_output_topology(s.reader,arena,
          remoting::PNextContext::ImageFormatPropertiesOutput,&output_chain)){
        s.reply_status(Status::DecodeError);return;
    }
    VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,output_chain};
    const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(device, &info, &properties);
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.i32(static_cast<int32_t>(result));
    remoting::write_ImageFormatProperties(s.writer, properties.imageFormatProperties);
    remoting::write_pnext_output_values(s.writer,
        remoting::PNextContext::ImageFormatPropertiesOutput,output_chain);
    s.reply();
}

void handle_vkGetPhysicalDeviceQueueFamilyProperties2(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties2> properties(
        count, VkQueueFamilyProperties2{VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
    if (count) vkGetPhysicalDeviceQueueFamilyProperties2(device, &count, properties.data());
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        remoting::write_QueueFamilyProperties(s.writer,
                                                properties[i].queueFamilyProperties);
    }
    s.reply();
}

void handle_vkGetPhysicalDeviceMemoryProperties2(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    VkPhysicalDeviceMemoryProperties2 properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    vkGetPhysicalDeviceMemoryProperties2(device, &properties);
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    remoting::write_PhysicalDeviceMemoryProperties(s.writer, properties.memoryProperties);
    s.reply();
}

void handle_vkGetPhysicalDeviceSparseImageFormatProperties2(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    VkPhysicalDeviceSparseImageFormatInfo2 info{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2};
    info.format = static_cast<VkFormat>(s.reader.i32());
    info.type = static_cast<VkImageType>(s.reader.i32());
    info.samples = static_cast<VkSampleCountFlagBits>(s.reader.i32());
    info.usage = static_cast<VkImageUsageFlags>(s.reader.u32());
    info.tiling = static_cast<VkImageTiling>(s.reader.i32());
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceSparseImageFormatProperties2(device, &info, &count, nullptr);
    std::vector<VkSparseImageFormatProperties2> properties(
        count, VkSparseImageFormatProperties2{VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2});
    if (count) vkGetPhysicalDeviceSparseImageFormatProperties2(device, &info, &count, properties.data());
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i)
        remoting::write_SparseImageFormatProperties(s.writer, properties[i].properties);
    s.reply();
}

void handle_vkGetPhysicalDeviceFragmentShadingRatesKHR(Session& s) {
    const uint64_t id = s.reader.handle();
    VkPhysicalDevice device = s.server.physical_device_from_id(id);
    if (!s.reader.ok() || device == VK_NULL_HANDLE) {
        s.reply_status(Status::DecodeError);
        return;
    }
    auto get_rates = reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(
        vkGetInstanceProcAddr(s.server.instance(), "vkGetPhysicalDeviceFragmentShadingRatesKHR"));
    if (get_rates == nullptr) {
        s.reply_status(Status::UnsupportedCommand);
        return;
    }
    uint32_t count = 0;
    VkResult result = get_rates(device, &count, nullptr);
    if (count > remoting::kMaxArrayElements) {
        s.reply_status(Status::DecodeError);
        return;
    }
    std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(
        count, VkPhysicalDeviceFragmentShadingRateKHR{
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR});
    if (result == VK_SUCCESS && count)
        result = get_rates(device, &count, rates.data());
    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.i32(static_cast<int32_t>(result));
    s.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        s.writer.u32(rates[i].sampleCounts);
        s.writer.u32(rates[i].fragmentSize.width);
        s.writer.u32(rates[i].fragmentSize.height);
    }
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

    std::vector<VkExtensionProperties> supported;
    const auto find_extension = [&](const char* name) -> const VkExtensionProperties* {
        for (const auto& extension : exts)
            if (strcmp(extension.extensionName, name) == 0) return &extension;
        return nullptr;
    };
    if (const auto* swapchain = find_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        supported.push_back(*swapchain);

    // The reviewed device contract is a dependency-closed unit. Advertising a
    // subset would let applications request an unusable FSR/renderpass2 graph,
    // so expose all six only when the real device reports every member.
    bool complete_contract = true;
    for (const auto& required : remoting::kRequiredDeviceExtensions)
        complete_contract &= find_extension(required.name) != nullptr;
    if (complete_contract) {
        for (const auto& required : remoting::kRequiredDeviceExtensions) {
            VkExtensionProperties property{};
            strncpy(property.extensionName, required.name, VK_MAX_EXTENSION_NAME_SIZE - 1);
            // This implementation may support no more than its reviewed contract,
            // while the real device may advertise no more than its own revision.
            // Advertising either larger value would not be a true intersection.
            property.specVersion = std::min(find_extension(required.name)->specVersion,
                                            required.spec_version);
            supported.push_back(property);
        }
    }

    s.writer.u32(static_cast<uint32_t>(Status::Ok));
    s.writer.i32(VK_SUCCESS);
    s.writer.u32(static_cast<uint32_t>(supported.size()));
    for (const auto& extension : supported)
        remoting::write_ExtensionProperties(s.writer, extension);
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
REGISTER_HANDLER(vkGetPhysicalDeviceSparseImageFormatProperties,
                  handle_vkGetPhysicalDeviceSparseImageFormatProperties);
REGISTER_HANDLER(vkGetPhysicalDeviceFeatures2, handle_vkGetPhysicalDeviceFeatures2);
REGISTER_HANDLER(vkGetPhysicalDeviceProperties2, handle_vkGetPhysicalDeviceProperties2);
REGISTER_HANDLER(vkGetPhysicalDeviceFormatProperties2, handle_vkGetPhysicalDeviceFormatProperties2);
REGISTER_HANDLER(vkGetPhysicalDeviceImageFormatProperties2,
                  handle_vkGetPhysicalDeviceImageFormatProperties2);
REGISTER_HANDLER(vkGetPhysicalDeviceQueueFamilyProperties2,
                  handle_vkGetPhysicalDeviceQueueFamilyProperties2);
REGISTER_HANDLER(vkGetPhysicalDeviceMemoryProperties2,
                  handle_vkGetPhysicalDeviceMemoryProperties2);
REGISTER_HANDLER(vkGetPhysicalDeviceSparseImageFormatProperties2,
                  handle_vkGetPhysicalDeviceSparseImageFormatProperties2);
REGISTER_HANDLER(vkGetPhysicalDeviceFragmentShadingRatesKHR,
                  handle_vkGetPhysicalDeviceFragmentShadingRatesKHR);
REGISTER_HANDLER(vkEnumerateDeviceExtensionProperties,
                  handle_vkEnumerateDeviceExtensionProperties);
