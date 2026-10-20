// Deterministic Vulkan 1.2 regression for queue handle identity and cross-queue
// semaphore dependencies. A binary semaphore carries a GPU buffer write from
// queue 0 to a copy on queue 1 whose fence makes coherent host readback safe.
// A second chain submits a timeline wait before the matching signal.

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

constexpr VkDeviceSize kBytes = 64;
constexpr uint32_t kPattern = 0x13579bdfu;
constexpr uint32_t kExpectedChecksum = 0x7f184285u;
constexpr uint64_t kTimeout = 5ull * 1000000000ull;

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
};

uint32_t checksum(const uint8_t* bytes, size_t count) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
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
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-validate") == 0) validate = false;
        else if (std::strcmp(argv[i], "--validate") == 0) validate = true;
        else {
            std::fprintf(stderr, "usage: %s [--no-validate]\n", argv[0]);
            return 2;
        }
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "queue-sync-chain-regression";
    application.apiVersion = VK_API_VERSION_1_2;
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
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()));

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    for (VkPhysicalDevice candidate : physical_devices) {
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &timeline;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!timeline.timelineSemaphore) continue;

        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        for (uint32_t i = 0; i < count; ++i) {
            if (families[i].queueCount >= 2 &&
                (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
                physical = candidate;
                family = i;
                break;
            }
        }
        if (physical != VK_NULL_HANDLE) break;
    }
    if (physical == VK_NULL_HANDLE) {
        std::fprintf(stderr, "FAIL: no timeline-capable transfer family with two queues\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    std::array<float, 2> priorities{1.0f, 1.0f};
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = static_cast<uint32_t>(priorities.size());
    queue_info.pQueuePriorities = priorities.data();
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline_feature.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &timeline_feature;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(physical, &device_info, nullptr, &device));

    std::array<VkQueue, 4> queue0{};
    std::array<VkQueue, 4> queue1{};
    for (VkQueue& queue : queue0) vkGetDeviceQueue(device, family, 0, &queue);
    for (VkQueue& queue : queue1) vkGetDeviceQueue(device, family, 1, &queue);
    if (queue0[0] == VK_NULL_HANDLE || queue1[0] == VK_NULL_HANDLE ||
        queue0[0] != queue0[1] || queue0[1] != queue0[2] || queue0[2] != queue0[3] ||
        queue1[0] != queue1[1] || queue1[1] != queue1[2] || queue1[2] != queue1[3] ||
        queue0[0] == queue1[0]) {
        std::fprintf(stderr, "FAIL: repeated vkGetDeviceQueue identity mismatch\n");
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    Buffer source = create_buffer(device);
    Buffer destination = create_buffer(device);
    VkMemoryRequirements source_requirements{};
    VkMemoryRequirements destination_requirements{};
    vkGetBufferMemoryRequirements(device, source.handle, &source_requirements);
    vkGetBufferMemoryRequirements(device, destination.handle, &destination_requirements);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const uint32_t compatible = source_requirements.memoryTypeBits &
                                destination_requirements.memoryTypeBits;
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((compatible & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & required) == required) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == UINT32_MAX) {
        std::fprintf(stderr, "FAIL: coherent host-visible memory unavailable\n");
        return 1;
    }
    auto allocate = [&](Buffer& buffer, const VkMemoryRequirements& requirements) {
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = requirements.size;
        info.memoryTypeIndex = memory_type;
        buffer.allocation_size = requirements.size;
        VK_CHECK(vkAllocateMemory(device, &info, nullptr, &buffer.memory));
        VK_CHECK(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0));
    };
    allocate(source, source_requirements);
    allocate(destination, destination_requirements);
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device, destination.memory, 0, destination.allocation_size, 0, &mapped));
    std::memset(mapped, 0, static_cast<size_t>(kBytes));

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 2;
    std::array<VkCommandBuffer, 2> commands{};
    VK_CHECK(vkAllocateCommandBuffers(device, &command_info, commands.data()));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(commands[0], &begin));
    vkCmdFillBuffer(commands[0], source.handle, 0, kBytes, kPattern);
    VK_CHECK(vkEndCommandBuffer(commands[0]));

    VK_CHECK(vkBeginCommandBuffer(commands[1], &begin));
    VkBufferCopy copy{};
    copy.size = kBytes;
    vkCmdCopyBuffer(commands[1], source.handle, destination.handle, 1, &copy);
    VkBufferMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.buffer = destination.handle;
    host_barrier.offset = 0;
    host_barrier.size = kBytes;
    vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                         1, &host_barrier, 0, nullptr);
    VK_CHECK(vkEndCommandBuffer(commands[1]));

    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore binary = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSemaphore(device, &semaphore_info, nullptr, &binary));
    VkSubmitInfo producer{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    producer.commandBufferCount = 1;
    producer.pCommandBuffers = &commands[0];
    producer.signalSemaphoreCount = 1;
    producer.pSignalSemaphores = &binary;
    VK_CHECK(vkQueueSubmit(queue0[0], 1, &producer, VK_NULL_HANDLE));

    VkPipelineStageFlags transfer_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo consumer{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    consumer.waitSemaphoreCount = 1;
    consumer.pWaitSemaphores = &binary;
    consumer.pWaitDstStageMask = &transfer_stage;
    consumer.commandBufferCount = 1;
    consumer.pCommandBuffers = &commands[1];
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence binary_fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &binary_fence));
    VK_CHECK(vkQueueSubmit(queue1[0], 1, &consumer, binary_fence));
    VK_CHECK(vkWaitForFences(device, 1, &binary_fence, VK_TRUE, kTimeout));
    const uint32_t actual_checksum = checksum(static_cast<const uint8_t*>(mapped),
                                              static_cast<size_t>(kBytes));

    VkSemaphoreTypeCreateInfo type_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = 0;
    VkSemaphoreCreateInfo timeline_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    timeline_info.pNext = &type_info;
    VkSemaphore timeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSemaphore(device, &timeline_info, nullptr, &timeline));

    uint64_t wait_value = 2;
    uint64_t completion_value = 3;
    VkTimelineSemaphoreSubmitInfo wait_values{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    wait_values.waitSemaphoreValueCount = 1;
    wait_values.pWaitSemaphoreValues = &wait_value;
    wait_values.signalSemaphoreValueCount = 1;
    wait_values.pSignalSemaphoreValues = &completion_value;
    VkPipelineStageFlags top_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo pending_wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    pending_wait.pNext = &wait_values;
    pending_wait.waitSemaphoreCount = 1;
    pending_wait.pWaitSemaphores = &timeline;
    pending_wait.pWaitDstStageMask = &top_stage;
    pending_wait.signalSemaphoreCount = 1;
    pending_wait.pSignalSemaphores = &timeline;
    VkFence timeline_fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &timeline_fence));
    VK_CHECK(vkQueueSubmit(queue1[0], 1, &pending_wait, timeline_fence));

    uint64_t signal_value = 2;
    VkTimelineSemaphoreSubmitInfo signal_values{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    signal_values.signalSemaphoreValueCount = 1;
    signal_values.pSignalSemaphoreValues = &signal_value;
    VkSubmitInfo later_signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    later_signal.pNext = &signal_values;
    later_signal.signalSemaphoreCount = 1;
    later_signal.pSignalSemaphores = &timeline;
    VK_CHECK(vkQueueSubmit(queue0[0], 1, &later_signal, VK_NULL_HANDLE));
    VK_CHECK(vkWaitForFences(device, 1, &timeline_fence, VK_TRUE, kTimeout));
    uint64_t counter = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(device, timeline, &counter));

    vkDestroyFence(device, timeline_fence, nullptr);
    vkDestroySemaphore(device, timeline, nullptr);
    vkDestroyFence(device, binary_fence, nullptr);
    vkDestroySemaphore(device, binary, nullptr);
    vkUnmapMemory(device, destination.memory);
    vkFreeCommandBuffers(device, pool, static_cast<uint32_t>(commands.size()), commands.data());
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyBuffer(device, destination.handle, nullptr);
    vkFreeMemory(device, destination.memory, nullptr);
    vkDestroyBuffer(device, source.handle, nullptr);
    vkFreeMemory(device, source.memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    if (actual_checksum != kExpectedChecksum) {
        std::fprintf(stderr, "FAIL: binary checksum expected %08x, got %08x\n",
                     kExpectedChecksum, actual_checksum);
        return 1;
    }
    if (counter != completion_value) {
        std::fprintf(stderr, "FAIL: timeline expected %llu, got %llu\n",
                     static_cast<unsigned long long>(completion_value),
                     static_cast<unsigned long long>(counter));
        return 1;
    }
    std::printf("PASS: queue-sync-chain identities=8 checksum=%08x timeline=%llu\n",
                actual_checksum, static_cast<unsigned long long>(counter));
    return 0;
}
