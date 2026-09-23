// Ordinary Vulkan 1.0 application that uploads a single-mip checkerboard
// through a staging buffer, samples it in a fullscreen draw, and reads the
// rendered image back. A positional argument optionally writes the result as
// PPM; pixel checks are always performed.
//
// The embedded SPIR-V was generated with glslc --target-env=vulkan1.0 -mfmt=c.
// Vertex shader:
//   #version 450
//   layout(location=0) out vec2 uv;
//   const vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
//   void main(){vec2 v=p[gl_VertexIndex];gl_Position=vec4(v,0,1);uv=v*.5+.5;}
// Fragment shader:
//   #version 450
//   layout(set=0,binding=0) uniform sampler2D checker;
//   layout(location=0) in vec2 uv; layout(location=0) out vec4 color;
//   void main(){color=texture(checker,uv);}

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

static constexpr uint32_t kTextureSize = 4;
static constexpr uint32_t kWidth = 256;
static constexpr uint32_t kHeight = 256;

static const uint32_t kVertSpirv[] = {
0x07230203,0x00010000,0x000d000b,0x00000031,0x00000000,0x00020011,0x00000001,0x0006000b,
0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
0x0008000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x00000015,0x00000020,0x0000002b,
0x00030003,0x00000002,0x000001c2,0x000a0004,0x475f4c47,0x4c474f4f,0x70635f45,0x74735f70,
0x5f656c79,0x656e696c,0x7269645f,0x69746365,0x00006576,0x00080004,0x475f4c47,0x4c474f4f,
0x6e695f45,0x64756c63,0x69645f65,0x74636572,0x00657669,0x00040005,0x00000004,0x6e69616d,
0x00000000,0x00030005,0x00000009,0x00000070,0x00060005,0x00000015,0x565f6c67,0x65747265,
0x646e4978,0x00007865,0x00050005,0x00000018,0x65646e69,0x6c626178,0x00000065,0x00060005,
0x0000001e,0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,0x0000001e,0x00000000,
0x505f6c67,0x7469736f,0x006e6f69,0x00070006,0x0000001e,0x00000001,0x505f6c67,0x746e696f,
0x657a6953,0x00000000,0x00070006,0x0000001e,0x00000002,0x435f6c67,0x4470696c,0x61747369,
0x0065636e,0x00070006,0x0000001e,0x00000003,0x435f6c67,0x446c6c75,0x61747369,0x0065636e,
0x00030005,0x00000020,0x00000000,0x00030005,0x0000002b,0x00007675,0x00040047,0x00000015,
0x0000000b,0x0000002a,0x00030047,0x0000001e,0x00000002,0x00050048,0x0000001e,0x00000000,
0x0000000b,0x00000000,0x00050048,0x0000001e,0x00000001,0x0000000b,0x00000001,0x00050048,
0x0000001e,0x00000002,0x0000000b,0x00000003,0x00050048,0x0000001e,0x00000003,0x0000000b,
0x00000004,0x00040047,0x0000002b,0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,
0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,
0x00000002,0x00040020,0x00000008,0x00000007,0x00000007,0x00040015,0x0000000a,0x00000020,
0x00000000,0x0004002b,0x0000000a,0x0000000b,0x00000003,0x0004001c,0x0000000c,0x00000007,
0x0000000b,0x0004002b,0x00000006,0x0000000d,0xbf800000,0x0005002c,0x00000007,0x0000000e,
0x0000000d,0x0000000d,0x0004002b,0x00000006,0x0000000f,0x40400000,0x0005002c,0x00000007,
0x00000010,0x0000000f,0x0000000d,0x0005002c,0x00000007,0x00000011,0x0000000d,0x0000000f,
0x0006002c,0x0000000c,0x00000012,0x0000000e,0x00000010,0x00000011,0x00040015,0x00000013,
0x00000020,0x00000001,0x00040020,0x00000014,0x00000001,0x00000013,0x0004003b,0x00000014,
0x00000015,0x00000001,0x00040020,0x00000017,0x00000007,0x0000000c,0x00040017,0x0000001b,
0x00000006,0x00000004,0x0004002b,0x0000000a,0x0000001c,0x00000001,0x0004001c,0x0000001d,
0x00000006,0x0000001c,0x0006001e,0x0000001e,0x0000001b,0x00000006,0x0000001d,0x0000001d,
0x00040020,0x0000001f,0x00000003,0x0000001e,0x0004003b,0x0000001f,0x00000020,0x00000003,
0x0004002b,0x00000013,0x00000021,0x00000000,0x0004002b,0x00000006,0x00000023,0x00000000,
0x0004002b,0x00000006,0x00000024,0x3f800000,0x00040020,0x00000028,0x00000003,0x0000001b,
0x00040020,0x0000002a,0x00000003,0x00000007,0x0004003b,0x0000002a,0x0000002b,0x00000003,
0x0004002b,0x00000006,0x0000002d,0x3f000000,0x00050036,0x00000002,0x00000004,0x00000000,
0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000008,0x00000009,0x00000007,0x0004003b,
0x00000017,0x00000018,0x00000007,0x0004003d,0x00000013,0x00000016,0x00000015,0x0003003e,
0x00000018,0x00000012,0x00050041,0x00000008,0x00000019,0x00000018,0x00000016,0x0004003d,
0x00000007,0x0000001a,0x00000019,0x0003003e,0x00000009,0x0000001a,0x0004003d,0x00000007,
0x00000022,0x00000009,0x00050051,0x00000006,0x00000025,0x00000022,0x00000000,0x00050051,
0x00000006,0x00000026,0x00000022,0x00000001,0x00070050,0x0000001b,0x00000027,0x00000025,
0x00000026,0x00000023,0x00000024,0x00050041,0x00000028,0x00000029,0x00000020,0x00000021,
0x0003003e,0x00000029,0x00000027,0x0004003d,0x00000007,0x0000002c,0x00000009,0x0005008e,
0x00000007,0x0000002e,0x0000002c,0x0000002d,0x00050050,0x00000007,0x0000002f,0x0000002d,
0x0000002d,0x00050081,0x00000007,0x00000030,0x0000002e,0x0000002f,0x0003003e,0x0000002b,
0x00000030,0x000100fd,0x00010038};

