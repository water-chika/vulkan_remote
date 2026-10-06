// Deterministic Vulkan 1.0 mipmap regression. The command buffer uploads an
// 8x8 base level, generates three levels with per-level barriers and
// vkCmdBlitImage, then reads back and verifies the complete mip chain.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define VK_CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    std::fprintf(stderr, "FATAL: %s failed with VkResult=%d\n", #call, int(r_)); \
    std::exit(1); \
} } while (0)

namespace {

constexpr uint32_t kBaseSize = 8;
constexpr uint32_t kLevelCount = 4;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkDeviceSize kBaseBytes = kBaseSize * kBaseSize * 4;
constexpr VkDeviceSize kReadbackBytes = (8 * 8 + 4 * 4 + 2 * 2 + 1) * 4;
constexpr uint32_t kExpectedChecksum = 0x82f68ebeu;

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
    bool coherent = false;
};

uint32_t image_memory_type(VkPhysicalDevice physical, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if (bits & (1u << i)) return i;
    }
    std::fprintf(stderr, "FATAL: no compatible image memory type\n");
    std::exit(1);
}

uint32_t host_memory_type(VkPhysicalDevice physical, uint32_t bits, bool* coherent) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    const VkMemoryPropertyFlags choices[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    };
    for (VkMemoryPropertyFlags required : choices) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & required) == required) {
                *coherent = (properties.memoryTypes[i].propertyFlags &
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                return i;
            }
        }
    }
    std::fprintf(stderr, "FATAL: no host-visible memory type\n");
    std::exit(1);
}

Buffer create_buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                     VkBufferUsageFlags usage) {
    Buffer buffer;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &info, nullptr, &buffer.handle));

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = host_memory_type(
        physical, requirements.memoryTypeBits, &buffer.coherent);
    VK_CHECK(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory));
    buffer.allocation_size = requirements.size;
    VK_CHECK(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0));
    return buffer;
}

VkMappedMemoryRange whole_mapped_range(VkPhysicalDevice physical,
                                        const Buffer& buffer,
                                        VkDeviceSize used_size) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    const VkDeviceSize atom = properties.limits.nonCoherentAtomSize;
    const VkDeviceSize aligned = (used_size + atom - 1) / atom * atom;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = buffer.memory;
    range.offset = 0;
    range.size = aligned <= buffer.allocation_size ? aligned : VK_WHOLE_SIZE;
    return range;
}

std::vector<uint8_t> make_reference() {
    std::vector<uint8_t> result;
    result.reserve(static_cast<size_t>(kReadbackBytes));
    uint32_t width = kBaseSize;
    uint32_t height = kBaseSize;
    std::vector<uint8_t> level(width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* pixel = &level[(y * width + x) * 4];
            pixel[0] = static_cast<uint8_t>(x * 31 + y * 7);
            pixel[1] = static_cast<uint8_t>(x * 11 + y * 29);
            pixel[2] = static_cast<uint8_t>((x ^ y) * 37);
            pixel[3] = 255;
        }
    }
    result.insert(result.end(), level.begin(), level.end());

    for (uint32_t mip = 1; mip < kLevelCount; ++mip) {
        const uint32_t next_width = width / 2;
        const uint32_t next_height = height / 2;
        std::vector<uint8_t> next(next_width * next_height * 4);
        for (uint32_t y = 0; y < next_height; ++y) {
            for (uint32_t x = 0; x < next_width; ++x) {
                const size_t source = ((2 * y + 1) * width + 2 * x + 1) * 4;
                const size_t destination = (y * next_width + x) * 4;
                std::memcpy(next.data() + destination, level.data() + source, 4);
            }
        }
        result.insert(result.end(), next.begin(), next.end());
        level = std::move(next);
        width = next_width;
        height = next_height;
    }
    return result;
}

