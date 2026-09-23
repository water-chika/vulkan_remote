// Server-side device-level handlers: device/queue lifecycle, fences and
// semaphores, every plain (non-command-buffer) resource creation
// (buffers, images, views, samplers, shader modules, pipeline caches,
// pipeline/descriptor-set layouts, render passes, framebuffers, graphics
// pipelines), and descriptor set allocation/update. Command pools and
// command buffers move with the vkCmd* recording functions instead, into
// handlers_command.cpp, since they are meaningless without each other.

#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

#include "marshal.hpp"
#include "session.hpp"

namespace {

using remoting::Arena;
using remoting::Session;
using remoting::Status;
using remoting::mark_oneway_error;

void handle_CreateDevice(Session& c) {
    const uint64_t physdev_id = c.reader.handle();
    VkPhysicalDevice physdev = c.server.physical_device_from_id(physdev_id);
    Arena arena;
    VkDeviceCreateInfo info{};
    if (!c.reader.ok() || physdev == VK_NULL_HANDLE ||
        !remoting::read_DeviceCreateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    VkDevice device = VK_NULL_HANDLE;
    const VkResult result = vkCreateDevice(physdev, &info, nullptr, &device);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.devices.add(device) : 0);
    c.reply();
}

void handle_DestroyDevice(Session& c) {
    const uint64_t id = c.reader.handle();
    VkDevice device = c.tables.devices.take(id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    bool waited = false;
    for (size_t i = 0; i < c.tables.swapchains.all().size(); ++i) {
        const VkSwapchainKHR swapchain = c.tables.swapchains.all()[i];
        const auto owner = c.tables.swapchain_devices.find(swapchain);
        if (swapchain == VK_NULL_HANDLE || owner == c.tables.swapchain_devices.end() ||
            owner->second != device) {
            continue;
        }
        if (!waited) {
            vkDeviceWaitIdle(device);
            waited = true;
        }
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        c.tables.swapchains.take(i + 1);
        c.tables.swapchain_devices.erase(owner);
        c.tables.swapchain_surfaces.erase(swapchain);
        c.tables.swapchain_image_ids.erase(i + 1);
    }
    vkDestroyDevice(device, nullptr);
}

void handle_GetDeviceQueue(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t family = c.reader.u32();
    const uint32_t index = c.reader.u32();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, index, &queue);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.handle(c.tables.queues.add(queue));
    c.reply();
}

void handle_QueueSubmit(Session& c) {
    const uint64_t queue_id = c.reader.handle();
    VkQueue queue = c.tables.queues.get(queue_id);
    const uint64_t fence_id = c.reader.handle();
    VkFence fence = c.tables.fence(fence_id);
    const uint32_t submit_count = c.reader.u32();
    // Each submit reads at least wait_count+cb_count+signal_count = 12 bytes.
    if (!count_fits(c.reader, submit_count, 12)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    std::vector<VkSubmitInfo> submits(submit_count);
    // Kept alive until vkQueueSubmit returns; one entry per submit so each
    // submit's arrays do not alias another's.
    std::vector<std::vector<VkSemaphore>> wait_semaphores(submit_count);
    std::vector<std::vector<VkPipelineStageFlags>> wait_stages(submit_count);
    std::vector<std::vector<VkCommandBuffer>> command_buffers(submit_count);
    std::vector<std::vector<VkSemaphore>> signal_semaphores(submit_count);

    bool ok = c.reader.ok() && queue != VK_NULL_HANDLE;
    for (uint32_t i = 0; ok && i < submit_count; ++i) {
        VkSubmitInfo& s = submits[i];
        s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        const uint32_t wait_count = c.reader.u32();
        if (ok && !count_fits(c.reader, wait_count, 12)) ok = false;
        if (ok) {
            wait_semaphores[i].resize(wait_count);
            wait_stages[i].resize(wait_count);
        }
        for (uint32_t j = 0; j < wait_count && ok; ++j) {
            wait_semaphores[i][j] = c.tables.semaphore(c.reader.handle());
            wait_stages[i][j] = c.reader.u32();
        }
        s.waitSemaphoreCount = wait_count;
        s.pWaitSemaphores = wait_count ? wait_semaphores[i].data() : nullptr;
        s.pWaitDstStageMask = wait_count ? wait_stages[i].data() : nullptr;

        const uint32_t cb_count = c.reader.u32();
        if (ok && !count_fits(c.reader, cb_count, 8)) ok = false;
        if (ok) command_buffers[i].resize(cb_count);
        for (uint32_t j = 0; j < cb_count && ok; ++j) {
            command_buffers[i][j] = c.tables.command_buffer(c.reader.handle());
        }
        s.commandBufferCount = cb_count;
        s.pCommandBuffers = cb_count ? command_buffers[i].data() : nullptr;

        const uint32_t signal_count = c.reader.u32();
        if (ok && !count_fits(c.reader, signal_count, 8)) ok = false;
        if (ok) signal_semaphores[i].resize(signal_count);
        for (uint32_t j = 0; j < signal_count && ok; ++j) {
            signal_semaphores[i][j] = c.tables.semaphore(c.reader.handle());
        }
        s.signalSemaphoreCount = signal_count;
        s.pSignalSemaphores = signal_count ? signal_semaphores[i].data() : nullptr;

        ok = ok && c.reader.ok();
    }

    if (!ok) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    const VkResult result = vkQueueSubmit(queue, submit_count, submits.data(), fence);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(c.oneway_errors);
    c.oneway_errors = 0;
    c.reply();
}

void handle_QueueWaitIdle(Session& c) {
    const uint64_t queue_id = c.reader.handle();
    VkQueue queue = c.tables.queues.get(queue_id);
    if (!c.reader.ok() || queue == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkQueueWaitIdle(queue);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.reply();
}

void handle_DeviceWaitIdle(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkDeviceWaitIdle(device);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(c.oneway_errors);
    c.oneway_errors = 0;
    c.reply();
}

void handle_CreateFence(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkFenceCreateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_FenceCreateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkFence fence = VK_NULL_HANDLE;
    const VkResult result = vkCreateFence(device, &info, nullptr, &fence);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.fences.add(fence) : 0);
    c.reply();
}

void handle_DestroyFence(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t fence_id = c.reader.handle();
    VkFence fence = c.tables.fences.take(fence_id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkDestroyFence(device, fence, nullptr);
}

void handle_ResetFences(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t count = c.reader.u32();
    if (!count_fits(c.reader, count, 8)) {
        mark_oneway_error(c);
        return;
    }
    std::vector<VkFence> fences(count);
    for (uint32_t i = 0; i < count; ++i) fences[i] = c.tables.fence(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    if (vkResetFences(device, count, fences.data()) != VK_SUCCESS) mark_oneway_error(c);
}

void handle_GetFenceStatus(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t fence_id = c.reader.handle();
    VkFence fence = c.tables.fence(fence_id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkGetFenceStatus(device, fence);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.reply();
}

void handle_WaitForFences(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const bool wait_all = c.reader.u32() != 0;
    const uint32_t count = c.reader.u32();
    if (!count_fits(c.reader, count, 8)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    std::vector<VkFence> fences(count);
    for (uint32_t i = 0; i < count; ++i) fences[i] = c.tables.fence(c.reader.handle());
    const uint64_t timeout = c.reader.u64();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkWaitForFences(device, count, fences.data(), wait_all, timeout);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.reply();
}

void handle_CreateSemaphore(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkSemaphoreCreateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_SemaphoreCreateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult result = vkCreateSemaphore(device, &info, nullptr, &semaphore);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.semaphores.add(semaphore) : 0);
    c.reply();
}

void handle_DestroySemaphore(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t id = c.reader.handle();
    VkSemaphore semaphore = c.tables.semaphores.take(id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkDestroySemaphore(device, semaphore, nullptr);
}

// ---------------------------------------------------------------------------

#define SERVER_CREATE(HandleName, VkType, InfoType, ReadFn, VkCreateFn, table)              \
    void handle_Create##HandleName(Session& c) {                                                \
        const uint64_t device_id = c.reader.handle();                                       \
        VkDevice device = c.tables.devices.get(device_id);                                  \
        Arena arena;                                                                        \
        InfoType info{};                                                                    \
        if (!c.reader.ok() || device == VK_NULL_HANDLE ||                                   \
            !ReadFn(c.reader, arena, c.tables, &info)) {                                    \
            c.writer.u32(static_cast<uint32_t>(Status::DecodeError));                       \
            return;                                                                         \
        }                                                                                   \
        VkType obj = VK_NULL_HANDLE;                                                        \
        const VkResult result = VkCreateFn(device, &info, nullptr, &obj);                   \
        c.writer.u32(static_cast<uint32_t>(Status::Ok));                                    \
        c.writer.i32(result);                                                               \
        c.writer.handle(result == VK_SUCCESS ? c.tables.table.add(obj) : 0);                \
        c.reply();                                                                          \
    }

#define SERVER_DESTROY(HandleName, VkType, VkDestroyFn, table)               \
    void handle_Destroy##HandleName(Session& c) {                                \
        const uint64_t device_id = c.reader.handle();                       \
        VkDevice device = c.tables.devices.get(device_id);                  \
        const uint64_t id = c.reader.handle();                              \
        VkType obj = c.tables.table.take(id);                               \
        if (!c.reader.ok() || device == VK_NULL_HANDLE) {                   \
            mark_oneway_error(c);                                           \
            return;                                                         \
        }                                                                   \
        VkDestroyFn(device, obj, nullptr);                                  \
    }

SERVER_CREATE(Buffer, VkBuffer, VkBufferCreateInfo, remoting::read_BufferCreateInfo,
              vkCreateBuffer, buffers)
SERVER_DESTROY(Buffer, VkBuffer, vkDestroyBuffer, buffers)

SERVER_CREATE(Image, VkImage, VkImageCreateInfo, remoting::read_ImageCreateInfo, vkCreateImage,
              images)
SERVER_DESTROY(Image, VkImage, vkDestroyImage, images)

SERVER_CREATE(ImageView, VkImageView, VkImageViewCreateInfo, remoting::read_ImageViewCreateInfo,
              vkCreateImageView, image_views)
SERVER_DESTROY(ImageView, VkImageView, vkDestroyImageView, image_views)

SERVER_CREATE(Sampler, VkSampler, VkSamplerCreateInfo, remoting::read_SamplerCreateInfo,
              vkCreateSampler, samplers)
SERVER_DESTROY(Sampler, VkSampler, vkDestroySampler, samplers)

SERVER_CREATE(ShaderModule, VkShaderModule, VkShaderModuleCreateInfo,
              remoting::read_ShaderModuleCreateInfo, vkCreateShaderModule, shader_modules)
SERVER_DESTROY(ShaderModule, VkShaderModule, vkDestroyShaderModule, shader_modules)

SERVER_CREATE(PipelineCache, VkPipelineCache, VkPipelineCacheCreateInfo,
              remoting::read_PipelineCacheCreateInfo, vkCreatePipelineCache, pipeline_caches)
SERVER_DESTROY(PipelineCache, VkPipelineCache, vkDestroyPipelineCache, pipeline_caches)

SERVER_CREATE(PipelineLayout, VkPipelineLayout, VkPipelineLayoutCreateInfo,
              remoting::read_PipelineLayoutCreateInfo, vkCreatePipelineLayout, pipeline_layouts)
SERVER_DESTROY(PipelineLayout, VkPipelineLayout, vkDestroyPipelineLayout, pipeline_layouts)

SERVER_DESTROY(Pipeline, VkPipeline, vkDestroyPipeline, pipelines)

SERVER_CREATE(RenderPass, VkRenderPass, VkRenderPassCreateInfo, remoting::read_RenderPassCreateInfo,
              vkCreateRenderPass, render_passes)
SERVER_DESTROY(RenderPass, VkRenderPass, vkDestroyRenderPass, render_passes)

SERVER_CREATE(Framebuffer, VkFramebuffer, VkFramebufferCreateInfo,
              remoting::read_FramebufferCreateInfo, vkCreateFramebuffer, framebuffers)
SERVER_DESTROY(Framebuffer, VkFramebuffer, vkDestroyFramebuffer, framebuffers)

SERVER_DESTROY(DescriptorSetLayout, VkDescriptorSetLayout, vkDestroyDescriptorSetLayout,
               descriptor_set_layouts)

SERVER_CREATE(DescriptorPool, VkDescriptorPool, VkDescriptorPoolCreateInfo,
              remoting::read_DescriptorPoolCreateInfo, vkCreateDescriptorPool, descriptor_pools)
SERVER_DESTROY(DescriptorPool, VkDescriptorPool, vkDestroyDescriptorPool, descriptor_pools)

#undef SERVER_CREATE
#undef SERVER_DESTROY


void handle_CreateDescriptorSetLayout(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkDescriptorSetLayoutCreateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_DescriptorSetLayoutCreateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    const VkResult result = vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.descriptor_set_layouts.add(layout) : 0);
    c.reply();
}

void handle_GetImageSubresourceLayout(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkImage image = c.tables.image(c.reader.handle());
    VkImageSubresource sub{};
    sub.aspectMask = c.reader.u32();
    sub.mipLevel = c.reader.u32();
    sub.arrayLayer = c.reader.u32();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(device, image, &sub, &layout);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.u64(layout.offset);
    c.writer.u64(layout.size);
    c.writer.u64(layout.rowPitch);
    c.writer.u64(layout.arrayPitch);
    c.writer.u64(layout.depthPitch);
    c.reply();
}

void handle_CreateGraphicsPipelines(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkPipelineCache cache = c.tables.pipeline_cache(c.reader.handle());
    const uint32_t count = c.reader.u32();
    // Each pipeline's read_GraphicsPipelineCreateInfo starts with a
    // read_raw() call, whose bytes() consumes at least a 4-byte length
    // prefix even on immediate failure.
    if (!count_fits(c.reader, count, 4)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    Arena arena;
    std::vector<VkGraphicsPipelineCreateInfo> infos(count);
    bool ok = c.reader.ok() && device != VK_NULL_HANDLE;
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_GraphicsPipelineCreateInfo(c.reader, arena, c.tables, &infos[i]);
    }
    if (!ok) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    std::vector<VkPipeline> pipelines(count, VK_NULL_HANDLE);
    const VkResult result =
        count ? vkCreateGraphicsPipelines(device, cache, count, infos.data(), nullptr,
                                           pipelines.data())
              : VK_SUCCESS;

    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        c.writer.handle(pipelines[i] != VK_NULL_HANDLE ? c.tables.pipelines.add(pipelines[i]) : 0);
    }
    c.reply();
}

void handle_CreateComputePipelines(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t cache_id = c.reader.handle();
    VkPipelineCache cache = c.tables.pipeline_cache(cache_id);
    const uint32_t count = c.reader.u32();
    // Each create info begins with a length-prefixed raw struct. Bound both
    // the bytes required on the wire and the number of vector elements.
    if (count > remoting::kMaxArrayElements || !count_fits(c.reader, count, 4)) {
        c.reply_status(Status::DecodeError);
        return;
    }

    Arena arena;
    std::vector<VkComputePipelineCreateInfo> infos(count);
    bool ok = c.reader.ok() && device != VK_NULL_HANDLE &&
              (!cache_id || cache != VK_NULL_HANDLE);
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_ComputePipelineCreateInfo(c.reader, arena, c.tables, &infos[i]);
    }
    if (!ok) {
        c.reply_status(Status::DecodeError);
        return;
    }

    std::vector<VkPipeline> pipelines(count, VK_NULL_HANDLE);
    const VkResult result =
        count ? vkCreateComputePipelines(device, cache, count, infos.data(), nullptr,
                                          pipelines.data())
              : VK_SUCCESS;
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        c.writer.handle(pipelines[i] != VK_NULL_HANDLE ? c.tables.pipelines.add(pipelines[i]) : 0);
    }
    c.reply();
}

