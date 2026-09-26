// Deterministic Vulkan 1.0 transfer regression. The command buffer fills two
// buffers, patches one with vkCmdUpdateBuffer, copies it into the other, and
// verifies the exact host-visible result after a transfer-to-host barrier.

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

constexpr uint32_t kWordCount = 64;
constexpr VkDeviceSize kBufferSize = kWordCount * sizeof(uint32_t);
constexpr uint32_t kSourceFill = 0x01020304u;
constexpr uint32_t kDestinationFill = 0xdeadbeefu;
constexpr uint32_t kCopyDestinationWord = 16;
constexpr uint32_t kCopyWordCount = 32;
constexpr uint32_t kRefillSourceWord = 4;
constexpr uint32_t kRefillWordCount = 4;
constexpr uint32_t kRefillValue = 0x89abcdefu;
constexpr uint32_t kPatchSourceWord = 8;
constexpr uint32_t kExpectedChecksum = 0x3b5a6394u;
constexpr std::array<uint32_t, 8> kPatch = {
    0x10203040u, 0x55667788u, 0x90abcdefu, 0x13579bdfu,
    0x2468ace0u, 0x0badf00du, 0xc001d00du, 0xfeedfaceu,
};

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
    bool coherent = false;
};

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

Buffer create_buffer(VkPhysicalDevice physical, VkDevice device) {
    Buffer buffer;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = kBufferSize;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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

uint32_t expected_word(uint32_t index) {
    if (index < kCopyDestinationWord ||
        index >= kCopyDestinationWord + kCopyWordCount) {
        return kDestinationFill;
    }
    const uint32_t source_index = index - kCopyDestinationWord;
    if (source_index >= kPatchSourceWord &&
        source_index < kPatchSourceWord + kPatch.size()) {
        return kPatch[source_index - kPatchSourceWord];
    }
    if (source_index >= kRefillSourceWord &&
        source_index < kRefillSourceWord + kRefillWordCount) {
        return kRefillValue;
    }
    return kSourceFill;
}

uint32_t checksum(const uint32_t* words) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < kWordCount; ++i) {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            hash ^= (words[i] >> shift) & 0xffu;
            hash *= 16777619u;
        }
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
    application.pApplicationName = "buffer-ops-regression";
    application.apiVersion = VK_API_VERSION_1_0;
    std::vector<const char*> layers;
    if (validate) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        for (const auto& layer : available) {
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
            }
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
        return 1;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()));
    VkPhysicalDevice physical = physical_devices[0];

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

    Buffer source = create_buffer(physical, device);
    Buffer destination = create_buffer(physical, device);

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
    vkCmdFillBuffer(command, source.handle, 0, kBufferSize, kSourceFill);
    VkBufferMemoryBarrier source_fill_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    source_fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    source_fill_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    source_fill_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_fill_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_fill_barrier.buffer = source.handle;
    source_fill_barrier.offset = 0;
    source_fill_barrier.size = kBufferSize;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         1, &source_fill_barrier, 0, nullptr);

    // A second partial fill remains observable after the update and copy. This
    // prevents the fill regression from becoming a no-op hidden by a later
    // full-range overwrite.
    vkCmdFillBuffer(command, source.handle,
                    kRefillSourceWord * sizeof(uint32_t),
                    kRefillWordCount * sizeof(uint32_t), kRefillValue);
    vkCmdUpdateBuffer(command, source.handle,
                      kPatchSourceWord * sizeof(uint32_t),
                      kPatch.size() * sizeof(uint32_t), kPatch.data());
    vkCmdFillBuffer(command, destination.handle, 0, kBufferSize, kDestinationFill);
    VkBufferMemoryBarrier before_copy[2]{};
    before_copy[0] = source_fill_barrier;
    before_copy[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before_copy[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before_copy[1] = source_fill_barrier;
    before_copy[1].buffer = destination.handle;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         2, before_copy, 0, nullptr);

    VkBufferCopy copy{};
    copy.srcOffset = 0;
    copy.dstOffset = kCopyDestinationWord * sizeof(uint32_t);
    copy.size = kCopyWordCount * sizeof(uint32_t);
    vkCmdCopyBuffer(command, source.handle, destination.handle, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = destination.handle;
    barrier.offset = 0;
    barrier.size = kBufferSize;
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
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000000000ull));

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device, destination.memory, 0, destination.allocation_size, 0, &mapped));
    if (!destination.coherent) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = destination.memory;
        range.offset = 0;
        const VkDeviceSize aligned =
            (kBufferSize + properties.limits.nonCoherentAtomSize - 1) /
            properties.limits.nonCoherentAtomSize * properties.limits.nonCoherentAtomSize;
        range.size = aligned <= destination.allocation_size ? aligned : VK_WHOLE_SIZE;
        VK_CHECK(vkInvalidateMappedMemoryRanges(device, 1, &range));
    }
    const auto* words = static_cast<const uint32_t*>(mapped);
    uint32_t mismatch = kWordCount;
    for (uint32_t i = 0; i < kWordCount; ++i) {
        if (words[i] != expected_word(i)) {
            mismatch = i;
            break;
        }
    }
    const uint32_t actual_checksum = checksum(words);
    const uint32_t actual = mismatch == kWordCount ? 0 : words[mismatch];
    const uint32_t expected = mismatch == kWordCount ? 0 : expected_word(mismatch);
    vkUnmapMemory(device, destination.memory);

    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool, 1, &command);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyBuffer(device, destination.handle, nullptr);
    vkFreeMemory(device, destination.memory, nullptr);
    vkDestroyBuffer(device, source.handle, nullptr);
    vkFreeMemory(device, source.memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    if (mismatch != kWordCount) {
        std::fprintf(stderr, "FAIL: word %u expected %08x, got %08x\n",
                     mismatch, expected, actual);
        return 1;
    }
    if (actual_checksum != kExpectedChecksum) {
        std::fprintf(stderr, "FAIL: checksum expected %08x, got %08x\n",
                     kExpectedChecksum, actual_checksum);
        return 1;
    }
    std::printf("PASS: buffer-ops checksum=%08x words=%u\n",
                actual_checksum, kWordCount);
    return 0;
}