uint32_t checksum(const uint8_t* bytes, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace

int main(int argc, char** argv) {
    bool validate = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-validate") == 0) validate = false;
        else if (std::strcmp(argv[i], "--validate") == 0) validate = true;
        else {
            std::fprintf(stderr, "usage: %s [--no-validate]\n", argv[0]);
            return 2;
        }
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "mipmap-regression";
    application.apiVersion = VK_API_VERSION_1_0;
    std::vector<const char*> layers;
    if (validate) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        for (const auto& layer : available) {
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                layers.push_back("VK_LAYER_KHRONOS_validation");
        }
    }
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    instance_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&instance_info, nullptr, &instance));

    uint32_t physical_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr));
    if (!physical_count) {
        std::fprintf(stderr, "FATAL: no Vulkan physical device\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()));
    VkPhysicalDevice physical = physical_devices[0];

    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(physical, kFormat, &format_properties);
    const VkFormatFeatureFlags required_features =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((format_properties.optimalTilingFeatures & required_features) != required_features) {
        std::fprintf(stderr, "FATAL: R8G8B8A8_UNORM does not support optimal-image blits\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i;
            break;
        }
    }
    if (family == UINT32_MAX) {
        std::fprintf(stderr, "FATAL: no graphics-capable queue\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(physical, &device_info, nullptr, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    const std::vector<uint8_t> expected = make_reference();
    Buffer staging = create_buffer(physical, device, kBaseBytes,
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Buffer readback = create_buffer(physical, device, kReadbackBytes,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device, staging.memory, 0, staging.allocation_size, 0, &mapped));
    std::memcpy(mapped, expected.data(), static_cast<size_t>(kBaseBytes));
    if (!staging.coherent) {
        VkMappedMemoryRange range = whole_mapped_range(physical, staging, kBaseBytes);
        VK_CHECK(vkFlushMappedMemoryRanges(device, 1, &range));
    }
    vkUnmapMemory(device, staging.memory);

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kFormat;
    image_info.extent = {kBaseSize, kBaseSize, 1};
    image_info.mipLevels = kLevelCount;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImage(device, &image_info, nullptr, &image));
    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(device, image, &image_requirements);
    VkMemoryAllocateInfo image_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    image_allocation.allocationSize = image_requirements.size;
    image_allocation.memoryTypeIndex = image_memory_type(
        physical, image_requirements.memoryTypeBits);
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(device, &image_allocation, nullptr, &image_memory));
    VK_CHECK(vkBindImageMemory(device, image, image_memory, 0));

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(command, &begin));

    VkImageMemoryBarrier level_zero{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    level_zero.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    level_zero.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    level_zero.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    level_zero.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    level_zero.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    level_zero.image = image;
    level_zero.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    level_zero.subresourceRange.baseMipLevel = 0;
    level_zero.subresourceRange.levelCount = 1;
    level_zero.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &level_zero);

    VkBufferImageCopy upload{};
    upload.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    upload.imageSubresource.layerCount = 1;
    upload.imageExtent = {kBaseSize, kBaseSize, 1};
    vkCmdCopyBufferToImage(command, staging.handle, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);

    for (uint32_t mip = 1; mip < kLevelCount; ++mip) {
        VkImageMemoryBarrier barriers[2]{};
        barriers[0].sType = barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = barriers[0].dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = image;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.baseMipLevel = mip - 1;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.layerCount = 1;
        barriers[1] = barriers[0];
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].subresourceRange.baseMipLevel = mip;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 2, barriers);

        const int32_t source_size = static_cast<int32_t>(kBaseSize >> (mip - 1));
        const int32_t destination_size = static_cast<int32_t>(kBaseSize >> mip);
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mip - 1;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = {source_size, source_size, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = mip;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = {destination_size, destination_size, 1};
        vkCmdBlitImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_NEAREST);
    }

    VkImageMemoryBarrier final_level{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    final_level.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    final_level.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    final_level.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    final_level.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    final_level.srcQueueFamilyIndex = final_level.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    final_level.image = image;
    final_level.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    final_level.subresourceRange.baseMipLevel = kLevelCount - 1;
    final_level.subresourceRange.levelCount = 1;
    final_level.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &final_level);

    std::array<VkBufferImageCopy, kLevelCount> downloads{};
    VkDeviceSize offset = 0;
    for (uint32_t mip = 0; mip < kLevelCount; ++mip) {
        const uint32_t size = kBaseSize >> mip;
        downloads[mip].bufferOffset = offset;
        downloads[mip].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        downloads[mip].imageSubresource.mipLevel = mip;
        downloads[mip].imageSubresource.layerCount = 1;
        downloads[mip].imageExtent = {size, size, 1};
        offset += size * size * 4;
    }
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.handle, kLevelCount, downloads.data());
    VkBufferMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.buffer = readback.handle;
    host_barrier.offset = 0;
    host_barrier.size = kReadbackBytes;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                         1, &host_barrier, 0, nullptr);
    VK_CHECK(vkEndCommandBuffer(command));

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000000000ull));

    VK_CHECK(vkMapMemory(device, readback.memory, 0, readback.allocation_size, 0, &mapped));
    if (!readback.coherent) {
        VkMappedMemoryRange range = whole_mapped_range(physical, readback, kReadbackBytes);
        VK_CHECK(vkInvalidateMappedMemoryRanges(device, 1, &range));
    }
    const auto* actual = static_cast<const uint8_t*>(mapped);
    size_t mismatch = expected.size();
    for (size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) {
            mismatch = i;
            break;
        }
    }
    const uint32_t actual_checksum = checksum(actual, expected.size());
    std::array<uint8_t, 4> actual_pixel{};
    std::array<uint8_t, 4> expected_pixel{};
    uint32_t mismatch_level = 0;
    uint32_t mismatch_x = 0;
    uint32_t mismatch_y = 0;
    if (mismatch != expected.size()) {
        size_t level_offset = 0;
        for (uint32_t mip = 0; mip < kLevelCount; ++mip) {
            const uint32_t size = kBaseSize >> mip;
            const size_t level_bytes = size * size * 4;
            if (mismatch < level_offset + level_bytes) {
                const size_t pixel_index = (mismatch - level_offset) / 4;
                mismatch_level = mip;
                mismatch_x = static_cast<uint32_t>(pixel_index % size);
                mismatch_y = static_cast<uint32_t>(pixel_index / size);
                const size_t pixel_offset = level_offset + pixel_index * 4;
                std::memcpy(actual_pixel.data(), actual + pixel_offset, 4);
                std::memcpy(expected_pixel.data(), expected.data() + pixel_offset, 4);
                break;
            }
            level_offset += level_bytes;
        }
    }
    vkUnmapMemory(device, readback.memory);

    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool, 1, &command);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkFreeMemory(device, image_memory, nullptr);
    vkDestroyBuffer(device, readback.handle, nullptr);
    vkFreeMemory(device, readback.memory, nullptr);
    vkDestroyBuffer(device, staging.handle, nullptr);
    vkFreeMemory(device, staging.memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    if (mismatch != expected.size()) {
        std::fprintf(stderr,
                     "FAIL: mip %u pixel (%u,%u) expected %02x%02x%02x%02x, got %02x%02x%02x%02x\n",
                     mismatch_level, mismatch_x, mismatch_y,
                     expected_pixel[0], expected_pixel[1], expected_pixel[2], expected_pixel[3],
                     actual_pixel[0], actual_pixel[1], actual_pixel[2], actual_pixel[3]);
        return 1;
    }
    if (actual_checksum != kExpectedChecksum) {
        std::fprintf(stderr, "FAIL: checksum expected %08x, got %08x\n",
                     kExpectedChecksum, actual_checksum);
        return 1;
    }
    std::printf("PASS: mipmap checksum=%08x bytes=%llu levels=%u\n",
                actual_checksum, static_cast<unsigned long long>(kReadbackBytes), kLevelCount);
    return 0;
}
