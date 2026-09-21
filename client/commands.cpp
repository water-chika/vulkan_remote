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
        device->command_buffers.push_back(cb);
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

    for (uint32_t i = 0; i < count; ++i) {
        RemoteCommandBuffer* cb = to_cmd(pCommandBuffers[i]);
        auto& v = device->command_buffers;
        v.erase(std::remove(v.begin(), v.end(), cb), v.end());
        delete cb;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer handle,
                                                  const VkCommandBufferBeginInfo* pBeginInfo) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    write_CommandBufferBeginInfo(request, *pBeginInfo);
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
    cb->device->instance->connection.send_oneway(Opcode::vkResetCommandBuffer, request);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer handle,
                                              const VkRenderPassBeginInfo* pRenderPassBegin,
                                              VkSubpassContents contents) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    write_RenderPassBeginInfo(request, *pRenderPassBegin);
    request.i32(static_cast<int32_t>(contents));
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBeginRenderPass, request);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer handle) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdEndRenderPass, request);
}

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
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBindDescriptorSets, request);
}

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer handle, uint32_t firstBinding,
                                                uint32_t bindingCount, const VkBuffer* pBuffers,
                                                const VkDeviceSize* pOffsets) {
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
    cb->device->instance->connection.send_oneway(Opcode::vkCmdDraw, request);
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
    cb->device->instance->connection.send_oneway(Opcode::vkCmdCopyImageToBuffer, request);
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
    cb->device->instance->connection.send_oneway(Opcode::vkCmdClearColorImage, request);
}

VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer handle, VkPipelineLayout layout,
                                            VkShaderStageFlags stageFlags, uint32_t offset,
                                            uint32_t size, const void* pValues) {
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
        D(CmdBeginRenderPass),
        D(CmdEndRenderPass),
        D(CmdBindPipeline),
        D(CmdBindDescriptorSets),
        D(CmdBindVertexBuffers),
        D(CmdBindIndexBuffer),
        D(CmdSetViewport),
        D(CmdSetScissor),
        D(CmdDraw),
        D(CmdDrawIndexed),
        D(CmdPipelineBarrier),
        D(CmdCopyBuffer),
        D(CmdCopyBufferToImage),
        D(CmdCopyImageToBuffer),
        D(CmdClearColorImage),
        D(CmdPushConstants),
#undef D
    };
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

}  // namespace remoting
