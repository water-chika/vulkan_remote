// Ordinary Vulkan application: renders a red triangle into an offscreen
// 256x256 image using only the real system Vulkan loader/headers, reads it
// back to host memory, and writes it out as a PPM file.
//
// This program knows nothing about this repository's remoting protocol; it
// only depends on <vulkan/vulkan.h> and the Vulkan loader (Vulkan::Vulkan).
//
// SPIR-V generation note: the vertSpirv/fragSpirv arrays below were produced
// by compiling the corresponding GLSL sources with:
//   glslc -mfmt=c -o tri.vert.h tri.vert
//   glslc -mfmt=c -o tri.frag.h tri.frag
// and validated with `spirv-val`. The GLSL sources were:
//
//   tri.vert:
//     #version 450
//     const vec2 positions[3] = vec2[](
//         vec2(0.0, -0.8),
//         vec2(0.8, 0.8),
//         vec2(-0.8, 0.8)
//     );
//     void main() {
//         gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
//     }
//
//   tri.frag:
//     #version 450
//     layout(location=0) out vec4 outColor;
//     void main() {
//         outColor = vec4(1.0, 0.0, 0.0, 1.0);
//     }

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const uint32_t vertSpirv[] = {
    0x07230203,0x00010000,0x000d000b,0x00000028,
    0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,
    0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000000,0x00000004,0x6e69616d,
    0x00000000,0x0000000d,0x0000001b,0x00030003,
    0x00000002,0x000001c2,0x000a0004,0x475f4c47,
    0x4c474f4f,0x70635f45,0x74735f70,0x5f656c79,
    0x656e696c,0x7269645f,0x69746365,0x00006576,
    0x00080004,0x475f4c47,0x4c474f4f,0x6e695f45,
    0x64756c63,0x69645f65,0x74636572,0x00657669,
    0x00040005,0x00000004,0x6e69616d,0x00000000,
    0x00060005,0x0000000b,0x505f6c67,0x65567265,
    0x78657472,0x00000000,0x00060006,0x0000000b,
    0x00000000,0x505f6c67,0x7469736f,0x006e6f69,
    0x00070006,0x0000000b,0x00000001,0x505f6c67,
    0x746e696f,0x657a6953,0x00000000,0x00070006,
    0x0000000b,0x00000002,0x435f6c67,0x4470696c,
    0x61747369,0x0065636e,0x00070006,0x0000000b,
    0x00000003,0x435f6c67,0x446c6c75,0x61747369,
    0x0065636e,0x00030005,0x0000000d,0x00000000,
    0x00060005,0x0000001b,0x565f6c67,0x65747265,
    0x646e4978,0x00007865,0x00050005,0x0000001e,
    0x65646e69,0x6c626178,0x00000065,0x00030047,
    0x0000000b,0x00000002,0x00050048,0x0000000b,
    0x00000000,0x0000000b,0x00000000,0x00050048,
    0x0000000b,0x00000001,0x0000000b,0x00000001,
    0x00050048,0x0000000b,0x00000002,0x0000000b,
    0x00000003,0x00050048,0x0000000b,0x00000003,
    0x0000000b,0x00000004,0x00040047,0x0000001b,
    0x0000000b,0x0000002a,0x00020013,0x00000002,
    0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,
    0x00000006,0x00000004,0x00040015,0x00000008,
    0x00000020,0x00000000,0x0004002b,0x00000008,
    0x00000009,0x00000001,0x0004001c,0x0000000a,
    0x00000006,0x00000009,0x0006001e,0x0000000b,
    0x00000007,0x00000006,0x0000000a,0x0000000a,
    0x00040020,0x0000000c,0x00000003,0x0000000b,
    0x0004003b,0x0000000c,0x0000000d,0x00000003,
    0x00040015,0x0000000e,0x00000020,0x00000001,
    0x0004002b,0x0000000e,0x0000000f,0x00000000,
    0x00040017,0x00000010,0x00000006,0x00000002,
    0x0004002b,0x00000008,0x00000011,0x00000003,
    0x0004001c,0x00000012,0x00000010,0x00000011,
    0x0004002b,0x00000006,0x00000013,0x00000000,
    0x0004002b,0x00000006,0x00000014,0xbf4ccccd,
    0x0005002c,0x00000010,0x00000015,0x00000013,
    0x00000014,0x0004002b,0x00000006,0x00000016,
    0x3f4ccccd,0x0005002c,0x00000010,0x00000017,
    0x00000016,0x00000016,0x0005002c,0x00000010,
    0x00000018,0x00000014,0x00000016,0x0006002c,
    0x00000012,0x00000019,0x00000015,0x00000017,
    0x00000018,0x00040020,0x0000001a,0x00000001,
    0x0000000e,0x0004003b,0x0000001a,0x0000001b,
    0x00000001,0x00040020,0x0000001d,0x00000007,
    0x00000012,0x00040020,0x0000001f,0x00000007,
    0x00000010,0x0004002b,0x00000006,0x00000022,
    0x3f800000,0x00040020,0x00000026,0x00000003,
    0x00000007,0x00050036,0x00000002,0x00000004,
    0x00000000,0x00000003,0x000200f8,0x00000005,
    0x0004003b,0x0000001d,0x0000001e,0x00000007,
    0x0004003d,0x0000000e,0x0000001c,0x0000001b,
    0x0003003e,0x0000001e,0x00000019,0x00050041,
    0x0000001f,0x00000020,0x0000001e,0x0000001c,
    0x0004003d,0x00000010,0x00000021,0x00000020,
    0x00050051,0x00000006,0x00000023,0x00000021,
    0x00000000,0x00050051,0x00000006,0x00000024,
    0x00000021,0x00000001,0x00070050,0x00000007,
    0x00000025,0x00000023,0x00000024,0x00000013,
    0x00000022,0x00050041,0x00000026,0x00000027,
    0x0000000d,0x0000000f,0x0003003e,0x00000027,
    0x00000025,0x000100fd,0x00010038
};

