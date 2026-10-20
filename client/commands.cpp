// Client-side command buffers and recording.
//
// Allocation/free/begin/end/reset and every vkCmd* here are sent with
// send_oneway: nobody waits on them, so a command buffer of many calls costs
// one round trip, not one per call. A failure inside one is not seen until
// the next call that does wait (vkQueueSubmit, vkDeviceWaitIdle); see
// device.cpp's report_oneway_errors.

#include <string.h>

#include <algorithm>
#include <vector>

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "entry_table.hpp"
#include "marshal.hpp"
#include "remote_objects.hpp"
#include "wire.hpp"

namespace remoting {
namespace {

RemoteDevice* to_device(VkDevice handle) { return reinterpret_cast<RemoteDevice*>(handle); }
RemoteCommandBuffer* to_cmd(VkCommandBuffer handle) {
    return reinterpret_cast<RemoteCommandBuffer*>(handle);
}

Reader payload_reader(const std::vector<char>& reply) {
    Reader r(reply.data(), reply.size());
    r.u32();
    return r;
}

void mark_framebuffer_writes(RemoteCommandBuffer* cb,
                             const VkRenderPassBeginInfo* info) {
    std::vector<uint64_t> views;
    {
        std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
        const auto framebuffer = cb->device->framebuffers.find(id_from_handle(info->framebuffer));
        if (framebuffer == cb->device->framebuffers.end()) return;
        views = framebuffer->second.image_views;
        if (framebuffer->second.imageless) {
            for (const auto* node = static_cast<const VkBaseInStructure*>(info->pNext);
                 node; node = node->pNext) {
                if (node->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) continue;
                const auto* attachments =
                    reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(node);
                views.clear();
                for (uint32_t i = 0; i < attachments->attachmentCount; ++i)
                    views.push_back(id_from_handle(attachments->pAttachments[i]));
                break;
            }
        }
        for (uint64_t view_id : views) {
            const auto view = cb->device->image_view_images.find(view_id);
            if (view == cb->device->image_view_images.end()) continue;
            const auto binding = cb->device->image_bindings.find(view->second);
            if (binding != cb->device->image_bindings.end())
                cb->pending_writes.insert(binding->second.memory_id);
        }
    }
}

void mark_descriptor_writes(RemoteCommandBuffer* cb,
                            const std::unordered_map<uint32_t, uint64_t>& sets) {
    for (const auto& bound : sets) cb->writable_descriptor_sets.insert(bound.second);
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(
    VkDevice handle, const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    write_CommandBufferAllocateInfo(request, *pAllocateInfo);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkAllocateCommandBuffers, request,
                                                 &reply)) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && i < pAllocateInfo->commandBufferCount; ++i) {
        auto* cb = new RemoteCommandBuffer();
        set_loader_magic_value(cb);
        cb->device = device;
        cb->remote_id = r.handle();
        cb->command_pool_id = id_from_handle(pAllocateInfo->commandPool);
        {
            std::lock_guard<std::mutex> lock(device->metadata_mutex);
            device->command_buffers.push_back(cb);
        }
        pCommandBuffers[i] = reinterpret_cast<VkCommandBuffer>(cb);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice handle, VkCommandPool pool, uint32_t count,
                                              const VkCommandBuffer* pCommandBuffers) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(pool));
    request.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        request.handle(to_cmd(pCommandBuffers[i])->remote_id);
    }
    device->instance->connection.send_oneway(Opcode::vkFreeCommandBuffers, request);

    std::vector<RemoteCommandBuffer*> retired;
    {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        for (uint32_t i = 0; i < count; ++i) {
            RemoteCommandBuffer* cb = to_cmd(pCommandBuffers[i]);
            auto& buffers = device->command_buffers;
            buffers.erase(std::remove(buffers.begin(), buffers.end(), cb), buffers.end());
            retired.push_back(cb);
        }
    }
    for (RemoteCommandBuffer* cb : retired) delete cb;
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer handle,
                                                  const VkCommandBufferBeginInfo* pBeginInfo) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    write_CommandBufferBeginInfo(request, *pBeginInfo);
    cb->pending_writes.clear();
    cb->host_visible_writes.clear();
    cb->bound_graphics_sets.clear();
    cb->bound_compute_sets.clear();
    cb->writable_descriptor_sets.clear();
    cb->descriptor_writes_host_visible = false;
    cb->device->instance->connection.send_oneway(Opcode::vkBeginCommandBuffer, request);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer handle) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    cb->device->instance->connection.send_oneway(Opcode::vkEndCommandBuffer, request);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer handle,
                                                  VkCommandBufferResetFlags flags) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.u32(flags);
    cb->pending_writes.clear();
    cb->host_visible_writes.clear();
    cb->bound_graphics_sets.clear();
    cb->bound_compute_sets.clear();
    cb->writable_descriptor_sets.clear();
    cb->descriptor_writes_host_visible = false;
    cb->device->instance->connection.send_oneway(Opcode::vkResetCommandBuffer, request);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL TrimCommandPool(VkDevice handle,VkCommandPool pool,VkCommandPoolTrimFlags flags){RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);w.handle(id_from_handle(pool));w.u32(flags);d->instance->connection.send_oneway(Opcode::vkTrimCommandPool,w);}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice handle, VkCommandPool pool,
                                                VkCommandPoolResetFlags flags) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(pool));
    request.u32(flags);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkResetCommandPool, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    if (r.ok() && result == VK_SUCCESS) {
        const uint64_t pool_id = id_from_handle(pool);
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        for (RemoteCommandBuffer* cb : device->command_buffers) {
            if (cb->command_pool_id == pool_id) {
                cb->pending_writes.clear();
                cb->host_visible_writes.clear();
                cb->writable_descriptor_sets.clear();
                cb->descriptor_writes_host_visible = false;
                cb->bound_graphics_sets.clear();
                cb->bound_compute_sets.clear();
            }
        }
    }
    return r.ok() ? result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer handle,
                                              const VkRenderPassBeginInfo* pRenderPassBegin,
                                              VkSubpassContents contents) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    if (!write_RenderPassBeginInfo(request, *pRenderPassBegin)) return;
    mark_framebuffer_writes(cb, pRenderPassBegin);
    request.i32(static_cast<int32_t>(contents));
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBeginRenderPass, request);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(VkCommandBuffer handle, VkSubpassContents contents) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.i32(static_cast<int32_t>(contents));
    cb->device->instance->connection.send_oneway(Opcode::vkCmdNextSubpass, request);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer handle) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdEndRenderPass, request);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(VkCommandBuffer handle,
                                               const VkRenderPassBeginInfo* pRenderPassBegin,
                                               const VkSubpassBeginInfo* pSubpassBeginInfo) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    if (!write_RenderPassBeginInfo(request, *pRenderPassBegin) ||
        !write_SubpassBeginInfo(request, *pSubpassBeginInfo)) return;
    mark_framebuffer_writes(cb, pRenderPassBegin);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBeginRenderPass2, request);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass2(VkCommandBuffer handle,
                                           const VkSubpassBeginInfo* pSubpassBeginInfo,
                                           const VkSubpassEndInfo* pSubpassEndInfo) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    if (!write_SubpassBeginInfo(request, *pSubpassBeginInfo) ||
        !write_SubpassEndInfo(request, *pSubpassEndInfo))
        return;
    cb->device->instance->connection.send_oneway(Opcode::vkCmdNextSubpass2, request);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2(VkCommandBuffer handle,
                                             const VkSubpassEndInfo* pSubpassEndInfo) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    if (!write_SubpassEndInfo(request, *pSubpassEndInfo)) return;
    cb->device->instance->connection.send_oneway(Opcode::vkCmdEndRenderPass2, request);
}