static const uint32_t kFragSpirv[] = {
0x07230203,0x00010000,0x000d000b,0x00000014,0x00000000,0x00020011,0x00000001,0x0006000b,
0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x00000011,0x00030010,
0x00000004,0x00000007,0x00030003,0x00000002,0x000001c2,0x000a0004,0x475f4c47,0x4c474f4f,
0x70635f45,0x74735f70,0x5f656c79,0x656e696c,0x7269645f,0x69746365,0x00006576,0x00080004,
0x475f4c47,0x4c474f4f,0x6e695f45,0x64756c63,0x69645f65,0x74636572,0x00657669,0x00040005,
0x00000004,0x6e69616d,0x00000000,0x00050005,0x00000009,0x4374756f,0x726f6c6f,0x00000000,
0x00040005,0x0000000d,0x63656863,0x0072656b,0x00030005,0x00000011,0x00007675,0x00040047,
0x00000009,0x0000001e,0x00000000,0x00040047,0x0000000d,0x00000021,0x00000000,0x00040047,
0x0000000d,0x00000022,0x00000000,0x00040047,0x00000011,0x0000001e,0x00000000,0x00020013,
0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,
0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,0x00000003,0x00000007,0x0004003b,
0x00000008,0x00000009,0x00000003,0x00090019,0x0000000a,0x00000006,0x00000001,0x00000000,
0x00000000,0x00000000,0x00000001,0x00000000,0x0003001b,0x0000000b,0x0000000a,0x00040020,
0x0000000c,0x00000000,0x0000000b,0x0004003b,0x0000000c,0x0000000d,0x00000000,0x00040017,
0x0000000f,0x00000006,0x00000002,0x00040020,0x00000010,0x00000001,0x0000000f,0x0004003b,
0x00000010,0x00000011,0x00000001,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,
0x000200f8,0x00000005,0x0004003d,0x0000000b,0x0000000e,0x0000000d,0x0004003d,0x0000000f,
0x00000012,0x00000011,0x00050057,0x00000007,0x00000013,0x0000000e,0x00000012,0x0003003e,
0x00000009,0x00000013,0x000100fd,0x00010038};

static uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits,
                           VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    fprintf(stderr, "FATAL: no memory type for flags 0x%x\n", flags);
    exit(1);
}

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

static Buffer makeBuffer(VkDevice device, VkPhysicalDevice physical, VkDeviceSize size,
                         VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    Buffer result;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &info, nullptr, &result.handle));
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, result.handle, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType(physical, requirements.memoryTypeBits, properties);
    VK_CHECK(vkAllocateMemory(device, &allocation, nullptr, &result.memory));
    VK_CHECK(vkBindBufferMemory(device, result.handle, result.memory, 0));
    return result;
}