static const uint32_t fragSpirv[] = {
    0x07230203,0x00010000,0x000d000b,0x0000000d,
    0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,
    0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0006000f,0x00000004,0x00000004,0x6e69616d,
    0x00000000,0x00000009,0x00030010,0x00000004,
    0x00000007,0x00030003,0x00000002,0x000001c2,
    0x000a0004,0x475f4c47,0x4c474f4f,0x70635f45,
    0x74735f70,0x5f656c79,0x656e696c,0x7269645f,
    0x69746365,0x00006576,0x00080004,0x475f4c47,
    0x4c474f4f,0x6e695f45,0x64756c63,0x69645f65,
    0x74636572,0x00657669,0x00040005,0x00000004,
    0x6e69616d,0x00000000,0x00050005,0x00000009,
    0x4374756f,0x726f6c6f,0x00000000,0x00040047,
    0x00000009,0x0000001e,0x00000000,0x00020013,
    0x00000002,0x00030021,0x00000003,0x00000002,
    0x00030016,0x00000006,0x00000020,0x00040017,
    0x00000007,0x00000006,0x00000004,0x00040020,
    0x00000008,0x00000003,0x00000007,0x0004003b,
    0x00000008,0x00000009,0x00000003,0x0004002b,
    0x00000006,0x0000000a,0x3f800000,0x0004002b,
    0x00000006,0x0000000b,0x00000000,0x0007002c,
    0x00000007,0x0000000c,0x0000000a,0x0000000b,
    0x0000000b,0x0000000a,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,
    0x00000005,0x0003003e,0x00000009,0x0000000c,
    0x000100fd,0x00010038
};