VKAPI_ATTR void VKAPI_CALL CmdSetFragmentShadingRateKHR(
    VkCommandBuffer handle, const VkExtent2D* pFragmentSize,
    const VkFragmentShadingRateCombinerOpKHR combinerOps[2]) {
    if (!pFragmentSize || !combinerOps) return;
    RemoteCommandBuffer* cb = to_cmd(handle); Writer request;
    request.handle(cb->remote_id); request.u32(pFragmentSize->width); request.u32(pFragmentSize->height);
    request.i32(static_cast<int32_t>(combinerOps[0])); request.i32(static_cast<int32_t>(combinerOps[1]));
    cb->device->instance->connection.send_oneway(Opcode::vkCmdSetFragmentShadingRateKHR, request);
}

VKAPI_ATTR void VKAPI_CALL CmdSetDeviceMask(VkCommandBuffer handle,uint32_t mask){RemoteCommandBuffer*cb=to_cmd(handle);Writer w;w.handle(cb->remote_id);w.u32(mask);cb->device->instance->connection.send_oneway(Opcode::vkCmdSetDeviceMask,w);}
VKAPI_ATTR void VKAPI_CALL CmdDispatchBase(VkCommandBuffer handle,uint32_t bx,uint32_t by,uint32_t bz,uint32_t x,uint32_t y,uint32_t z){RemoteCommandBuffer*cb=to_cmd(handle);Writer w;w.handle(cb->remote_id);w.u32(bx);w.u32(by);w.u32(bz);w.u32(x);w.u32(y);w.u32(z);mark_descriptor_writes(cb,cb->bound_compute_sets);cb->device->instance->connection.send_oneway(Opcode::vkCmdDispatchBase,w);}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer handle,
                                           VkPipelineBindPoint pipelineBindPoint,
                                           VkPipeline pipeline) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.i32(static_cast<int32_t>(pipelineBindPoint));
    request.handle(id_from_handle(pipeline));
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBindPipeline, request);
}

VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(
    VkCommandBuffer handle, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
    uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
    if (layout == VK_NULL_HANDLE || descriptorSetCount > kMaxArrayElements ||
        dynamicOffsetCount > kMaxArrayElements ||
        (descriptorSetCount != 0 && pDescriptorSets == nullptr) ||
        (dynamicOffsetCount != 0 && pDynamicOffsets == nullptr)) {
        return;
    }
    for (uint32_t i = 0; i < descriptorSetCount; ++i) {
        if (pDescriptorSets[i] == VK_NULL_HANDLE) return;
    }
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.i32(static_cast<int32_t>(pipelineBindPoint));
    request.handle(id_from_handle(layout));
    request.u32(firstSet);
    request.u32(descriptorSetCount);
    for (uint32_t i = 0; i < descriptorSetCount; ++i) {
        request.handle(id_from_handle(pDescriptorSets[i]));
    }
    request.u32(dynamicOffsetCount);
    for (uint32_t i = 0; i < dynamicOffsetCount; ++i) request.u32(pDynamicOffsets[i]);
    auto& bound = pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE
        ? cb->bound_compute_sets : cb->bound_graphics_sets;
    for (uint32_t i = 0; i < descriptorSetCount; ++i)
        bound[firstSet + i] = id_from_handle(pDescriptorSets[i]);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBindDescriptorSets, request);
}

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer handle, uint32_t firstBinding,
                                                uint32_t bindingCount, const VkBuffer* pBuffers,
                                                const VkDeviceSize* pOffsets) {
    if (bindingCount > kMaxArrayElements ||
        (bindingCount != 0 && (pBuffers == nullptr || pOffsets == nullptr))) {
        return;
    }
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.u32(firstBinding);
    request.u32(bindingCount);
    for (uint32_t i = 0; i < bindingCount; ++i) {
        request.handle(id_from_handle(pBuffers[i]));
        request.u64(pOffsets[i]);
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBindVertexBuffers, request);
}

VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer handle, VkBuffer buffer,
                                              VkDeviceSize offset, VkIndexType indexType) {
    if (buffer == VK_NULL_HANDLE) return;
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(buffer));
    request.u64(offset);
    request.i32(static_cast<int32_t>(indexType));
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBindIndexBuffer, request);
}

VKAPI_ATTR void VKAPI_CALL CmdSetViewport(VkCommandBuffer handle, uint32_t first, uint32_t count,
                                          const VkViewport* pViewports) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.u32(first);
    request.u32(count);
    for (uint32_t i = 0; i < count; ++i) write_Viewport(request, pViewports[i]);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdSetViewport, request);
}

VKAPI_ATTR void VKAPI_CALL CmdSetScissor(VkCommandBuffer handle, uint32_t first, uint32_t count,
                                         const VkRect2D* pScissors) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.u32(first);
    request.u32(count);
    for (uint32_t i = 0; i < count; ++i) write_Rect2D(request, pScissors[i]);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdSetScissor, request);
}

VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer handle, uint32_t vertexCount,
                                   uint32_t instanceCount, uint32_t firstVertex,
                                   uint32_t firstInstance) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.u32(vertexCount);
    request.u32(instanceCount);
    request.u32(firstVertex);
    request.u32(firstInstance);
    mark_descriptor_writes(cb, cb->bound_graphics_sets);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdDraw, request);
}

VKAPI_ATTR void VKAPI_CALL CmdDispatch(VkCommandBuffer handle, uint32_t groupCountX,
                                       uint32_t groupCountY, uint32_t groupCountZ) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.u32(groupCountX);
    request.u32(groupCountY);
    request.u32(groupCountZ);
    mark_descriptor_writes(cb, cb->bound_compute_sets);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdDispatch, request);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer handle, uint32_t indexCount,
                                          uint32_t instanceCount, uint32_t firstIndex,
                                          int32_t vertexOffset, uint32_t firstInstance) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.u32(indexCount);
    request.u32(instanceCount);
    request.u32(firstIndex);
    request.i32(vertexOffset);
    request.u32(firstInstance);
    mark_descriptor_writes(cb, cb->bound_graphics_sets);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdDrawIndexed, request);
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(
    VkCommandBuffer handle, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount,
    const VkMemoryBarrier* pMemoryBarriers, uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier* pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.u32(srcStageMask);
    request.u32(dstStageMask);
    request.u32(dependencyFlags);
    request.u32(memoryBarrierCount);
    for (uint32_t i = 0; i < memoryBarrierCount; ++i) write_MemoryBarrier(request, pMemoryBarriers[i]);
    request.u32(bufferMemoryBarrierCount);
    for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
        write_BufferMemoryBarrier(request, pBufferMemoryBarriers[i]);
    }
    request.u32(imageMemoryBarrierCount);
    for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
        write_ImageMemoryBarrier(request, pImageMemoryBarriers[i]);
    }
    if (dstStageMask & VK_PIPELINE_STAGE_HOST_BIT) {
        const VkAccessFlags host = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
        for (uint32_t i = 0; i < memoryBarrierCount; ++i) {
            if (pMemoryBarriers[i].dstAccessMask & host) {
                cb->host_visible_writes.insert(cb->pending_writes.begin(), cb->pending_writes.end());
                cb->descriptor_writes_host_visible = true;
            }
        }
        for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
            if (!(pBufferMemoryBarriers[i].dstAccessMask & host)) continue;
            std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
            const auto binding = cb->device->buffer_bindings.find(
                id_from_handle(pBufferMemoryBarriers[i].buffer));
            if (binding != cb->device->buffer_bindings.end()) {
                const uint64_t memory_id = binding->second.memory_id;
                if (cb->pending_writes.count(memory_id))
                    cb->host_visible_writes.insert(memory_id);
                for (uint64_t set_id : cb->writable_descriptor_sets) {
                    const auto resources = cb->device->descriptor_resources.find(set_id);
                    if (resources == cb->device->descriptor_resources.end()) continue;
                    for (const auto& item : resources->second) {
                        const DescriptorResource& resource = item.second;
                        bool matches = (resource.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                                        resource.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) &&
                                       resource.resource_id ==
                                           id_from_handle(pBufferMemoryBarriers[i].buffer);
                        if (resource.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                            const auto view = cb->device->buffer_view_buffers.find(resource.resource_id);
                            matches = view != cb->device->buffer_view_buffers.end() &&
                                      view->second == id_from_handle(pBufferMemoryBarriers[i].buffer);
                        }
                        if (matches) cb->host_visible_writes.insert(memory_id);
                    }
                }
            }
        }
        for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
            if (!(pImageMemoryBarriers[i].dstAccessMask & host)) continue;
            std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
            const auto binding = cb->device->image_bindings.find(
                id_from_handle(pImageMemoryBarriers[i].image));
            if (binding != cb->device->image_bindings.end()) {
                const uint64_t memory_id = binding->second.memory_id;
                if (cb->pending_writes.count(memory_id))
                    cb->host_visible_writes.insert(memory_id);
                for (uint64_t set_id : cb->writable_descriptor_sets) {
                    const auto resources = cb->device->descriptor_resources.find(set_id);
                    if (resources == cb->device->descriptor_resources.end()) continue;
                    for (const auto& item : resources->second) {
                        const DescriptorResource& resource = item.second;
                        if (resource.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
                        const auto view = cb->device->image_view_images.find(resource.resource_id);
                        if (view != cb->device->image_view_images.end() &&
                            view->second == id_from_handle(pImageMemoryBarriers[i].image))
                            cb->host_visible_writes.insert(memory_id);
                    }
                }
            }
        }
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdPipelineBarrier, request);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer handle, VkBuffer src, VkBuffer dst,
                                         uint32_t regionCount, const VkBufferCopy* pRegions) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(src));
    request.handle(id_from_handle(dst));
    request.u32(regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) write_BufferCopy(request, pRegions[i]);
    {
        std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
        const auto binding = cb->device->buffer_bindings.find(id_from_handle(dst));
        if (binding != cb->device->buffer_bindings.end())
            cb->pending_writes.insert(binding->second.memory_id);
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdCopyBuffer, request);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer handle, VkBuffer src, VkImage dst,
                                                VkImageLayout dstImageLayout,
                                                uint32_t regionCount,
                                                const VkBufferImageCopy* pRegions) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(src));
    request.handle(id_from_handle(dst));
    request.i32(static_cast<int32_t>(dstImageLayout));
    request.u32(regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) write_BufferImageCopy(request, pRegions[i]);
    {
        std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
        const auto binding = cb->device->image_bindings.find(id_from_handle(dst));
        if (binding != cb->device->image_bindings.end())
            cb->pending_writes.insert(binding->second.memory_id);
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdCopyBufferToImage, request);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer(VkCommandBuffer handle, VkImage src,
                                                VkImageLayout srcImageLayout, VkBuffer dst,
                                                uint32_t regionCount,
                                                const VkBufferImageCopy* pRegions) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(src));
    request.i32(static_cast<int32_t>(srcImageLayout));
    request.handle(id_from_handle(dst));
    request.u32(regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) write_BufferImageCopy(request, pRegions[i]);
    {
        std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
        const auto binding = cb->device->buffer_bindings.find(id_from_handle(dst));
        if (binding != cb->device->buffer_bindings.end())
            cb->pending_writes.insert(binding->second.memory_id);
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdCopyImageToBuffer, request);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImage(VkCommandBuffer handle, VkImage src,
                                        VkImageLayout srcImageLayout, VkImage dst,
                                        VkImageLayout dstImageLayout, uint32_t regionCount,
                                        const VkImageCopy* pRegions) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(src));
    request.i32(static_cast<int32_t>(srcImageLayout));
    request.handle(id_from_handle(dst));
    request.i32(static_cast<int32_t>(dstImageLayout));
    request.u32(regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) write_ImageCopy(request, pRegions[i]);
    {
        std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
        const auto binding = cb->device->image_bindings.find(id_from_handle(dst));
        if (binding != cb->device->image_bindings.end())
            cb->pending_writes.insert(binding->second.memory_id);
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdCopyImage, request);
}

