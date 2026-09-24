#pragma once

// Struct marshalling for Vulkan CreateInfo-shaped structs, built on top of
// wire.hpp's Writer/Reader.
//
// Both peers are the same compiler/ABI on the same architecture (a handshake
// already checks they were generated from the same vk.xml), so instead of
// hand-copying every field we take a local copy of the struct with pNext
// nulled, raw-copy that, and separately serialise whatever it points to.
// Reading does the mirror image: read the raw struct, then allocate each
// pointed-to array from an Arena and patch the pointer to it.
//
// pNext chains are never sent. write_* silently drops pNext; this module
// only understands the base struct for each type, so any extension chain a
// caller attached is lost. There is no encoding for arbitrary pNext chains
// here, and none is planned - callers relying on an extension struct need a
// dedicated wire message, not this one.
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

// Implemented on the server, where handles are real driver objects. Resolves
// an id that travelled over the wire back to the handle it names. Passing 0
// is a caller bug, not a valid lookup - the caller is expected to have
// already special-cased id 0 as VK_NULL_HANDLE.
struct HandleResolver {
    virtual ~HandleResolver() = default;
    virtual VkBuffer buffer(uint64_t id) const = 0;
    virtual VkImage image(uint64_t id) const = 0;
    virtual VkImageView image_view(uint64_t id) const = 0;
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
    virtual VkSemaphore semaphore(uint64_t id) const = 0;
    virtual VkFence fence(uint64_t id) const = 0;
    virtual VkSurfaceKHR surface(uint64_t id) const = 0;
    virtual VkSwapchainKHR swapchain(uint64_t id) const = 0;
    virtual VkCommandPool command_pool(uint64_t id) const = 0;
    virtual VkCommandBuffer command_buffer(uint64_t id) const = 0;
};

// Group A: structs with arrays and/or handles beyond sType/pNext.
void write_DeviceQueueCreateInfo(Writer& w, const VkDeviceQueueCreateInfo& s);
bool read_DeviceQueueCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkDeviceQueueCreateInfo* out);

void write_DeviceCreateInfo(Writer& w, const VkDeviceCreateInfo& s);
bool read_DeviceCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                            VkDeviceCreateInfo* out);

void write_SwapchainCreateInfoKHR(Writer& w, const VkSwapchainCreateInfoKHR& s);
bool read_SwapchainCreateInfoKHR(Reader& r, Arena& arena, const HandleResolver& hr,
                                  VkSwapchainCreateInfoKHR* out);

void write_ImageViewCreateInfo(Writer& w, const VkImageViewCreateInfo& s);
bool read_ImageViewCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkImageViewCreateInfo* out);

void write_RenderPassCreateInfo(Writer& w, const VkRenderPassCreateInfo& s);
bool read_RenderPassCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                VkRenderPassCreateInfo* out);

void write_ShaderModuleCreateInfo(Writer& w, const VkShaderModuleCreateInfo& s);
bool read_ShaderModuleCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                  VkShaderModuleCreateInfo* out);

void write_PipelineLayoutCreateInfo(Writer& w, const VkPipelineLayoutCreateInfo& s);
bool read_PipelineLayoutCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                    VkPipelineLayoutCreateInfo* out);

void write_GraphicsPipelineCreateInfo(Writer& w, const VkGraphicsPipelineCreateInfo& s);
bool read_GraphicsPipelineCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                      VkGraphicsPipelineCreateInfo* out);

void write_ComputePipelineCreateInfo(Writer& w, const VkComputePipelineCreateInfo& s);
bool read_ComputePipelineCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                     VkComputePipelineCreateInfo* out);

void write_FramebufferCreateInfo(Writer& w, const VkFramebufferCreateInfo& s);
bool read_FramebufferCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkFramebufferCreateInfo* out);

void write_CommandBufferAllocateInfo(Writer& w, const VkCommandBufferAllocateInfo& s);
bool read_CommandBufferAllocateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                     VkCommandBufferAllocateInfo* out);

void write_CommandBufferBeginInfo(Writer& w, const VkCommandBufferBeginInfo& s);
bool read_CommandBufferBeginInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                  VkCommandBufferBeginInfo* out);

void write_RenderPassBeginInfo(Writer& w, const VkRenderPassBeginInfo& s);
bool read_RenderPassBeginInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkRenderPassBeginInfo* out);

void write_DescriptorSetLayoutCreateInfo(Writer& w, const VkDescriptorSetLayoutCreateInfo& s);
bool read_DescriptorSetLayoutCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                         VkDescriptorSetLayoutCreateInfo* out);

void write_DescriptorPoolCreateInfo(Writer& w, const VkDescriptorPoolCreateInfo& s);
bool read_DescriptorPoolCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                    VkDescriptorPoolCreateInfo* out);

void write_DescriptorSetAllocateInfo(Writer& w, const VkDescriptorSetAllocateInfo& s);
bool read_DescriptorSetAllocateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                     VkDescriptorSetAllocateInfo* out);

// pTexelBufferView (VkBufferView arrays) is rejected: this project never
// creates buffer views, so there is no resolver entry for them.
void write_WriteDescriptorSet(Writer& w, const VkWriteDescriptorSet& s);
bool read_WriteDescriptorSet(Reader& r, Arena& arena, const HandleResolver& hr,
                              VkWriteDescriptorSet* out);

void write_CopyDescriptorSet(Writer& w, const VkCopyDescriptorSet& s);
bool read_CopyDescriptorSet(Reader& r, Arena& arena, const HandleResolver& hr,
                             VkCopyDescriptorSet* out);

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

void write_ImageCreateInfo(Writer& w, const VkImageCreateInfo& s);
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
bool read_PhysicalDeviceMemoryProperties(Reader& r, Arena& arena, const HandleResolver& hr,
                                          VkPhysicalDeviceMemoryProperties* out);

void write_PhysicalDeviceFeatures(Writer& w, const VkPhysicalDeviceFeatures& s);
bool read_PhysicalDeviceFeatures(Reader& r, Arena& arena, const HandleResolver& hr,
                                  VkPhysicalDeviceFeatures* out);

void write_FormatProperties(Writer& w, const VkFormatProperties& s);
bool read_FormatProperties(Reader& r, Arena& arena, const HandleResolver& hr,
                            VkFormatProperties* out);

void write_ExtensionProperties(Writer& w, const VkExtensionProperties& s);
bool read_ExtensionProperties(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkExtensionProperties* out);

}  // namespace remoting
