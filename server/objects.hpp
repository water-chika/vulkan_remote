#pragma once

// Server-side object tables.
//
// A Vulkan handle is a pointer or an opaque 64-bit token that means something
// only inside the process that created it. Sending its bits to another machine
// would be meaningless at best and an address leak at worst, so every object
// the client learns about is named by a 1-based index into one of these
// tables. Index 0 is reserved and always maps to VK_NULL_HANDLE, which lets
// the optional-handle cases (oldSwapchain, basePipelineHandle, a null fence)
// travel without a special encoding.
//
// Slots are never reused. Destroying an object clears its slot but keeps the
// index spent, so a client that uses a handle after destroying it gets a clean
// refusal rather than somebody else's object.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "marshal.hpp"

namespace remoting {

template <typename T>
class Table {
   public:
    uint64_t add(T value) {
        m_slots.push_back(value);
        return static_cast<uint64_t>(m_slots.size());
    }

    T get(uint64_t id) const {
        if (id == 0 || id > m_slots.size()) return VK_NULL_HANDLE;
        return m_slots[id - 1];
    }

    // Returns what was there, so the caller can pass it to the matching
    // vkDestroy* without looking it up twice.
    T take(uint64_t id) {
        if (id == 0 || id > m_slots.size()) return VK_NULL_HANDLE;
        T value = m_slots[id - 1];
        m_slots[id - 1] = VK_NULL_HANDLE;
        return value;
    }

    size_t size() const { return m_slots.size(); }

    // For teardown, which has to walk everything that was ever added. Slots
    // emptied by take() read back as VK_NULL_HANDLE, so a caller must skip
    // those rather than assume every slot is live.
    const std::vector<T>& all() const { return m_slots; }

   private:
    std::vector<T> m_slots;
};

// Everything the server knows about one connected client. A second client
// would get its own, which is why this is not global state.
struct ObjectTables final : public HandleResolver {
    Table<VkDevice> devices;
    Table<VkQueue> queues;
    Table<VkCommandPool> command_pools;
    Table<VkCommandBuffer> command_buffers;
    Table<VkBuffer> buffers;
    Table<VkImage> images;
    Table<VkImageView> image_views;
    Table<VkDeviceMemory> memories;
    Table<VkRenderPass> render_passes;
    Table<VkFramebuffer> framebuffers;
    Table<VkShaderModule> shader_modules;
    Table<VkPipeline> pipelines;
    Table<VkPipelineLayout> pipeline_layouts;
    Table<VkPipelineCache> pipeline_caches;
    Table<VkDescriptorSetLayout> descriptor_set_layouts;
    Table<VkDescriptorPool> descriptor_pools;
    Table<VkDescriptorSet> descriptor_sets;
    Table<VkSampler> samplers;
    Table<VkSemaphore> semaphores;
    Table<VkFence> fences;
    Table<VkSurfaceKHR> surfaces;
    Table<VkSwapchainKHR> swapchains;

    // A swapchain can only be destroyed through the device that created it,
    // and nothing in the handle it is given carries that back, so teardown
    // would otherwise have no way to clean one up.
    std::unordered_map<VkSwapchainKHR, VkDevice> swapchain_devices;

    // GetSwapchainImagesKHR is idempotent on the real driver (same VkImages
    // every call), but Table::add is not - calling it twice for the same
    // swapchain would hand the client two different ids for one underlying
    // image. Cached per swapchain id so a second query reuses the first
    // call's ids instead of growing the table pointlessly.
    std::unordered_map<uint64_t, std::vector<uint64_t>> swapchain_image_ids;

    VkBuffer buffer(uint64_t id) const override { return buffers.get(id); }
    VkImage image(uint64_t id) const override { return images.get(id); }
    VkImageView image_view(uint64_t id) const override { return image_views.get(id); }
    VkDeviceMemory memory(uint64_t id) const override { return memories.get(id); }
    VkRenderPass render_pass(uint64_t id) const override { return render_passes.get(id); }
    VkFramebuffer framebuffer(uint64_t id) const override { return framebuffers.get(id); }
    VkShaderModule shader_module(uint64_t id) const override { return shader_modules.get(id); }
    VkPipeline pipeline(uint64_t id) const override { return pipelines.get(id); }
    VkPipelineLayout pipeline_layout(uint64_t id) const override {
        return pipeline_layouts.get(id);
    }
    VkPipelineCache pipeline_cache(uint64_t id) const override { return pipeline_caches.get(id); }
    VkDescriptorSetLayout descriptor_set_layout(uint64_t id) const override {
        return descriptor_set_layouts.get(id);
    }
    VkDescriptorPool descriptor_pool(uint64_t id) const override { return descriptor_pools.get(id); }
    VkDescriptorSet descriptor_set(uint64_t id) const override { return descriptor_sets.get(id); }
    VkSampler sampler(uint64_t id) const override { return samplers.get(id); }
    VkSemaphore semaphore(uint64_t id) const override { return semaphores.get(id); }
    VkFence fence(uint64_t id) const override { return fences.get(id); }
    VkSurfaceKHR surface(uint64_t id) const override { return surfaces.get(id); }
    VkSwapchainKHR swapchain(uint64_t id) const override { return swapchains.get(id); }
    VkCommandPool command_pool(uint64_t id) const override { return command_pools.get(id); }
    VkCommandBuffer command_buffer(uint64_t id) const override { return command_buffers.get(id); }
};

}  // namespace remoting