void handle_AllocateDescriptorSets(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkDescriptorSetAllocateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_DescriptorSetAllocateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    std::vector<VkDescriptorSet> sets(info.descriptorSetCount);
    const VkResult result = vkAllocateDescriptorSets(device, &info, sets.data());
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(info.descriptorSetCount);
    for (uint32_t i = 0; i < info.descriptorSetCount; ++i) {
        c.writer.handle(result == VK_SUCCESS ? c.tables.descriptor_sets.add(sets[i]) : 0);
    }
    c.reply();
}

void handle_UpdateDescriptorSets(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t write_count = c.reader.u32();
    // read_WriteDescriptorSet starts with read_raw(), whose bytes() call
    // consumes at least a 4-byte length prefix.
    if (!count_fits(c.reader, write_count, 4)) {
        mark_oneway_error(c);
        return;
    }
    Arena arena;
    std::vector<VkWriteDescriptorSet> writes(write_count);
    bool ok = c.reader.ok() && device != VK_NULL_HANDLE;
    for (uint32_t i = 0; ok && i < write_count; ++i) {
        ok = remoting::read_WriteDescriptorSet(c.reader, arena, c.tables, &writes[i]);
    }
    const uint32_t copy_count = ok ? c.reader.u32() : 0;
    if (ok && !count_fits(c.reader, copy_count, 4)) ok = false;
    std::vector<VkCopyDescriptorSet> copies(ok ? copy_count : 0);
    for (uint32_t i = 0; ok && i < copy_count; ++i) {
        ok = remoting::read_CopyDescriptorSet(c.reader, arena, c.tables, &copies[i]);
    }
    if (!ok || !c.reader.ok()) {
        mark_oneway_error(c);
        return;
    }
    vkUpdateDescriptorSets(device, write_count, writes.data(), copy_count, copies.data());
}


}  // namespace

