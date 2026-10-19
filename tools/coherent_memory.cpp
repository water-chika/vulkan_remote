// Deterministic Vulkan 1.0 regression for experimental HOST_COHERENT mapped
// memory. Both allocations remain mapped while two host-write -> GPU-copy ->
// host-read epochs run without vkFlushMappedMemoryRanges or
// vkInvalidateMappedMemoryRanges.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define VK_CHECK(call) do { VkResult result_ = (call); if (result_ != VK_SUCCESS) { \
    std::fprintf(stderr, "FATAL: %s failed with VkResult=%d\n", #call, int(result_)); \
    std::exit(1); \
} } while (0)

namespace {

constexpr size_t kBytes = 128;
constexpr uint32_t kEpochs = 2;
constexpr uint32_t kExpectedChecksum = 0xc3349e45u;
constexpr const char* kSkip = "SKIP: coherent host-visible memory unavailable";

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
};

uint8_t expected_byte(uint32_t epoch, size_t index) {
    return static_cast<uint8_t>((epoch * 83u + index * 37u + 11u) & 0xffu);
}

uint32_t checksum_byte(uint32_t hash, uint8_t byte) {
    hash ^= byte;
    return hash * 16777619u;
}

Buffer create_buffer(VkDevice device) {
    Buffer buffer;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = kBytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &info, nullptr, &buffer.handle));
    return buffer;
}

}  // namespace

int main(int argc, char** argv) {
    bool validate = true;
    bool require_coherent = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-validate") == 0) validate = false;
        else if (std::strcmp(argv[i], "--validate") == 0) validate = true;
        else if (std::strcmp(argv[i], "--require-coherent") == 0) require_coherent = true;
        else {
            std::fprintf(stderr, "usage: %s [--no-validate] [--require-coherent]\n", argv[0]);
            return 2;
        }
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "coherent-memory-regression";
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
    if (physical_count == 0) {
        std::fprintf(stderr, "FATAL: no Vulkan physical device\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()));
    const VkPhysicalDevice physical = physical_devices[0];

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

    Buffer source = create_buffer(device);
    Buffer destination = create_buffer(device);
    VkMemoryRequirements source_requirements{};
    VkMemoryRequirements destination_requirements{};
    vkGetBufferMemoryRequirements(device, source.handle, &source_requirements);
    vkGetBufferMemoryRequirements(device, destination.handle, &destination_requirements);

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    const uint32_t compatible = source_requirements.memoryTypeBits &
                                destination_requirements.memoryTypeBits;
    const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((compatible & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & required) == required) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == UINT32_MAX) {
        vkDestroyBuffer(device, destination.handle, nullptr);
        vkDestroyBuffer(device, source.handle, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        if (require_coherent) {
            std::fprintf(stderr, "FAIL: required coherent host-visible memory unavailable\n");
            return 1;
        }
        std::printf("%s\n", kSkip);
        return 0;
    }

    auto allocate = [&](Buffer& buffer, const VkMemoryRequirements& requirements) {
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        buffer.allocation_size = requirements.size;
        VK_CHECK(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory));
        VK_CHECK(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0));
    };
    allocate(source, source_requirements);
    allocate(destination, destination_requirements);

    void* source_mapping = nullptr;
    void* destination_mapping = nullptr;
    VK_CHECK(vkMapMemory(device, source.memory, 0, source.allocation_size, 0, &source_mapping));
    VK_CHECK(vkMapMemory(device, destination.memory, 0, destination.allocation_size, 0,
                         &destination_mapping));

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
    VkBufferCopy copy{};
    copy.size = kBytes;
    vkCmdCopyBuffer(command, source.handle, destination.handle, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = destination.handle;
    barrier.offset = 0;
    barrier.size = kBytes;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                         1, &barrier, 0, nullptr);
    VK_CHECK(vkEndCommandBuffer(command));

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;

    auto* source_bytes = static_cast<uint8_t*>(source_mapping);
    const auto* destination_bytes = static_cast<const uint8_t*>(destination_mapping);
    uint32_t actual_checksum = 2166136261u;
    size_t mismatch = kBytes;
    uint32_t mismatch_epoch = kEpochs;
    uint8_t mismatch_actual = 0;
    for (uint32_t epoch = 0; epoch < kEpochs; ++epoch) {
        for (size_t i = 0; i < kBytes; ++i)
            source_bytes[i] = expected_byte(epoch, i);

        if (epoch != 0) VK_CHECK(vkResetFences(device, 1, &fence));
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000000000ull));

        for (size_t i = 0; i < kBytes; ++i) {
            const uint8_t actual = destination_bytes[i];
            if (mismatch_epoch == kEpochs && actual != expected_byte(epoch, i)) {
                mismatch_epoch = epoch;
                mismatch = i;
                mismatch_actual = actual;
            }
            actual_checksum = checksum_byte(actual_checksum, actual);
        }
    }

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

    if (mismatch_epoch != kEpochs) {
        std::fprintf(stderr, "FAIL: epoch %u byte %zu expected %02x, got %02x\n",
                     mismatch_epoch, mismatch, expected_byte(mismatch_epoch, mismatch),
                     mismatch_actual);
        return 1;
    }
    if (actual_checksum != kExpectedChecksum) {
        std::fprintf(stderr, "FAIL: checksum expected %08x, got %08x\n",
                     kExpectedChecksum, actual_checksum);
        return 1;
    }
    std::printf("PASS: coherent-memory checksum=%08x bytes=%zu epochs=%u\n",
                actual_checksum, kBytes, kEpochs);
    return 0;
}
