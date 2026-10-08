#pragma once

// Struct marshalling for Vulkan CreateInfo-shaped structs, built on top of
// wire.hpp's Writer/Reader.
//
// Base Vulkan 1.0 structs retain their established ABI-local encoding. The
// extension closure uses the bounded, context-specific and field-wise pNext
// codecs below: native pointers and arbitrary native struct bytes never enter
// that encoding.
//
// Non-dispatchable handles inside a struct travel as the raw bits already in
// that field: on the client, a "handle" IS the id the server needs, so the
// struct's raw byte copy already carries it. The read side turns that id back
// into a real handle via HandleResolver. An id of 0 must resolve to
// VK_NULL_HANDLE; callers of a resolver method here never pass a 0 id.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include "wire.hpp"

namespace remoting {

// Backing storage for anything a read_* function allocates (arrays, strings,
// single POD structs pointed to by an optional field). Pointers returned by
// read_* remain valid only as long as this Arena is alive.
class Arena {
   public:
    void* allocate(size_t bytes) {
        if (bytes == 0) return nullptr;
        auto block = std::make_unique<char[]>(bytes);
        void* p = block.get();
        m_blocks.push_back(std::move(block));
        return p;
    }

    template <typename T>
    T* array(size_t count) {
        return static_cast<T*>(allocate(sizeof(T) * count));
    }

   private:
    std::vector<std::unique_ptr<char[]>> m_blocks;
};

// A single array on the wire is never trusted past this many elements; a
// legitimate CreateInfo never approaches it, so it only ever rejects a
// hostile or desynchronised peer.
constexpr uint32_t kMaxArrayElements = 1u << 20;
constexpr uint32_t kMaxPNextNodes = 16;
constexpr uint32_t kMaxPNextDepth = 4;
// Keep shader requests comfortably below the common 64 MiB message cap after
// their device handle and field-wise CreateInfo envelope are added.
constexpr uint64_t kMaxShaderCodeBytes = 16u * 1024u * 1024u;


// Implemented on the server, where handles are real driver objects. Resolves
// an id that travelled over the wire back to the handle it names. Passing 0
// is a caller bug, not a valid lookup - the caller is expected to have
// already special-cased id 0 as VK_NULL_HANDLE.
struct HandleResolver {
    virtual ~HandleResolver() = default;
    virtual VkBuffer buffer(uint64_t id) const = 0;
    virtual VkImage image(uint64_t id) const = 0;
    virtual VkImageView image_view(uint64_t id) const = 0;
    virtual VkBufferView buffer_view(uint64_t id) const = 0;
    virtual VkDeviceMemory memory(uint64_t id) const = 0;
    virtual VkRenderPass render_pass(uint64_t id) const = 0;
    virtual VkFramebuffer framebuffer(uint64_t id) const = 0;
    virtual VkShaderModule shader_module(uint64_t id) const = 0;
    virtual VkPipeline pipeline(uint64_t id) const = 0;
    virtual VkPipelineLayout pipeline_layout(uint64_t id) const = 0;
    virtual VkPipelineCache pipeline_cache(uint64_t id) const = 0;
    virtual VkDescriptorSetLayout descriptor_set_layout(uint64_t id) const = 0;
    virtual VkDescriptorPool descriptor_pool(uint64_t id) const = 0;
    virtual VkDescriptorSet descriptor_set(uint64_t id) const = 0;
    virtual VkSampler sampler(uint64_t id) const = 0;
    virtual VkSamplerYcbcrConversion sampler_ycbcr_conversion(uint64_t id) const = 0;
    virtual VkSemaphore semaphore(uint64_t id) const = 0;
    virtual VkFence fence(uint64_t id) const = 0;
    virtual VkEvent event(uint64_t id) const = 0;
    virtual VkQueryPool query_pool(uint64_t id) const = 0;
    virtual VkSurfaceKHR surface(uint64_t id) const = 0;
    virtual VkSwapchainKHR swapchain(uint64_t id) const = 0;
    virtual VkCommandPool command_pool(uint64_t id) const = 0;
    virtual VkCommandBuffer command_buffer(uint64_t id) const = 0;
};

// pNext is deliberately not a universal codec. Each call-site selects one
// semantic context, and every context has a closed allowlist in marshal.cpp.
enum class PNextContext : uint32_t {
    QueryFeaturesOutput,
    QueryPropertiesOutput,
    ImageFormatPropertiesOutput,
    DeviceCreate,
    RenderPass,
    RenderPass2,
    GraphicsPipeline,
    TessellationState,
    ImageCreate,
    ImageViewCreate,
    SamplerCreate,
    MemoryRequirements2,
    BindBufferMemory2,
    BindImageMemory2,
    DescriptorSetLayoutSupport,
    FramebufferCreate,
    RenderPassBegin,
    SemaphoreCreate,
    Submit,
    BufferCreate,
    MemoryAllocate,
    // VkShaderModuleCreateInfo currently permits no transported extension
    // structures. A non-null chain is rejected rather than silently dropped.
    ShaderModule,
};

// Input-chain codec. Returns false for an unknown node, a duplicate singleton,
// an invalid sType, excessive node/depth/count bounds, or malformed wire data.
bool write_pnext_chain(Writer& w, PNextContext context, const void* head,
                       uint32_t depth = 0);
bool read_pnext_chain(Reader& r, Arena& arena, const HandleResolver& hr,
                      PNextContext context, const void** out_head, uint32_t depth = 0);

// Output chains remain caller-owned. The request carries only their topology;
// the reply carries field values. Applying a reply fills matching nodes in
// place and never rewires the caller's pNext pointers.
bool write_pnext_output_topology(Writer& w, PNextContext context, const void* head);
bool read_pnext_output_topology(Reader& r, Arena& arena, PNextContext context,
                                void** out_head);
bool write_pnext_output_values(Writer& w, PNextContext context, const void* head);
bool apply_pnext_output_values(Reader& r, PNextContext context, void* caller_head);

// Group A: structs with arrays and/or handles beyond sType/pNext.
void write_DeviceQueueCreateInfo(Writer& w, const VkDeviceQueueCreateInfo& s);
bool read_DeviceQueueCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkDeviceQueueCreateInfo* out);

bool write_DeviceCreateInfo(Writer& w, const VkDeviceCreateInfo& s);
bool read_DeviceCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                            VkDeviceCreateInfo* out);

void write_SwapchainCreateInfoKHR(Writer& w, const VkSwapchainCreateInfoKHR& s);
bool read_SwapchainCreateInfoKHR(Reader& r, Arena& arena, const HandleResolver& hr,
                                  VkSwapchainCreateInfoKHR* out);

bool write_ImageViewCreateInfo(Writer& w, const VkImageViewCreateInfo& s);
bool read_ImageViewCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkImageViewCreateInfo* out);

bool write_RenderPassCreateInfo(Writer& w, const VkRenderPassCreateInfo& s);
bool read_RenderPassCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                VkRenderPassCreateInfo* out);
bool write_RenderPassCreateInfo2(Writer& w, const VkRenderPassCreateInfo2& s);
bool read_RenderPassCreateInfo2(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkRenderPassCreateInfo2* out);

bool write_ShaderModuleCreateInfo(Writer& w, const VkShaderModuleCreateInfo& s);
bool read_ShaderModuleCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkShaderModuleCreateInfo* out);

bool write_BufferViewCreateInfo(Writer& w, const VkBufferViewCreateInfo& s);
bool read_BufferViewCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkBufferViewCreateInfo* out);
void write_QueryPoolCreateInfo(Writer& w, const VkQueryPoolCreateInfo& s);
bool read_QueryPoolCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                              VkQueryPoolCreateInfo* out);
void write_EventCreateInfo(Writer& w, const VkEventCreateInfo& s);
bool read_EventCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                          VkEventCreateInfo* out);

void write_PipelineLayoutCreateInfo(Writer& w, const VkPipelineLayoutCreateInfo& s);
bool read_PipelineLayoutCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                    VkPipelineLayoutCreateInfo* out);

bool write_GraphicsPipelineCreateInfo(Writer& w, const VkGraphicsPipelineCreateInfo& s);
bool read_GraphicsPipelineCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                      VkGraphicsPipelineCreateInfo* out);

void write_ComputePipelineCreateInfo(Writer& w, const VkComputePipelineCreateInfo& s);
bool read_ComputePipelineCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                     VkComputePipelineCreateInfo* out);