REGISTER_HANDLER(vkCreateDevice, handle_CreateDevice);
REGISTER_HANDLER(vkDestroyDevice, handle_DestroyDevice);
REGISTER_HANDLER(vkGetDeviceQueue, handle_GetDeviceQueue);
REGISTER_HANDLER(vkQueueSubmit, handle_QueueSubmit);
REGISTER_HANDLER(vkQueueWaitIdle, handle_QueueWaitIdle);
REGISTER_HANDLER(vkDeviceWaitIdle, handle_DeviceWaitIdle);
REGISTER_HANDLER(vkCreateFence, handle_CreateFence);
REGISTER_HANDLER(vkDestroyFence, handle_DestroyFence);
REGISTER_HANDLER(vkResetFences, handle_ResetFences);
REGISTER_HANDLER(vkGetFenceStatus, handle_GetFenceStatus);
REGISTER_HANDLER(vkWaitForFences, handle_WaitForFences);
REGISTER_HANDLER(vkCreateSemaphore, handle_CreateSemaphore);
REGISTER_HANDLER(vkDestroySemaphore, handle_DestroySemaphore);
REGISTER_HANDLER(vkCreateBuffer, handle_CreateBuffer);
REGISTER_HANDLER(vkDestroyBuffer, handle_DestroyBuffer);
REGISTER_HANDLER(vkCreateImage, handle_CreateImage);
REGISTER_HANDLER(vkDestroyImage, handle_DestroyImage);
REGISTER_HANDLER(vkCreateImageView, handle_CreateImageView);
REGISTER_HANDLER(vkDestroyImageView, handle_DestroyImageView);
REGISTER_HANDLER(vkCreateSampler, handle_CreateSampler);
REGISTER_HANDLER(vkDestroySampler, handle_DestroySampler);
REGISTER_HANDLER(vkCreateShaderModule, handle_CreateShaderModule);
REGISTER_HANDLER(vkDestroyShaderModule, handle_DestroyShaderModule);
REGISTER_HANDLER(vkCreatePipelineCache, handle_CreatePipelineCache);
REGISTER_HANDLER(vkDestroyPipelineCache, handle_DestroyPipelineCache);
REGISTER_HANDLER(vkCreatePipelineLayout, handle_CreatePipelineLayout);
REGISTER_HANDLER(vkDestroyPipelineLayout, handle_DestroyPipelineLayout);
REGISTER_HANDLER(vkDestroyPipeline, handle_DestroyPipeline);
REGISTER_HANDLER(vkCreateRenderPass, handle_CreateRenderPass);
REGISTER_HANDLER(vkDestroyRenderPass, handle_DestroyRenderPass);
REGISTER_HANDLER(vkCreateFramebuffer, handle_CreateFramebuffer);
REGISTER_HANDLER(vkDestroyFramebuffer, handle_DestroyFramebuffer);
REGISTER_HANDLER(vkCreateDescriptorSetLayout, handle_CreateDescriptorSetLayout);
REGISTER_HANDLER(vkDestroyDescriptorSetLayout, handle_DestroyDescriptorSetLayout);
REGISTER_HANDLER(vkCreateDescriptorPool, handle_CreateDescriptorPool);
REGISTER_HANDLER(vkDestroyDescriptorPool, handle_DestroyDescriptorPool);
REGISTER_HANDLER(vkGetImageSubresourceLayout, handle_GetImageSubresourceLayout);
REGISTER_HANDLER(vkCreateGraphicsPipelines, handle_CreateGraphicsPipelines);
REGISTER_HANDLER(vkCreateComputePipelines, handle_CreateComputePipelines);
REGISTER_HANDLER(vkAllocateDescriptorSets, handle_AllocateDescriptorSets);
REGISTER_HANDLER(vkUpdateDescriptorSets, handle_UpdateDescriptorSets);
