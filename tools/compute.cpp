// Ordinary Vulkan 1.0 application that transforms a fixed integer vector in a
// storage buffer with a compute shader, synchronizes it back to the host, and
// verifies every element. Successful runs print one stable checksum.
//
// The embedded SPIR-V was generated and validated with:
//   glslc --target-env=vulkan1.0 -O compute.comp -o compute.spv
//   spirv-val --target-env vulkan1.0 compute.spv
// Compute shader:
//   #version 450
//   layout(local_size_x = 8) in;
//   layout(set = 0, binding = 0, std430) buffer Data { uint values[]; } data;
//   void main() {
//       uint i = gl_GlobalInvocationID.x;
//       data.values[i] = (data.values[i] * 1664525u + 1013904223u) ^
//                        (i * 2246822519u);
//   }

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define VK_CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "FATAL: %s failed with VkResult=%d\n", #call, int(r_)); exit(1); \
} } while (0)

static constexpr uint32_t kElementCount = 64;
static constexpr uint32_t kLocalSize = 8;
static constexpr uint32_t kExpectedChecksum = 0xead71172u;

static const uint32_t kComputeSpirv[] = {
0x07230203,0x00010000,0x000d000b,0x00000027,0x00000000,0x00020011,0x00000001,0x0006000b,
0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
0x0006000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x0000000b,0x00060010,0x00000004,
0x00000011,0x00000008,0x00000001,0x00000001,0x00040047,0x0000000b,0x0000000b,0x0000001c,
0x00040047,0x00000010,0x00000006,0x00000004,0x00030047,0x00000011,0x00000003,0x00050048,
0x00000011,0x00000000,0x00000023,0x00000000,0x00040047,0x00000013,0x00000021,0x00000000,
0x00040047,0x00000013,0x00000022,0x00000000,0x00040047,0x00000026,0x0000000b,0x00000019,
0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00040015,0x00000006,0x00000020,
0x00000000,0x00040017,0x00000009,0x00000006,0x00000003,0x00040020,0x0000000a,0x00000001,
0x00000009,0x0004003b,0x0000000a,0x0000000b,0x00000001,0x0004002b,0x00000006,0x0000000c,
0x00000000,0x00040020,0x0000000d,0x00000001,0x00000006,0x0003001d,0x00000010,0x00000006,
0x0003001e,0x00000011,0x00000010,0x00040020,0x00000012,0x00000002,0x00000011,0x0004003b,
0x00000012,0x00000013,0x00000002,0x00040015,0x00000014,0x00000020,0x00000001,0x0004002b,
0x00000014,0x00000015,0x00000000,0x00040020,0x00000018,0x00000002,0x00000006,0x0004002b,
0x00000006,0x0000001b,0x0019660d,0x0004002b,0x00000006,0x0000001d,0x3c6ef35f,0x0004002b,
0x00000006,0x00000020,0x85ebca77,0x0004002b,0x00000006,0x00000024,0x00000008,0x0004002b,
0x00000006,0x00000025,0x00000001,0x0006002c,0x00000009,0x00000026,0x00000024,0x00000025,
0x00000025,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,
0x00050041,0x0000000d,0x0000000e,0x0000000b,0x0000000c,0x0004003d,0x00000006,0x0000000f,
0x0000000e,0x00060041,0x00000018,0x00000019,0x00000013,0x00000015,0x0000000f,0x0004003d,
0x00000006,0x0000001a,0x00000019,0x00050084,0x00000006,0x0000001c,0x0000001a,0x0000001b,
0x00050080,0x00000006,0x0000001e,0x0000001c,0x0000001d,0x00050084,0x00000006,0x00000021,
0x0000000f,0x00000020,0x000500c6,0x00000006,0x00000022,0x0000001e,0x00000021,0x0003003e,
0x00000019,0x00000022,0x000100fd,0x00010038
};

static uint32_t hostMemoryType(VkPhysicalDevice physical, uint32_t bits,
                               bool& coherent) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags preferred = required | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags choices[] = {preferred, required};
    for (VkMemoryPropertyFlags flags : choices) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & flags) == flags) {
                coherent = (properties.memoryTypes[i].propertyFlags &
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                return i;
            }
        }
    }
    fprintf(stderr, "FATAL: no host-visible memory type\n");
    exit(1);
}

static uint32_t inputValue(uint32_t index) {
    return index * 0x9e3779b9u ^ 0xa5a5a5a5u;
}

static uint32_t expectedValue(uint32_t index) {
    return (inputValue(index) * 1664525u + 1013904223u) ^
           (index * 2246822519u);
}

static uint32_t checksum(const uint32_t* values) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < kElementCount; ++i) {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            hash ^= (values[i] >> shift) & 0xffu;
            hash *= 16777619u;
        }
    }
    return hash;
}