bool write_FramebufferCreateInfo(Writer& w, const VkFramebufferCreateInfo& s);
bool read_FramebufferCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkFramebufferCreateInfo* out);

void write_CommandBufferAllocateInfo(Writer& w, const VkCommandBufferAllocateInfo& s);
bool read_CommandBufferAllocateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                     VkCommandBufferAllocateInfo* out);

void write_CommandBufferBeginInfo(Writer& w, const VkCommandBufferBeginInfo& s);
bool read_CommandBufferBeginInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                  VkCommandBufferBeginInfo* out);

bool write_RenderPassBeginInfo(Writer& w, const VkRenderPassBeginInfo& s);
bool read_RenderPassBeginInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkRenderPassBeginInfo* out);

bool write_SubpassBeginInfo(Writer& w, const VkSubpassBeginInfo& s);
bool read_SubpassBeginInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                           VkSubpassBeginInfo* out);
bool write_SubpassEndInfo(Writer& w, const VkSubpassEndInfo& s);
bool read_SubpassEndInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                         VkSubpassEndInfo* out);

void write_DescriptorSetLayoutCreateInfo(Writer& w, const VkDescriptorSetLayoutCreateInfo& s);
bool read_DescriptorSetLayoutCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                         VkDescriptorSetLayoutCreateInfo* out);

void write_DescriptorPoolCreateInfo(Writer& w, const VkDescriptorPoolCreateInfo& s);
bool read_DescriptorPoolCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                    VkDescriptorPoolCreateInfo* out);

void write_DescriptorSetAllocateInfo(Writer& w, const VkDescriptorSetAllocateInfo& s);
bool read_DescriptorSetAllocateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                     VkDescriptorSetAllocateInfo* out);

void write_WriteDescriptorSet(Writer& w, const VkWriteDescriptorSet& s);
bool read_WriteDescriptorSet(Reader& r, Arena& arena, const HandleResolver& hr,
                              VkWriteDescriptorSet* out);

void write_CopyDescriptorSet(Writer& w, const VkCopyDescriptorSet& s);
bool read_CopyDescriptorSet(Reader& r, Arena& arena, const HandleResolver& hr,
                             VkCopyDescriptorSet* out);

bool write_DescriptorUpdateTemplateCreateInfo(Writer& w,
    const VkDescriptorUpdateTemplateCreateInfo& s);
bool read_DescriptorUpdateTemplateCreateInfo(Reader& r, Arena& arena,
    const HandleResolver& hr, VkDescriptorUpdateTemplateCreateInfo* out);
void write_SamplerYcbcrConversionCreateInfo(Writer& w,
    const VkSamplerYcbcrConversionCreateInfo& s);
bool read_SamplerYcbcrConversionCreateInfo(Reader& r, Arena& arena,
    const HandleResolver& hr, VkSamplerYcbcrConversionCreateInfo* out);

void write_ImageMemoryBarrier(Writer& w, const VkImageMemoryBarrier& s);
bool read_ImageMemoryBarrier(Reader& r, Arena& arena, const HandleResolver& hr,
                              VkImageMemoryBarrier* out);

void write_BufferMemoryBarrier(Writer& w, const VkBufferMemoryBarrier& s);
bool read_BufferMemoryBarrier(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkBufferMemoryBarrier* out);

void write_MemoryBarrier(Writer& w, const VkMemoryBarrier& s);
bool read_MemoryBarrier(Reader& r, Arena& arena, const HandleResolver& hr, VkMemoryBarrier* out);

// Group B: raw-copy structs. VkBufferCreateInfo/VkImageCreateInfo carry a
// pQueueFamilyIndices array so they get array handling too, despite the
// grouping.
void write_BufferCreateInfo(Writer& w, const VkBufferCreateInfo& s);
bool read_BufferCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                            VkBufferCreateInfo* out);

bool write_ImageCreateInfo(Writer& w, const VkImageCreateInfo& s);
bool read_ImageCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                           VkImageCreateInfo* out);

void write_MemoryAllocateInfo(Writer& w, const VkMemoryAllocateInfo& s);
bool read_MemoryAllocateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                              VkMemoryAllocateInfo* out);

void write_SamplerCreateInfo(Writer& w, const VkSamplerCreateInfo& s);
bool read_SamplerCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                             VkSamplerCreateInfo* out);