VKAPI_ATTR void VKAPI_CALL CmdBlitImage(VkCommandBuffer handle, VkImage src,
                                        VkImageLayout srcImageLayout, VkImage dst,
                                        VkImageLayout dstImageLayout, uint32_t regionCount,
                                        const VkImageBlit* pRegions, VkFilter filter) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(src));
    request.i32(static_cast<int32_t>(srcImageLayout));
    request.handle(id_from_handle(dst));
    request.i32(static_cast<int32_t>(dstImageLayout));
    request.u32(regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) write_ImageBlit(request, pRegions[i]);
    request.i32(static_cast<int32_t>(filter));
    {
        std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
        const auto binding = cb->device->image_bindings.find(id_from_handle(dst));
        if (binding != cb->device->image_bindings.end())
            cb->pending_writes.insert(binding->second.memory_id);
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBlitImage, request);
}

VKAPI_ATTR void VKAPI_CALL CmdFillBuffer(VkCommandBuffer handle, VkBuffer dst,
                                         VkDeviceSize dstOffset, VkDeviceSize size,
                                         uint32_t data) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(dst));
    request.u64(dstOffset);
    request.u64(size);
    request.u32(data);
    {
        std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
        const auto binding = cb->device->buffer_bindings.find(id_from_handle(dst));
        if (binding != cb->device->buffer_bindings.end())
            cb->pending_writes.insert(binding->second.memory_id);
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdFillBuffer, request);
}

VKAPI_ATTR void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer handle, VkBuffer dst,
                                           VkDeviceSize dstOffset, VkDeviceSize dataSize,
                                           const void* pData) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(dst));
    request.u64(dstOffset);
    request.u64(dataSize);
    if (dataSize != 0 && dataSize <= 65536 && (dataSize & 3) == 0 && pData != nullptr) {
        request.bytes(pData, static_cast<size_t>(dataSize));
    } else {
        request.bytes(nullptr, 0);
    }
    {
        std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
        const auto binding = cb->device->buffer_bindings.find(id_from_handle(dst));
        if (binding != cb->device->buffer_bindings.end())
            cb->pending_writes.insert(binding->second.memory_id);
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdUpdateBuffer, request);
}