#define VK_CHECK(call)                                                        \
    do {                                                                      \
        VkResult _vkres = (call);                                            \
        if (_vkres != VK_SUCCESS) {                                          \
            fprintf(stderr, "FATAL: %s failed with VkResult=%d\n", #call,    \
                    (int)_vkres);                                             \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static const uint32_t kWidth = 256;
static const uint32_t kHeight = 256;

static uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits,
                                VkMemoryPropertyFlags wantFlags,
                                bool allowFallbackAnyMatchingBit) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & wantFlags) == wantFlags) {
            return i;
        }
    }
    if (allowFallbackAnyMatchingBit) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if (typeBits & (1u << i)) {
                return i;
            }
        }
    }
    fprintf(stderr, "FATAL: no suitable memory type found (typeBits=0x%x)\n",
            typeBits);
    exit(1);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(stderr, "offscreen: validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

int main(int argc, char** argv) {
    // Validation is on by default: this program is the independent second
    // driver of our ICD the task asked for, and the layer catches handle-
    // lifetime/return-code/entry-point bugs in OUR driver that a well-behaved
    // application would never trip. --no-validate turns it off (e.g. to
    // confirm a failure is validation-only and not a real functional bug).
    const char* outPath = "/tmp/offscreen_output.ppm";
    bool validate = true;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-validate") == 0) {
            validate = false;
        } else if (strcmp(argv[i], "--validate") == 0) {
            validate = true;
        } else {
            outPath = argv[i];
        }
    }

    fprintf(stderr, "creating instance...\n");
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "offscreen-triangle";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "offscreen-triangle";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    if (validate) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availLayers.data());
        bool found = false;
        for (const auto& l : availLayers) {
            if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) found = true;
        }
        if (found) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            fprintf(stderr, "offscreen: VK_LAYER_KHRONOS_validation enabled\n");
        } else {
            fprintf(stderr,
                    "offscreen: --validate requested but VK_LAYER_KHRONOS_validation is not "
                    "available; continuing without it\n");
        }
    }

    VkInstanceCreateInfo instInfo{};
    instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instInfo.pApplicationInfo = &appInfo;
    instInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instInfo.ppEnabledLayerNames = layers.data();
    instInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instInfo.ppEnabledExtensionNames = extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&instInfo, nullptr, &instance));

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (!extensions.empty()) {
        auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createFn) {
            VkDebugUtilsMessengerCreateInfoEXT dbgInfo{};
            dbgInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dbgInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbgInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbgInfo.pfnUserCallback = debugCallback;
            createFn(instance, &dbgInfo, nullptr, &messenger);
        }
    }

    uint32_t physCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physCount, nullptr));
    if (physCount == 0) {
        fprintf(stderr, "FATAL: no Vulkan physical devices found\n");
        exit(1);
    }
    std::vector<VkPhysicalDevice> physDevices(physCount);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physCount, physDevices.data()));
    VkPhysicalDevice phys = physDevices[0];

    VkPhysicalDeviceProperties physProps;
    vkGetPhysicalDeviceProperties(phys, &physProps);
    fprintf(stderr, "physical device: %s\n", physProps.deviceName);

    uint32_t queueFamCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &queueFamCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFams(queueFamCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &queueFamCount, queueFams.data());

    uint32_t graphicsFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamCount; ++i) {
        if (queueFams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = i;
            break;
        }
    }
    if (graphicsFamily == UINT32_MAX) {
        fprintf(stderr, "FATAL: no graphics queue family found\n");
        exit(1);
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = graphicsFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo devInfo{};
    devInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devInfo.queueCreateInfoCount = 1;
    devInfo.pQueueCreateInfos = &queueInfo;

    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(phys, &devInfo, nullptr, &device));

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, graphicsFamily, 0, &queue);
    fprintf(stderr, "device+queue ready\n");

    // Image, view, render pass, framebuffer.
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = {kWidth, kHeight, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImage(device, &imgInfo, nullptr, &image));

    VkMemoryRequirements imgMemReq;
    vkGetImageMemoryRequirements(device, image, &imgMemReq);
    uint32_t imgMemType = findMemoryType(
        phys, imgMemReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);

    VkMemoryAllocateInfo imgAllocInfo{};
    imgAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imgAllocInfo.allocationSize = imgMemReq.size;
    imgAllocInfo.memoryTypeIndex = imgMemType;

    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(device, &imgAllocInfo, nullptr, &imageMemory));
    VK_CHECK(vkBindImageMemory(device, image, imageMemory, 0));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &imageView));

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &attachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(device, &rpInfo, nullptr, &renderPass));

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &imageView;
    fbInfo.width = kWidth;
    fbInfo.height = kHeight;
    fbInfo.layers = 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer));
    fprintf(stderr, "image/view/renderpass/framebuffer ready\n");

    // Shaders + pipeline.
    VkShaderModuleCreateInfo vertModInfo{};
    vertModInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertModInfo.codeSize = sizeof(vertSpirv);
    vertModInfo.pCode = vertSpirv;
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &vertModInfo, nullptr, &vertModule));

    VkShaderModuleCreateInfo fragModInfo{};
    fragModInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragModInfo.codeSize = sizeof(fragSpirv);
    fragModInfo.pCode = fragSpirv;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &fragModInfo, nullptr, &fragModule));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkDescriptorSetLayoutCreateInfo dsLayoutInfo{};
    dsLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsLayoutInfo.bindingCount = 0;
    dsLayoutInfo.pBindings = nullptr;
    VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &dsLayoutInfo, nullptr, &dsLayout));

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &dsLayout;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout));

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{0.0f, 0.0f, (float)kWidth, (float)kHeight, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {kWidth, kHeight}};

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterState{};
    rasterState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterState.cullMode = VK_CULL_MODE_NONE;
    rasterState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterState.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlendState{};
    colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = &blendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterState;
    pipelineInfo.pMultisampleState = &multisampleState;
    pipelineInfo.pDepthStencilState = nullptr;
    pipelineInfo.pColorBlendState = &colorBlendState;
    pipelineInfo.pDynamicState = nullptr;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                        nullptr, &pipeline));
    fprintf(stderr, "shaders/pipeline ready\n");

    // Command pool/buffer.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = graphicsFamily;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool));

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = cmdPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmdBuf));

    // Readback buffer.
    VkDeviceSize bufSize = (VkDeviceSize)kWidth * kHeight * 4;
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = bufSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer readbackBuffer = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(device, &bufInfo, nullptr, &readbackBuffer));

    VkMemoryRequirements bufMemReq;
    vkGetBufferMemoryRequirements(device, readbackBuffer, &bufMemReq);
    uint32_t bufMemType = findMemoryType(
        phys, bufMemReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true);

    VkMemoryAllocateInfo bufAllocInfo{};
    bufAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufAllocInfo.allocationSize = bufMemReq.size;
    bufAllocInfo.memoryTypeIndex = bufMemType;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(device, &bufAllocInfo, nullptr, &bufferMemory));
    VK_CHECK(vkBindBufferMemory(device, readbackBuffer, bufferMemory, 0));

    // Record command buffer.
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmdBuf, &beginInfo));

    VkClearValue clearValue{};
    clearValue.color = {{0.0f, 0.0f, 1.0f, 1.0f}};

    VkRenderPassBeginInfo rpBeginInfo{};
    rpBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBeginInfo.renderPass = renderPass;
    rpBeginInfo.framebuffer = framebuffer;
    rpBeginInfo.renderArea.offset = {0, 0};
    rpBeginInfo.renderArea.extent = {kWidth, kHeight};
    rpBeginInfo.clearValueCount = 1;
    rpBeginInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmdBuf, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(cmdBuf, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmdBuf);
    fprintf(stderr, "recorded triangle draw\n");

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &barrier);

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = {0, 0, 0};
    copyRegion.imageExtent = {kWidth, kHeight, 1};

    vkCmdCopyImageToBuffer(cmdBuf, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            readbackBuffer, 1, &copyRegion);

    VK_CHECK(vkEndCommandBuffer(cmdBuf));

    // Submit + wait.
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &fence));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;

    VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
    fprintf(stderr, "submitted, waiting on fence\n");
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000000000ull));

    // Readback.
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
    const uint8_t* pixels = reinterpret_cast<const uint8_t*>(mapped);

    fprintf(stderr, "readback done, writing %s\n", outPath);
    FILE* f = fopen(outPath, "wb");
    if (!f) {
        fprintf(stderr, "FATAL: could not open %s for writing\n", outPath);
        exit(1);
    }
    fprintf(f, "P6\n%u %u\n255\n", kWidth, kHeight);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint8_t* p = pixels + (y * kWidth + x) * 4;
            uint8_t rgb[3] = {p[0], p[1], p[2]};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);

    auto pixelAt = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return pixels + (y * kWidth + x) * 4;
    };
    const uint8_t* corner = pixelAt(10, 10);
    const uint8_t* center = pixelAt(128, 128);

    bool cornerOk = (corner[0] <= 10) && (corner[1] <= 10) && (corner[2] >= 245);
    bool centerOk = (center[0] >= 245) && (center[1] <= 10) && (center[2] <= 10);

    // Snapshot the pixel bytes before unmapping; the pointers above alias
    // mapped device memory that becomes invalid after vkUnmapMemory.
    uint8_t cornerBytes[4] = {corner[0], corner[1], corner[2], corner[3]};
    uint8_t centerBytes[4] = {center[0], center[1], center[2], center[3]};

    vkUnmapMemory(device, bufferMemory);

    fprintf(stderr,
            "corner(10,10) = (%d,%d,%d,%d), center(128,128) = (%d,%d,%d,%d)\n",
            cornerBytes[0], cornerBytes[1], cornerBytes[2], cornerBytes[3],
            centerBytes[0], centerBytes[1], centerBytes[2], centerBytes[3]);

    // Cleanup, reverse creation order.
    vkDestroyFence(device, fence, nullptr);
    vkDestroyBuffer(device, readbackBuffer, nullptr);
    vkFreeMemory(device, bufferMemory, nullptr);
    vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);
    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyImageView(device, imageView, nullptr);
    vkFreeMemory(device, imageMemory, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkDestroyDevice(device, nullptr);
    if (messenger != VK_NULL_HANDLE) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(instance, messenger, nullptr);
    }
    vkDestroyInstance(instance, nullptr);

    if (cornerOk && centerOk) {
        printf("PASS: corner(10,10)=(%d,%d,%d) center(128,128)=(%d,%d,%d)\n",
               cornerBytes[0], cornerBytes[1], cornerBytes[2], centerBytes[0],
               centerBytes[1], centerBytes[2]);
        return 0;
    }

    printf("FAIL: corner(10,10)=(%d,%d,%d) center(128,128)=(%d,%d,%d)\n",
           cornerBytes[0], cornerBytes[1], cornerBytes[2], centerBytes[0],
           centerBytes[1], centerBytes[2]);
    return 1;
}