int main(int argc, char** argv) {
    bool validate = true;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-validate") == 0) validate = false;
        else if (strcmp(argv[i], "--validate") == 0) validate = true;
        else {
            fprintf(stderr, "usage: %s [--no-validate]\n", argv[0]);
            return 2;
        }
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "compute-regression";
    application.apiVersion = VK_API_VERSION_1_0;
    std::vector<const char*> layers;
    if (validate) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        for (const auto& layer : available) {
            if (strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                layers.push_back("VK_LAYER_KHRONOS_validation");
        }
    }
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledLayerCount = uint32_t(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &instance));

    uint32_t physicalCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr));
    if (!physicalCount) {
        fprintf(stderr, "FATAL: no Vulkan physical device\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physicalCount, physicalDevices.data()));
    VkPhysicalDevice physical = physicalDevices[0];

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    }
    if (family == UINT32_MAX) {
        fprintf(stderr, "FATAL: no compute queue\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    VkDevice device;
    VK_CHECK(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    const VkDeviceSize bufferSize = sizeof(uint32_t) * kElementCount;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer;
    VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    bool memoryCoherent = false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = hostMemoryType(
        physical, requirements.memoryTypeBits, memoryCoherent);
    VkDeviceMemory memory;
    VK_CHECK(vkAllocateMemory(device, &allocation, nullptr, &memory));
    VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));

    VkPhysicalDeviceProperties physicalProperties{};
    vkGetPhysicalDeviceProperties(physical, &physicalProperties);
    const VkDeviceSize atomSize = physicalProperties.limits.nonCoherentAtomSize;
    const VkDeviceSize alignedSize =
        (bufferSize + atomSize - 1) / atomSize * atomSize;
    VkMappedMemoryRange hostRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    hostRange.memory = memory;
    hostRange.offset = 0;
    hostRange.size = alignedSize <= allocation.allocationSize ?
                     alignedSize : VK_WHOLE_SIZE;

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device, memory, 0, allocation.allocationSize, 0, &mapped));
    auto* values = static_cast<uint32_t*>(mapped);
    for (uint32_t i = 0; i < kElementCount; ++i) values[i] = inputValue(i);
    if (!memoryCoherent)
        VK_CHECK(vkFlushMappedMemoryRanges(device, 1, &hostRange));
    vkUnmapMemory(device, memory);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &binding;
    VkDescriptorSetLayout setLayout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout));
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool descriptorPool;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool));
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &setLayout;
    VkDescriptorSet descriptorSet;
    VK_CHECK(vkAllocateDescriptorSets(device, &setInfo, &descriptorSet));
    VkDescriptorBufferInfo descriptorBuffer{buffer, 0, bufferSize};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &descriptorBuffer;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = sizeof(kComputeSpirv);
    shaderInfo.pCode = kComputeSpirv;
    VkShaderModule shader;
    VK_CHECK(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader));
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    VkPipelineLayout pipelineLayout;
    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout));
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = stage;
    pipelineInfo.layout = pipelineLayout;
    VkPipeline pipeline;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));

    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.queueFamilyIndex = family;
    VkCommandPool commandPool;
    VK_CHECK(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool));
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer;
    VK_CHECK(vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(commandBuffer, &begin));
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                            0, 1, &descriptorSet, 0, nullptr);
    vkCmdDispatch(commandBuffer, kElementCount / kLocalSize, 1, 1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
    VK_CHECK(vkEndCommandBuffer(commandBuffer));

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000000000ull));

    VK_CHECK(vkMapMemory(device, memory, 0, allocation.allocationSize, 0, &mapped));
    if (!memoryCoherent)
        VK_CHECK(vkInvalidateMappedMemoryRanges(device, 1, &hostRange));
    values = static_cast<uint32_t*>(mapped);
    uint32_t mismatch = kElementCount;
    for (uint32_t i = 0; i < kElementCount; ++i) {
        if (values[i] != expectedValue(i)) { mismatch = i; break; }
    }
    const uint32_t actualChecksum = checksum(values);
    const uint32_t actualMismatch = mismatch == kElementCount ? 0 : values[mismatch];
    const uint32_t expectedMismatch = mismatch == kElementCount ? 0 : expectedValue(mismatch);
    vkUnmapMemory(device, memory);

    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyShaderModule(device, shader, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    if (mismatch != kElementCount) {
        fprintf(stderr, "FAIL: element %u expected 0x%08x, got 0x%08x\n",
                mismatch, expectedMismatch, actualMismatch);
        return 1;
    }
    if (actualChecksum != kExpectedChecksum) {
        fprintf(stderr, "FAIL: checksum expected %08x, got %08x\n",
                kExpectedChecksum, actualChecksum);
        return 1;
    }
    printf("PASS: compute checksum=%08x elements=%u\n", actualChecksum, kElementCount);
    return 0;
}