int main(int argc, char** argv) {
    const char* output = "/tmp/texture_upload_output.ppm";
    bool validate = true;
    bool outputSpecified = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-validate") == 0) validate = false;
        else if (strcmp(argv[i], "--validate") == 0) validate = true;
        else if (strcmp(argv[i], "--no-output") == 0) {
            output = nullptr;
            outputSpecified = true;
        } else if (!outputSpecified) {
            output = argv[i];
            outputSpecified = true;
        } else {
            fprintf(stderr, "usage: %s [--no-validate] [--no-output | output.ppm]\n", argv[0]);
            return 2;
        }
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "texture-upload";
    application.apiVersion = VK_API_VERSION_1_0;
    std::vector<const char*> layers, extensions;
    if (validate) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        for (const auto& layer : available)
            if (strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledLayerCount = uint32_t(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    instanceInfo.enabledExtensionCount = uint32_t(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &instance));

    uint32_t physicalCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr));
    if (!physicalCount) { fprintf(stderr, "FATAL: no Vulkan physical device\n"); return 1; }
    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physicalCount, physicalDevices.data()));
    VkPhysicalDevice physical = physicalDevices[0];
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    if (family == UINT32_MAX) { fprintf(stderr, "FATAL: no graphics queue\n"); return 1; }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
    VkDevice device;
    VK_CHECK(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    std::array<uint8_t, kTextureSize * kTextureSize * 4> checker{};
    for (uint32_t y = 0; y < kTextureSize; ++y) for (uint32_t x = 0; x < kTextureSize; ++x) {
        uint8_t* p = &checker[(y * kTextureSize + x) * 4];
        bool red = ((x + y) & 1) == 0;
        p[0] = red ? 255 : 0; p[1] = red ? 0 : 255; p[2] = 0; p[3] = 255;
    }
    Buffer staging = makeBuffer(device, physical, checker.size(),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device, staging.memory, 0, checker.size(), 0, &mapped));
    memcpy(mapped, checker.data(), checker.size());
    vkUnmapMemory(device, staging.memory);

    auto makeImage = [&](uint32_t width, uint32_t height, VkImageUsageFlags usage,
                         VkImage& image, VkDeviceMemory& memory) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D; info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {width, height, 1}; info.mipLevels = 1; info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &info, nullptr, &image));
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(physical, requirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &allocation, nullptr, &memory));
        VK_CHECK(vkBindImageMemory(device, image, memory, 0));
    };
    VkImage texture, target;
    VkDeviceMemory textureMemory, targetMemory;
    makeImage(kTextureSize, kTextureSize,
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
              texture, textureMemory);
    makeImage(kWidth, kHeight,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
              target, targetMemory);

    auto makeView = [&](VkImage image) {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image; info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1; info.subresourceRange.layerCount = 1;
        VkImageView view; VK_CHECK(vkCreateImageView(device, &info, nullptr, &view)); return view;
    };
    VkImageView textureView = makeView(texture), targetView = makeView(target);
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST; samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    VkSampler sampler; VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &sampler));

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0; binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1; binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayoutInfo.bindingCount = 1; setLayoutInfo.pBindings = &binding;
    VkDescriptorSetLayout setLayout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout));
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool descriptorPool;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool));
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool; setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &setLayout;
    VkDescriptorSet descriptorSet; VK_CHECK(vkAllocateDescriptorSets(device, &setInfo, &descriptorSet));
    VkDescriptorImageInfo descriptorImage{sampler, textureView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptorSet; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descriptorImage;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &colorReference;
    VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = 1; renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1; renderPassInfo.pSubpasses = &subpass;
    VkRenderPass renderPass; VK_CHECK(vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass));
    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferInfo.renderPass = renderPass; framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &targetView; framebufferInfo.width = kWidth;
    framebufferInfo.height = kHeight; framebufferInfo.layers = 1;
    VkFramebuffer framebuffer;
    VK_CHECK(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer));

    auto shader = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytes; info.pCode = code;
        VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module)); return module;
    };
    VkShaderModule vertexModule = shader(kVertSpirv, sizeof(kVertSpirv));
    VkShaderModule fragmentModule = shader(kFragSpirv, sizeof(kFragSpirv));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertexModule; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragmentModule; stages[1].pName = "main";
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1; pipelineLayoutInfo.pSetLayouts = &setLayout;
    VkPipelineLayout pipelineLayout;
    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout));
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
    VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = viewportState.scissorCount = 1;
    viewportState.pViewports = &viewport; viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1; blend.pAttachments = &blendAttachment;
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2; pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput; pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewportState; pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample; pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.layout = pipelineLayout; pipelineInfo.renderPass = renderPass;
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));

    Buffer readback = makeBuffer(device, physical, VkDeviceSize(kWidth) * kHeight * 4,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.queueFamilyIndex = family;
    VkCommandPool commandPool; VK_CHECK(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool));
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer; VK_CHECK(vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(commandBuffer, &begin));

    VkImageMemoryBarrier textureToCopy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    textureToCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    textureToCopy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    textureToCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    textureToCopy.srcQueueFamilyIndex = textureToCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    textureToCopy.image = texture; textureToCopy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    textureToCopy.subresourceRange.levelCount = textureToCopy.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &textureToCopy);
    VkBufferImageCopy upload{};
    upload.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    upload.imageSubresource.layerCount = 1; upload.imageExtent = {kTextureSize, kTextureSize, 1};
    vkCmdCopyBufferToImage(commandBuffer, staging.handle, texture,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
    VkImageMemoryBarrier textureToShader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    textureToShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    textureToShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    textureToShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    textureToShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    textureToShader.srcQueueFamilyIndex = textureToShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    textureToShader.image = texture; textureToShader.subresourceRange = textureToCopy.subresourceRange;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &textureToShader);

    VkClearValue clear{};
    VkRenderPassBeginInfo renderBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    renderBegin.renderPass = renderPass; renderBegin.framebuffer = framebuffer;
    renderBegin.renderArea.extent = {kWidth, kHeight}; renderBegin.clearValueCount = 1;
    renderBegin.pClearValues = &clear;
    vkCmdBeginRenderPass(commandBuffer, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            0, 1, &descriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
    VkImageMemoryBarrier targetToCopy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    targetToCopy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    targetToCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    targetToCopy.oldLayout = targetToCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    targetToCopy.srcQueueFamilyIndex = targetToCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    targetToCopy.image = target; targetToCopy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    targetToCopy.subresourceRange.levelCount = targetToCopy.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &targetToCopy);
    VkBufferImageCopy download{};
    download.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    download.imageSubresource.layerCount = 1; download.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(commandBuffer, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.handle, 1, &download);
    VK_CHECK(vkEndCommandBuffer(commandBuffer));
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &commandBuffer;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000000000ull));

    VK_CHECK(vkMapMemory(device, readback.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    const uint8_t* pixels = static_cast<const uint8_t*>(mapped);
    auto pixel = [&](uint32_t x, uint32_t y) { return pixels + (y * kWidth + x) * 4; };
    const uint32_t coordinates[][2] = {{32,32}, {96,32}, {32,96}, {96,96}};
    bool ok = true;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t* p = pixel(coordinates[i][0], coordinates[i][1]);
        const bool expectRed = (i == 0 || i == 3);
        const uint8_t expected[4] = {uint8_t(expectRed ? 255 : 0),
                                     uint8_t(expectRed ? 0 : 255), 0, 255};
        ok &= memcmp(p, expected, 4) == 0;
    }
    if (output) {
        FILE* file = fopen(output, "wb");
        if (!file) { fprintf(stderr, "FATAL: cannot open %s\n", output); exit(1); }
        bool writeOk = fprintf(file, "P6\n%u %u\n255\n", kWidth, kHeight) > 0;
        for (uint32_t i = 0; writeOk && i < kWidth * kHeight; ++i) {
            writeOk = fwrite(pixels + i * 4, 1, 3, file) == 3;
        }
        if (fclose(file) != 0) writeOk = false;
        if (!writeOk) {
            std::remove(output);
            fprintf(stderr, "FATAL: could not write %s\n", output);
            exit(1);
        }
    }
    vkUnmapMemory(device, readback.memory);

    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyBuffer(device, readback.handle, nullptr); vkFreeMemory(device, readback.memory, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr); vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyShaderModule(device, fragmentModule, nullptr); vkDestroyShaderModule(device, vertexModule, nullptr);
    vkDestroyFramebuffer(device, framebuffer, nullptr); vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr); vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    vkDestroySampler(device, sampler, nullptr);
    vkDestroyImageView(device, targetView, nullptr); vkDestroyImage(device, target, nullptr); vkFreeMemory(device, targetMemory, nullptr);
    vkDestroyImageView(device, textureView, nullptr); vkDestroyImage(device, texture, nullptr); vkFreeMemory(device, textureMemory, nullptr);
    vkDestroyBuffer(device, staging.handle, nullptr); vkFreeMemory(device, staging.memory, nullptr);
    vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);

    printf("%s: 4x4 checkerboard sampled into %ux%u%s%s\n", ok ? "PASS" : "FAIL",
           kWidth, kHeight, output ? ", wrote " : "", output ? output : "");
    return ok ? 0 : 1;
}