VKAPI_ATTR void VKAPI_CALL CmdClearColorImage(VkCommandBuffer handle, VkImage image,
                                              VkImageLayout imageLayout,
                                              const VkClearColorValue* pColor,
                                              uint32_t rangeCount,
                                              const VkImageSubresourceRange* pRanges) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(image));
    request.i32(static_cast<int32_t>(imageLayout));
    request.bytes(pColor, sizeof(*pColor));
    request.u32(rangeCount);
    for (uint32_t i = 0; i < rangeCount; ++i) request.bytes(&pRanges[i], sizeof(pRanges[i]));
    {
        std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
        const auto binding = cb->device->image_bindings.find(id_from_handle(image));
        if (binding != cb->device->image_bindings.end())
            cb->pending_writes.insert(binding->second.memory_id);
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdClearColorImage, request);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer h, VkBuffer b, VkDeviceSize off,
                                            uint32_t count, uint32_t stride) {
    auto* cb=to_cmd(h); Writer w; w.handle(cb->remote_id); w.handle(id_from_handle(b));
    w.u64(off); w.u32(count); w.u32(stride); mark_descriptor_writes(cb,cb->bound_graphics_sets); cb->device->instance->connection.send_oneway(Opcode::vkCmdDrawIndirect,w);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer h, VkBuffer b, VkDeviceSize off,
                                                   uint32_t count, uint32_t stride) {
    auto* cb=to_cmd(h); Writer w; w.handle(cb->remote_id); w.handle(id_from_handle(b));
    w.u64(off); w.u32(count); w.u32(stride); mark_descriptor_writes(cb,cb->bound_graphics_sets); cb->device->instance->connection.send_oneway(Opcode::vkCmdDrawIndexedIndirect,w);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCount(VkCommandBuffer h,VkBuffer b,VkDeviceSize off,VkBuffer countBuffer,VkDeviceSize countOffset,uint32_t maxDrawCount,uint32_t stride){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(b));w.u64(off);w.handle(id_from_handle(countBuffer));w.u64(countOffset);w.u32(maxDrawCount);w.u32(stride);mark_descriptor_writes(cb,cb->bound_graphics_sets);cb->device->instance->connection.send_oneway(Opcode::vkCmdDrawIndirectCount,w);}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount(VkCommandBuffer h,VkBuffer b,VkDeviceSize off,VkBuffer countBuffer,VkDeviceSize countOffset,uint32_t maxDrawCount,uint32_t stride){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(b));w.u64(off);w.handle(id_from_handle(countBuffer));w.u64(countOffset);w.u32(maxDrawCount);w.u32(stride);mark_descriptor_writes(cb,cb->bound_graphics_sets);cb->device->instance->connection.send_oneway(Opcode::vkCmdDrawIndexedIndirectCount,w);}
VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(VkCommandBuffer h, VkBuffer b, VkDeviceSize off) {
    auto* cb=to_cmd(h); Writer w; w.handle(cb->remote_id); w.handle(id_from_handle(b)); w.u64(off);
    mark_descriptor_writes(cb,cb->bound_compute_sets);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdDispatchIndirect,w);
}
VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer h, uint32_t count,
                                               const VkCommandBuffer* commands) {
    auto* cb=to_cmd(h); Writer w; w.handle(cb->remote_id); w.u32(count);
    for(uint32_t i=0;i<count;++i){auto*secondary=to_cmd(commands[i]);w.handle(secondary->remote_id);cb->pending_writes.insert(secondary->pending_writes.begin(),secondary->pending_writes.end());cb->host_visible_writes.insert(secondary->host_visible_writes.begin(),secondary->host_visible_writes.end());cb->writable_descriptor_sets.insert(secondary->writable_descriptor_sets.begin(),secondary->writable_descriptor_sets.end());cb->descriptor_writes_host_visible=cb->descriptor_writes_host_visible||secondary->descriptor_writes_host_visible;}
    cb->device->instance->connection.send_oneway(Opcode::vkCmdExecuteCommands,w);
}
VKAPI_ATTR void VKAPI_CALL CmdBeginQuery(VkCommandBuffer h,VkQueryPool p,uint32_t q,VkQueryControlFlags f){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(p));w.u32(q);w.u32(f);cb->device->instance->connection.send_oneway(Opcode::vkCmdBeginQuery,w);}
VKAPI_ATTR void VKAPI_CALL CmdEndQuery(VkCommandBuffer h,VkQueryPool p,uint32_t q){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(p));w.u32(q);cb->device->instance->connection.send_oneway(Opcode::vkCmdEndQuery,w);}
VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer h,VkQueryPool p,uint32_t first,uint32_t count){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(p));w.u32(first);w.u32(count);cb->device->instance->connection.send_oneway(Opcode::vkCmdResetQueryPool,w);}
VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer h,VkPipelineStageFlagBits stage,VkQueryPool p,uint32_t q){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.u32(stage);w.handle(id_from_handle(p));w.u32(q);cb->device->instance->connection.send_oneway(Opcode::vkCmdWriteTimestamp,w);}
VKAPI_ATTR void VKAPI_CALL CmdCopyQueryPoolResults(VkCommandBuffer h,VkQueryPool p,uint32_t first,uint32_t count,VkBuffer b,VkDeviceSize off,VkDeviceSize stride,VkQueryResultFlags flags){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(p));w.u32(first);w.u32(count);w.handle(id_from_handle(b));w.u64(off);w.u64(stride);w.u32(flags);{std::lock_guard<std::mutex>lock(cb->device->metadata_mutex);auto binding=cb->device->buffer_bindings.find(id_from_handle(b));if(binding!=cb->device->buffer_bindings.end())cb->pending_writes.insert(binding->second.memory_id);}cb->device->instance->connection.send_oneway(Opcode::vkCmdCopyQueryPoolResults,w);}
VKAPI_ATTR void VKAPI_CALL CmdSetEvent(VkCommandBuffer h,VkEvent e,VkPipelineStageFlags stage){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(e));w.u32(stage);cb->device->instance->connection.send_oneway(Opcode::vkCmdSetEvent,w);}
VKAPI_ATTR void VKAPI_CALL CmdResetEvent(VkCommandBuffer h,VkEvent e,VkPipelineStageFlags stage){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(e));w.u32(stage);cb->device->instance->connection.send_oneway(Opcode::vkCmdResetEvent,w);}
VKAPI_ATTR void VKAPI_CALL CmdSetLineWidth(VkCommandBuffer h,float v){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.f32(v);cb->device->instance->connection.send_oneway(Opcode::vkCmdSetLineWidth,w);}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBias(VkCommandBuffer h,float a,float b,float d){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.f32(a);w.f32(b);w.f32(d);cb->device->instance->connection.send_oneway(Opcode::vkCmdSetDepthBias,w);}
VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(VkCommandBuffer h,const float v[4]){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);for(int i=0;i<4;++i)w.f32(v[i]);cb->device->instance->connection.send_oneway(Opcode::vkCmdSetBlendConstants,w);}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBounds(VkCommandBuffer h,float a,float b){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.f32(a);w.f32(b);cb->device->instance->connection.send_oneway(Opcode::vkCmdSetDepthBounds,w);}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilCompareMask(VkCommandBuffer h,VkStencilFaceFlags f,uint32_t m){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.u32(f);w.u32(m);cb->device->instance->connection.send_oneway(Opcode::vkCmdSetStencilCompareMask,w);}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilWriteMask(VkCommandBuffer h,VkStencilFaceFlags f,uint32_t m){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.u32(f);w.u32(m);cb->device->instance->connection.send_oneway(Opcode::vkCmdSetStencilWriteMask,w);}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilReference(VkCommandBuffer h,VkStencilFaceFlags f,uint32_t m){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.u32(f);w.u32(m);cb->device->instance->connection.send_oneway(Opcode::vkCmdSetStencilReference,w);}

