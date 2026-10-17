// Deterministic Vulkan 1.0 regression for batched queue submissions, command
// buffer re-recording, and fence reset/reuse. Three primary command buffers
// write disjoint ranges of one host-visible buffer using transfer commands.

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
constexpr uint32_t kFirstFill = 0x11111111u;
constexpr uint32_t kSecondFill = 0x22222222u;
constexpr uint32_t kMiddleFill = 0xabcdef01u;
constexpr uint32_t kExpectedChecksum = 0xcf2969b4u;
constexpr std::array<uint32_t, 32> kTail = {
    0x10203040u, 0x55667788u, 0x90abcdefu, 0x13579bdfu,
    0x2468ace0u, 0x0badf00du, 0xc001d00du, 0xfeedfaceu,
    0x31415926u, 0x27182818u, 0xaaaaaaaa, 0x55555555u,
    0x01234567u, 0x89abcdefu, 0xfedcba98u, 0x76543210u,
    0x00000000u, 0xffffffffu, 0x01010101u, 0x10101010u,
    0x11223344u, 0x44332211u, 0xdeadbeefu, 0xdecafbadu,
    0x80000000u, 0x7fffffffu, 0x00010001u, 0x10001000u,
    0x55aa55aau, 0xaa55aa55u, 0x12345678u, 0x87654321u,
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

uint32_t expected_word(uint32_t index) {
    if (index < 16) return kSecondFill;
    if (index < 32) return kMiddleFill;
    return kTail[index - 32];
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

void begin(VkCommandBuffer command) {
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    VK_CHECK(vkBeginCommandBuffer(command, &info));
}

void record_fill(VkCommandBuffer command, VkBuffer buffer, uint32_t first_word,
                 uint32_t word_count, uint32_t value) {
    begin(command);
    vkCmdFillBuffer(command, buffer, first_word * sizeof(uint32_t),
                    word_count * sizeof(uint32_t), value);
    VK_CHECK(vkEndCommandBuffer(command));
}

void record_tail(VkCommandBuffer command, VkBuffer buffer,
                 const std::array<uint32_t, 32>& values) {
    begin(command);
    vkCmdUpdateBuffer(command, buffer, 32 * sizeof(uint32_t),
                      values.size() * sizeof(uint32_t), values.data());
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = kBufferSize;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                         1, &barrier, 0, nullptr);
    VK_CHECK(vkEndCommandBuffer(command));
}

void submit_batch(VkQueue queue, const std::array<VkCommandBuffer, 3>& commands,
                  VkFence fence) {
    VkSubmitInfo submits[2]{};
    submits[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submits[0].commandBufferCount = 1;
    submits[0].pCommandBuffers = commands.data();
    submits[1].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submits[1].commandBufferCount = 1;
    submits[1].pCommandBuffers = commands.data() + 1;
    VK_CHECK(vkQueueSubmit(queue, 2, submits, VK_NULL_HANDLE));

    VkSubmitInfo tail_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    tail_submit.commandBufferCount = 1;
    tail_submit.pCommandBuffers = commands.data() + 2;
    // No host wait separates these calls. Queue order makes the fenced tail
    // complete only after both submit infos (and both CBs) in the first call.
    VK_CHECK(vkQueueSubmit(queue, 1, &tail_submit, fence));
}

}  // namespace

int main(int argc, char** argv) {
    bool validate = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-validate") == 0) validate = false;
        else if (std::strcmp(argv[i], "--validate") == 0) validate = true;
        else {
            std::fprintf(stderr, "usage: %s [--validate|--no-validate]\n", argv[0]);
            return 2;
        }
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "multi-submit-reuse-regression";
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

    Buffer buffer;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = kBufferSize;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &buffer.handle));
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = host_memory_type(
        physical, requirements.memoryTypeBits, &buffer.coherent);
    buffer.allocation_size = requirements.size;
    VK_CHECK(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory));
    VK_CHECK(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0));

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    std::array<VkCommandBuffer, 3> commands{};
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = static_cast<uint32_t>(commands.size());
    VK_CHECK(vkAllocateCommandBuffers(device, &command_info, commands.data()));

    record_fill(commands[0], buffer.handle, 0, 16, kFirstFill);
    record_fill(commands[1], buffer.handle, 16, 16, kMiddleFill);
    record_tail(commands[2], buffer.handle, kTail);

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));
    VK_CHECK(vkGetFenceStatus(device, fence));
    VK_CHECK(vkResetFences(device, 1, &fence));
    if (vkGetFenceStatus(device, fence) != VK_NOT_READY) {
        std::fprintf(stderr, "FAIL: reset fence was not VK_NOT_READY\n");
        return 1;
    }

    submit_batch(queue, commands, fence);
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000000000ull));
    VK_CHECK(vkGetFenceStatus(device, fence));

    // Preserve command 1, but reset and re-record commands 0 and 2.
    VK_CHECK(vkResetCommandBuffer(commands[0], 0));
    VK_CHECK(vkResetCommandBuffer(commands[2], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));
    record_fill(commands[0], buffer.handle, 0, 16, kSecondFill);
    record_tail(commands[2], buffer.handle, kTail);
    VK_CHECK(vkResetFences(device, 1, &fence));
    if (vkGetFenceStatus(device, fence) != VK_NOT_READY) {
        std::fprintf(stderr, "FAIL: reused fence was not VK_NOT_READY after reset\n");
        return 1;
    }
    submit_batch(queue, commands, fence);
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000000000ull));
    VK_CHECK(vkGetFenceStatus(device, fence));

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device, buffer.memory, 0, buffer.allocation_size, 0, &mapped));
    if (!buffer.coherent) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = buffer.memory;
        range.offset = 0;
        const VkDeviceSize atom = properties.limits.nonCoherentAtomSize;
        const VkDeviceSize aligned = (kBufferSize + atom - 1) / atom * atom;
        range.size = aligned <= buffer.allocation_size ? aligned : VK_WHOLE_SIZE;
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
    vkUnmapMemory(device, buffer.memory);

    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool, static_cast<uint32_t>(commands.size()), commands.data());
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyBuffer(device, buffer.handle, nullptr);
    vkFreeMemory(device, buffer.memory, nullptr);
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
    std::printf("PASS: multi-submit-reuse checksum=%08x words=%u submits=4\n",
                actual_checksum, kWordCount);
    return 0;
}