void write_FenceCreateInfo(Writer& w, const VkFenceCreateInfo& s);
bool read_FenceCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                           VkFenceCreateInfo* out);

void write_SemaphoreCreateInfo(Writer& w, const VkSemaphoreCreateInfo& s);
bool read_SemaphoreCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkSemaphoreCreateInfo* out);

void write_CommandPoolCreateInfo(Writer& w, const VkCommandPoolCreateInfo& s);
bool read_CommandPoolCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkCommandPoolCreateInfo* out);

void write_PipelineCacheCreateInfo(Writer& w, const VkPipelineCacheCreateInfo& s);
bool read_PipelineCacheCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                   VkPipelineCacheCreateInfo* out);

void write_BufferImageCopy(Writer& w, const VkBufferImageCopy& s);
bool read_BufferImageCopy(Reader& r, Arena& arena, const HandleResolver& hr,
                           VkBufferImageCopy* out);

void write_ImageCopy(Writer& w, const VkImageCopy& s);
bool read_ImageCopy(Reader& r, Arena& arena, const HandleResolver& hr, VkImageCopy* out);

void write_ImageBlit(Writer& w, const VkImageBlit& s);
bool read_ImageBlit(Reader& r, Arena& arena, const HandleResolver& hr, VkImageBlit* out);

void write_BufferCopy(Writer& w, const VkBufferCopy& s);
bool read_BufferCopy(Reader& r, Arena& arena, const HandleResolver& hr, VkBufferCopy* out);

void write_Viewport(Writer& w, const VkViewport& s);
bool read_Viewport(Reader& r, Arena& arena, const HandleResolver& hr, VkViewport* out);

void write_Rect2D(Writer& w, const VkRect2D& s);
bool read_Rect2D(Reader& r, Arena& arena, const HandleResolver& hr, VkRect2D* out);

void write_MemoryRequirements(Writer& w, const VkMemoryRequirements& s);
bool read_MemoryRequirements(Reader& r, Arena& arena, const HandleResolver& hr,
                              VkMemoryRequirements* out);

void write_SubresourceLayout(Writer& w, const VkSubresourceLayout& s);
bool read_SubresourceLayout(Reader& r, Arena& arena, const HandleResolver& hr,
                             VkSubresourceLayout* out);

void write_SurfaceCapabilitiesKHR(Writer& w, const VkSurfaceCapabilitiesKHR& s);
bool read_SurfaceCapabilitiesKHR(Reader& r, Arena& arena, const HandleResolver& hr,
                                  VkSurfaceCapabilitiesKHR* out);

void write_SurfaceFormatKHR(Writer& w, const VkSurfaceFormatKHR& s);
bool read_SurfaceFormatKHR(Reader& r, VkSurfaceFormatKHR* out);

void write_PhysicalDeviceProperties(Writer& w, const VkPhysicalDeviceProperties& s);
bool read_PhysicalDeviceProperties(Reader& r, VkPhysicalDeviceProperties* out);

void write_PhysicalDeviceMemoryProperties(Writer& w, const VkPhysicalDeviceMemoryProperties& s);
bool read_PhysicalDeviceMemoryProperties(Reader& r, VkPhysicalDeviceMemoryProperties* out);

void write_PhysicalDeviceFeatures(Writer& w, const VkPhysicalDeviceFeatures& s);
bool read_PhysicalDeviceFeatures(Reader& r, VkPhysicalDeviceFeatures* out);

void write_FormatProperties(Writer& w, const VkFormatProperties& s);
bool read_FormatProperties(Reader& r, VkFormatProperties* out);
void write_ImageFormatProperties(Writer& w, const VkImageFormatProperties& s);
bool read_ImageFormatProperties(Reader& r, VkImageFormatProperties* out);
void write_QueueFamilyProperties(Writer& w, const VkQueueFamilyProperties& s);
bool read_QueueFamilyProperties(Reader& r, VkQueueFamilyProperties* out);
void write_SparseImageFormatProperties(Writer& w, const VkSparseImageFormatProperties& s);
bool read_SparseImageFormatProperties(Reader& r, VkSparseImageFormatProperties* out);

void write_ExtensionProperties(Writer& w, const VkExtensionProperties& s);
bool read_ExtensionProperties(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkExtensionProperties* out);

}  // namespace remoting