VKAPI_ATTR void VKAPI_CALL CmdResolveImage(VkCommandBuffer h,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageResolve* regions){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(src));w.i32(sl);w.handle(id_from_handle(dst));w.i32(dl);w.u32(count);for(uint32_t i=0;i<count;++i){const auto&r=regions[i];w.u32(r.srcSubresource.aspectMask);w.u32(r.srcSubresource.mipLevel);w.u32(r.srcSubresource.baseArrayLayer);w.u32(r.srcSubresource.layerCount);w.i32(r.srcOffset.x);w.i32(r.srcOffset.y);w.i32(r.srcOffset.z);w.u32(r.dstSubresource.aspectMask);w.u32(r.dstSubresource.mipLevel);w.u32(r.dstSubresource.baseArrayLayer);w.u32(r.dstSubresource.layerCount);w.i32(r.dstOffset.x);w.i32(r.dstOffset.y);w.i32(r.dstOffset.z);w.u32(r.extent.width);w.u32(r.extent.height);w.u32(r.extent.depth);}{std::lock_guard<std::mutex>lock(cb->device->metadata_mutex);auto binding=cb->device->image_bindings.find(id_from_handle(dst));if(binding!=cb->device->image_bindings.end())cb->pending_writes.insert(binding->second.memory_id);}cb->device->instance->connection.send_oneway(Opcode::vkCmdResolveImage,w);}
VKAPI_ATTR void VKAPI_CALL CmdClearDepthStencilImage(VkCommandBuffer h,VkImage image,VkImageLayout layout,const VkClearDepthStencilValue* value,uint32_t count,const VkImageSubresourceRange* ranges){if(!value)return;auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(image));w.i32(layout);w.f32(value->depth);w.u32(value->stencil);w.u32(count);for(uint32_t i=0;i<count;++i){w.u32(ranges[i].aspectMask);w.u32(ranges[i].baseMipLevel);w.u32(ranges[i].levelCount);w.u32(ranges[i].baseArrayLayer);w.u32(ranges[i].layerCount);}{std::lock_guard<std::mutex>lock(cb->device->metadata_mutex);auto binding=cb->device->image_bindings.find(id_from_handle(image));if(binding!=cb->device->image_bindings.end())cb->pending_writes.insert(binding->second.memory_id);}cb->device->instance->connection.send_oneway(Opcode::vkCmdClearDepthStencilImage,w);}
VKAPI_ATTR void VKAPI_CALL CmdClearAttachments(VkCommandBuffer h,uint32_t attachmentCount,const VkClearAttachment* attachments,uint32_t rectCount,const VkClearRect* rects){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.u32(attachmentCount);for(uint32_t i=0;i<attachmentCount;++i){w.u32(attachments[i].aspectMask);w.u32(attachments[i].colorAttachment);w.bytes(&attachments[i].clearValue,sizeof(VkClearValue));}w.u32(rectCount);for(uint32_t i=0;i<rectCount;++i){w.i32(rects[i].rect.offset.x);w.i32(rects[i].rect.offset.y);w.u32(rects[i].rect.extent.width);w.u32(rects[i].rect.extent.height);w.u32(rects[i].baseArrayLayer);w.u32(rects[i].layerCount);}cb->device->instance->connection.send_oneway(Opcode::vkCmdClearAttachments,w);}
VKAPI_ATTR void VKAPI_CALL CmdWaitEvents(VkCommandBuffer h,uint32_t eventCount,const VkEvent* events,VkPipelineStageFlags src,VkPipelineStageFlags dst,uint32_t memoryCount,const VkMemoryBarrier* memory,uint32_t bufferCount,const VkBufferMemoryBarrier* buffers,uint32_t imageCount,const VkImageMemoryBarrier* images){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.u32(eventCount);for(uint32_t i=0;i<eventCount;++i)w.handle(id_from_handle(events[i]));w.u32(src);w.u32(dst);w.u32(memoryCount);for(uint32_t i=0;i<memoryCount;++i)write_MemoryBarrier(w,memory[i]);w.u32(bufferCount);for(uint32_t i=0;i<bufferCount;++i)write_BufferMemoryBarrier(w,buffers[i]);w.u32(imageCount);for(uint32_t i=0;i<imageCount;++i)write_ImageMemoryBarrier(w,images[i]);if(dst&VK_PIPELINE_STAGE_HOST_BIT){const VkAccessFlags host=VK_ACCESS_HOST_READ_BIT|VK_ACCESS_HOST_WRITE_BIT;for(uint32_t i=0;i<memoryCount;++i)if(memory[i].dstAccessMask&host){cb->host_visible_writes.insert(cb->pending_writes.begin(),cb->pending_writes.end());cb->descriptor_writes_host_visible=true;}std::lock_guard<std::mutex>lock(cb->device->metadata_mutex);for(uint32_t i=0;i<bufferCount;++i)if(buffers[i].dstAccessMask&host){auto binding=cb->device->buffer_bindings.find(id_from_handle(buffers[i].buffer));if(binding!=cb->device->buffer_bindings.end()){uint64_t memory_id=binding->second.memory_id;if(cb->pending_writes.count(memory_id))cb->host_visible_writes.insert(memory_id);for(uint64_t set_id:cb->writable_descriptor_sets){auto resources=cb->device->descriptor_resources.find(set_id);if(resources==cb->device->descriptor_resources.end())continue;for(const auto&item:resources->second){const auto&resource=item.second;bool matches=(resource.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||resource.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)&&resource.resource_id==id_from_handle(buffers[i].buffer);if(resource.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER){auto view=cb->device->buffer_view_buffers.find(resource.resource_id);matches=view!=cb->device->buffer_view_buffers.end()&&view->second==id_from_handle(buffers[i].buffer);}if(matches)cb->host_visible_writes.insert(memory_id);}}}}for(uint32_t i=0;i<imageCount;++i)if(images[i].dstAccessMask&host){auto binding=cb->device->image_bindings.find(id_from_handle(images[i].image));if(binding!=cb->device->image_bindings.end()){uint64_t memory_id=binding->second.memory_id;if(cb->pending_writes.count(memory_id))cb->host_visible_writes.insert(memory_id);for(uint64_t set_id:cb->writable_descriptor_sets){auto resources=cb->device->descriptor_resources.find(set_id);if(resources==cb->device->descriptor_resources.end())continue;for(const auto&item:resources->second){const auto&resource=item.second;if(resource.type!=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)continue;auto view=cb->device->image_view_images.find(resource.resource_id);if(view!=cb->device->image_view_images.end()&&view->second==id_from_handle(images[i].image))cb->host_visible_writes.insert(memory_id);}}}}}cb->device->instance->connection.send_oneway(Opcode::vkCmdWaitEvents,w);}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer handle,
                                                 const VkDependencyInfo* info) {
    if (!info) return;
    auto* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    if (!write_DependencyInfo(request, *info)) return;
    const VkPipelineStageFlags2 host_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
    const VkAccessFlags2 host_access = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    bool general_host = false;
    for (uint32_t i=0;i<info->memoryBarrierCount;++i)
        general_host |= (info->pMemoryBarriers[i].dstStageMask & host_stage) &&
                        (info->pMemoryBarriers[i].dstAccessMask & host_access);
    if (general_host) {
        cb->host_visible_writes.insert(cb->pending_writes.begin(), cb->pending_writes.end());
        cb->descriptor_writes_host_visible = true;
    }
    {
        std::lock_guard<std::mutex> lock(cb->device->metadata_mutex);
        for(uint32_t i=0;i<info->bufferMemoryBarrierCount;++i){const auto&barrier=info->pBufferMemoryBarriers[i];if(!(barrier.dstStageMask&host_stage)||!(barrier.dstAccessMask&host_access))continue;auto binding=cb->device->buffer_bindings.find(id_from_handle(barrier.buffer));if(binding!=cb->device->buffer_bindings.end())cb->host_visible_writes.insert(binding->second.memory_id);}
        for(uint32_t i=0;i<info->imageMemoryBarrierCount;++i){const auto&barrier=info->pImageMemoryBarriers[i];if(!(barrier.dstStageMask&host_stage)||!(barrier.dstAccessMask&host_access))continue;auto binding=cb->device->image_bindings.find(id_from_handle(barrier.image));if(binding!=cb->device->image_bindings.end())cb->host_visible_writes.insert(binding->second.memory_id);}
    }
    cb->device->instance->connection.send_oneway(Opcode::vkCmdPipelineBarrier2, request);
}

