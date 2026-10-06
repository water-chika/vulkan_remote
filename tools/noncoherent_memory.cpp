// Deterministic Vulkan 1.0 regression for explicit non-coherent mapped-memory
// synchronization. Two disjoint, nonzero, atom-aligned ranges are flushed,
// copied by the GPU, invalidated, and checked without inspecting other bytes.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#define VK_CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    std::fprintf(stderr, "FATAL: %s failed with VkResult=%d\n", #call, int(r_)); \
    std::exit(1); \
} } while (0)

namespace {

constexpr VkDeviceSize kRangeBytes = 64;
constexpr size_t kOracleBytes = 128;
constexpr uint32_t kExpectedChecksum = 0xc7c92645u;
constexpr const char* kSkip =
    "SKIP: non-coherent host-visible memory unavailable";

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
};

uint8_t pattern_byte(size_t index) {
    return static_cast<uint8_t>((index * 37 + 11) & 255);
}

uint32_t checksum(const std::array<uint8_t, kOracleBytes>& bytes) {
    uint32_t hash = 2166136261u;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

Buffer create_buffer(VkDevice device, VkDeviceSize size) {
    Buffer buffer;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &info, nullptr, &buffer.handle));
    return buffer;
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
    application.pApplicationName = "noncoherent-memory-regression";
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

    VkPhysicalDeviceProperties physical_properties{};
    vkGetPhysicalDeviceProperties(physical, &physical_properties);
    const VkDeviceSize atom = physical_properties.limits.nonCoherentAtomSize;
    if (atom == 0 || atom > std::numeric_limits<VkDeviceSize>::max() / 6) {
        std::fprintf(stderr, "FATAL: invalid nonCoherentAtomSize\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    const VkDeviceSize range_size = ((kRangeBytes + atom - 1) / atom) * atom;
    if (range_size > std::numeric_limits<VkDeviceSize>::max() / 6) {
        std::fprintf(stderr, "FATAL: nonCoherentAtomSize is too large\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    const VkDeviceSize source_offsets[2] = {range_size, 3 * range_size};
    const VkDeviceSize destination_offsets[2] = {2 * range_size, 4 * range_size};
    const VkDeviceSize buffer_size = 6 * range_size;

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
            family = i;
            break;
        }
    }
    if (family == UINT32_MAX) {
        std::fprintf(stderr, "FATAL: no transfer-capable queue\n");
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

    Buffer source = create_buffer(device, buffer_size);
    Buffer destination = create_buffer(device, buffer_size);
    VkMemoryRequirements source_requirements{};
    VkMemoryRequirements destination_requirements{};
    vkGetBufferMemoryRequirements(device, source.handle, &source_requirements);
    vkGetBufferMemoryRequirements(device, destination.handle, &destination_requirements);

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    const uint32_t compatible_bits =
        source_requirements.memoryTypeBits & destination_requirements.memoryTypeBits;
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((compatible_bits & (1u << i)) &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == UINT32_MAX) {
        vkDestroyBuffer(device, destination.handle, nullptr);
        vkDestroyBuffer(device, source.handle, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        std::printf("%s\n", kSkip);
        return 0;
    }

    VkMemoryAllocateInfo source_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    source_allocation.allocationSize = source_requirements.size;
    source_allocation.memoryTypeIndex = memory_type;
    source.allocation_size = source_allocation.allocationSize;
    VK_CHECK(vkAllocateMemory(device, &source_allocation, nullptr, &source.memory));
    VK_CHECK(vkBindBufferMemory(device, source.handle, source.memory, 0));
    VkMemoryAllocateInfo destination_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    destination_allocation.allocationSize = destination_requirements.size;
    destination_allocation.memoryTypeIndex = memory_type;
    destination.allocation_size = destination_allocation.allocationSize;
    VK_CHECK(vkAllocateMemory(device, &destination_allocation, nullptr, &destination.memory));
    VK_CHECK(vkBindBufferMemory(device, destination.handle, destination.memory, 0));

    void* source_mapping = nullptr;
    void* destination_mapping = nullptr;
    VK_CHECK(vkMapMemory(device, source.memory, 0, source.allocation_size, 0,
                         &source_mapping));
    VK_CHECK(vkMapMemory(device, destination.memory, 0,
                         destination.allocation_size, 0, &destination_mapping));
    auto* source_bytes = static_cast<uint8_t*>(source_mapping);
    for (size_t range = 0; range < 2; ++range) {
        for (size_t i = 0; i < kRangeBytes; ++i) {
            source_bytes[source_offsets[range] + i] =
                pattern_byte(range * static_cast<size_t>(kRangeBytes) + i);
        }
    }
    VkMappedMemoryRange flush_ranges[2]{};
    for (size_t i = 0; i < 2; ++i) {
        flush_ranges[i].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        flush_ranges[i].memory = source.memory;
        flush_ranges[i].offset = source_offsets[i];
        flush_ranges[i].size = range_size;
    }
    VK_CHECK(vkFlushMappedMemoryRanges(device, 2, flush_ranges));

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
    VkBufferCopy copies[2]{};
    for (size_t i = 0; i < 2; ++i) {
        copies[i].srcOffset = source_offsets[i];
        copies[i].dstOffset = destination_offsets[i];
        copies[i].size = kRangeBytes;
    }
    vkCmdCopyBuffer(command, source.handle, destination.handle, 2, copies);
    VkBufferMemoryBarrier barriers[2]{};
    for (size_t i = 0; i < 2; ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].buffer = destination.handle;
        barriers[i].offset = destination_offsets[i];
        barriers[i].size = kRangeBytes;
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                         2, barriers, 0, nullptr);
    VK_CHECK(vkEndCommandBuffer(command));

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000000000ull));

    VkMappedMemoryRange invalidate_ranges[2]{};
    for (size_t i = 0; i < 2; ++i) {
        invalidate_ranges[i].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        invalidate_ranges[i].memory = destination.memory;
        invalidate_ranges[i].offset = destination_offsets[i];
        invalidate_ranges[i].size = range_size;
    }
    VK_CHECK(vkInvalidateMappedMemoryRanges(device, 2, invalidate_ranges));

    std::array<uint8_t, kOracleBytes> actual{};
    const auto* destination_bytes = static_cast<const uint8_t*>(destination_mapping);
    for (size_t range = 0; range < 2; ++range) {
        std::memcpy(actual.data() + range * static_cast<size_t>(kRangeBytes),
                    destination_bytes + destination_offsets[range],
                    static_cast<size_t>(kRangeBytes));
    }
    size_t mismatch = actual.size();
    for (size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != pattern_byte(i)) {
            mismatch = i;
            break;
        }
    }
    const uint32_t actual_checksum = checksum(actual);

    vkUnmapMemory(device, destination.memory);
    vkUnmapMemory(device, source.memory);
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool, 1, &command);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyBuffer(device, destination.handle, nullptr);
    vkFreeMemory(device, destination.memory, nullptr);
    vkDestroyBuffer(device, source.handle, nullptr);
    vkFreeMemory(device, source.memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    if (mismatch != actual.size()) {
        std::fprintf(stderr, "FAIL: byte %zu expected %02x, got %02x\n",
                     mismatch, pattern_byte(mismatch), actual[mismatch]);
        return 1;
    }
    if (actual_checksum != kExpectedChecksum) {
        std::fprintf(stderr, "FAIL: checksum expected %08x, got %08x\n",
                     kExpectedChecksum, actual_checksum);
        return 1;
    }
    std::printf("PASS: noncoherent-memory checksum=%08x bytes=%zu ranges=2\n",
                actual_checksum, actual.size());
    return 0;
}