VKAPI_ATTR void VKAPI_CALL CmdSetEvent2(VkCommandBuffer h,VkEvent e,const VkDependencyInfo*info){if(!info)return;auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(e));if(!write_DependencyInfo(w,*info))return;cb->device->instance->connection.send_oneway(Opcode::vkCmdSetEvent2,w);}
VKAPI_ATTR void VKAPI_CALL CmdResetEvent2(VkCommandBuffer h,VkEvent e,VkPipelineStageFlags2 stage){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.handle(id_from_handle(e));w.u64(stage);cb->device->instance->connection.send_oneway(Opcode::vkCmdResetEvent2,w);}
VKAPI_ATTR void VKAPI_CALL CmdWaitEvents2(VkCommandBuffer h,uint32_t count,const VkEvent*events,const VkDependencyInfo*infos){if(count>kMaxArrayElements||(count&&(!events||!infos)))return;auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.u32(count);for(uint32_t i=0;i<count;++i){w.handle(id_from_handle(events[i]));if(!write_DependencyInfo(w,infos[i]))return;const auto&d=infos[i];const VkPipelineStageFlags2 hs=VK_PIPELINE_STAGE_2_HOST_BIT;const VkAccessFlags2 ha=VK_ACCESS_2_HOST_READ_BIT|VK_ACCESS_2_HOST_WRITE_BIT;for(uint32_t j=0;j<d.memoryBarrierCount;++j)if((d.pMemoryBarriers[j].dstStageMask&hs)&&(d.pMemoryBarriers[j].dstAccessMask&ha)){cb->host_visible_writes.insert(cb->pending_writes.begin(),cb->pending_writes.end());cb->descriptor_writes_host_visible=true;}std::lock_guard<std::mutex>lock(cb->device->metadata_mutex);for(uint32_t j=0;j<d.bufferMemoryBarrierCount;++j){const auto&b=d.pBufferMemoryBarriers[j];if(!(b.dstStageMask&hs)||!(b.dstAccessMask&ha))continue;auto binding=cb->device->buffer_bindings.find(id_from_handle(b.buffer));if(binding!=cb->device->buffer_bindings.end())cb->host_visible_writes.insert(binding->second.memory_id);}for(uint32_t j=0;j<d.imageMemoryBarrierCount;++j){const auto&b=d.pImageMemoryBarriers[j];if(!(b.dstStageMask&hs)||!(b.dstAccessMask&ha))continue;auto binding=cb->device->image_bindings.find(id_from_handle(b.image));if(binding!=cb->device->image_bindings.end())cb->host_visible_writes.insert(binding->second.memory_id);}}cb->device->instance->connection.send_oneway(Opcode::vkCmdWaitEvents2,w);}
VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp2(VkCommandBuffer h,VkPipelineStageFlags2 stage,VkQueryPool pool,uint32_t query){auto*cb=to_cmd(h);Writer w;w.handle(cb->remote_id);w.u64(stage);w.handle(id_from_handle(pool));w.u32(query);cb->device->instance->connection.send_oneway(Opcode::vkCmdWriteTimestamp2,w);}

VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer handle, VkPipelineLayout layout,
                                            VkShaderStageFlags stageFlags, uint32_t offset,
                                            uint32_t size, const void* pValues) {
    if (layout == VK_NULL_HANDLE || size == 0 || size > kMaxPushConstantBytes ||
        (offset & 3) != 0 || (size & 3) != 0 || pValues == nullptr) {
        return;
    }
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(layout));
    request.u32(stageFlags);
    request.u32(offset);
    request.bytes(pValues, size);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdPushConstants, request);
}

}  // namespace

const DeviceEntry* get_command_entries(size_t* count) {
    static const DeviceEntry kEntries[] = {
#define D(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name)}
        D(AllocateCommandBuffers),
        D(FreeCommandBuffers),
        D(BeginCommandBuffer),
        D(EndCommandBuffer),
        D(ResetCommandBuffer),
        D(ResetCommandPool),
        D(TrimCommandPool),
        {"vkTrimCommandPoolKHR", reinterpret_cast<PFN_vkVoidFunction>(TrimCommandPool)},
        D(CmdBeginRenderPass),
        D(CmdNextSubpass),
        D(CmdEndRenderPass),
        D(CmdBeginRenderPass2),
        {"vkCmdBeginRenderPass2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdBeginRenderPass2)},
        D(CmdNextSubpass2),
        {"vkCmdNextSubpass2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdNextSubpass2)},
        D(CmdEndRenderPass2),
        {"vkCmdEndRenderPass2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdEndRenderPass2)},
        D(CmdSetFragmentShadingRateKHR),
        D(CmdSetDeviceMask),
        {"vkCmdSetDeviceMaskKHR", reinterpret_cast<PFN_vkVoidFunction>(CmdSetDeviceMask)},
        D(CmdDispatchBase),
        {"vkCmdDispatchBaseKHR", reinterpret_cast<PFN_vkVoidFunction>(CmdDispatchBase)},
        D(CmdBindPipeline),
        D(CmdBindDescriptorSets),
        D(CmdBindVertexBuffers),
        D(CmdBindIndexBuffer),
        D(CmdSetViewport),
        D(CmdSetScissor),
        D(CmdDraw),
        D(CmdDispatch),
        D(CmdDrawIndexed),
        D(CmdDrawIndirect),
        D(CmdDrawIndexedIndirect),
        D(CmdDrawIndirectCount),
        {"vkCmdDrawIndirectCountKHR", reinterpret_cast<PFN_vkVoidFunction>(CmdDrawIndirectCount)},
        D(CmdDrawIndexedIndirectCount),
        {"vkCmdDrawIndexedIndirectCountKHR", reinterpret_cast<PFN_vkVoidFunction>(CmdDrawIndexedIndirectCount)},
        D(CmdDispatchIndirect),
        D(CmdExecuteCommands),
        D(CmdBeginQuery),
        D(CmdEndQuery),
        D(CmdResetQueryPool),
        D(CmdWriteTimestamp),
        D(CmdCopyQueryPoolResults),
        D(CmdSetEvent),
        D(CmdResetEvent),
        D(CmdSetLineWidth),
        D(CmdSetDepthBias),
        D(CmdSetBlendConstants),
        D(CmdSetDepthBounds),
        D(CmdSetStencilCompareMask),
        D(CmdSetStencilWriteMask),
        D(CmdSetStencilReference),
        D(CmdPipelineBarrier),
        D(CmdPipelineBarrier2),
        {"vkCmdPipelineBarrier2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdPipelineBarrier2)},
        D(CmdSetEvent2),
        {"vkCmdSetEvent2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdSetEvent2)},
        D(CmdResetEvent2),
        {"vkCmdResetEvent2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdResetEvent2)},
        D(CmdWaitEvents2),
        {"vkCmdWaitEvents2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdWaitEvents2)},
        D(CmdWriteTimestamp2),
        {"vkCmdWriteTimestamp2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdWriteTimestamp2)},
        D(CmdCopyBuffer),
        D(CmdCopyBufferToImage),
        D(CmdCopyImageToBuffer),
        D(CmdCopyImage),
        D(CmdBlitImage),
        D(CmdFillBuffer),
        D(CmdUpdateBuffer),
        D(CmdClearColorImage),
        D(CmdClearDepthStencilImage),
        D(CmdClearAttachments),
        D(CmdResolveImage),
        D(CmdWaitEvents),
        D(CmdPushConstants),
#undef D
    };
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

}  // namespace remoting
