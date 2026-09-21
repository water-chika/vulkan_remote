// Server-side command handlers: command pool lifecycle, command buffer
// allocation/free/begin/end/reset, and every vkCmd* recording call. These
// never reply (recording is oneway - the client has already moved on by the
// time this runs), except for the pool/buffer lifecycle calls that hand
// back a result or a new handle.

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

SERVER_CREATE(CommandPool, VkCommandPool, VkCommandPoolCreateInfo,
              remoting::read_CommandPoolCreateInfo, vkCreateCommandPool, command_pools)
SERVER_DESTROY(CommandPool, VkCommandPool, vkDestroyCommandPool, command_pools)

#undef SERVER_CREATE
#undef SERVER_DESTROY

void handle_AllocateCommandBuffers(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkCommandBufferAllocateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_CommandBufferAllocateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    std::vector<VkCommandBuffer> cbs(info.commandBufferCount);
    const VkResult result = vkAllocateCommandBuffers(device, &info, cbs.data());
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(info.commandBufferCount);
    for (uint32_t i = 0; i < info.commandBufferCount; ++i) {
        c.writer.handle(result == VK_SUCCESS ? c.tables.command_buffers.add(cbs[i]) : 0);
    }
    c.reply();
}

void handle_FreeCommandBuffers(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkCommandPool pool = c.tables.command_pool(c.reader.handle());
    const uint32_t count = c.reader.u32();
    std::vector<VkCommandBuffer> cbs(count);
    for (uint32_t i = 0; i < count; ++i) cbs[i] = c.tables.command_buffers.take(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkFreeCommandBuffers(device, pool, count, cbs.data());
}

void handle_BeginCommandBuffer(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    Arena arena;
    VkCommandBufferBeginInfo info{};
    if (!c.reader.ok() || cb == VK_NULL_HANDLE ||
        !remoting::read_CommandBufferBeginInfo(c.reader, arena, c.tables, &info)) {
        mark_oneway_error(c);
        return;
    }
    if (vkBeginCommandBuffer(cb, &info) != VK_SUCCESS) mark_oneway_error(c);
}

void handle_EndCommandBuffer(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) mark_oneway_error(c);
}

void handle_ResetCommandBuffer(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t flags = c.reader.u32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    if (vkResetCommandBuffer(cb, flags) != VK_SUCCESS) mark_oneway_error(c);
}

void handle_CmdBeginRenderPass(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    Arena arena;
    VkRenderPassBeginInfo info{};
    if (!c.reader.ok() || cb == VK_NULL_HANDLE ||
        !remoting::read_RenderPassBeginInfo(c.reader, arena, c.tables, &info)) {
        mark_oneway_error(c);
        return;
    }
    const int32_t contents = c.reader.i32();
    if (!c.reader.ok()) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBeginRenderPass(cb, &info, static_cast<VkSubpassContents>(contents));
}

void handle_CmdEndRenderPass(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdEndRenderPass(cb);
}

void handle_CmdBindPipeline(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const int32_t bind_point = c.reader.i32();
    VkPipeline pipeline = c.tables.pipeline(c.reader.handle());
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBindPipeline(cb, static_cast<VkPipelineBindPoint>(bind_point), pipeline);
}

void handle_CmdBindDescriptorSets(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const int32_t bind_point = c.reader.i32();
    VkPipelineLayout layout = c.tables.pipeline_layout(c.reader.handle());
    const uint32_t first_set = c.reader.u32();
    const uint32_t count = c.reader.u32();
    std::vector<VkDescriptorSet> sets(count);
    for (uint32_t i = 0; i < count; ++i) sets[i] = c.tables.descriptor_set(c.reader.handle());
    const uint32_t dyn_count = c.reader.u32();
    std::vector<uint32_t> offsets(dyn_count);
    for (uint32_t i = 0; i < dyn_count; ++i) offsets[i] = c.reader.u32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBindDescriptorSets(cb, static_cast<VkPipelineBindPoint>(bind_point), layout, first_set,
                            count, sets.data(), dyn_count, offsets.data());
}

void handle_CmdBindVertexBuffers(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t first = c.reader.u32();
    const uint32_t count = c.reader.u32();
    std::vector<VkBuffer> buffers(count);
    std::vector<VkDeviceSize> offsets(count);
    for (uint32_t i = 0; i < count; ++i) {
        buffers[i] = c.tables.buffer(c.reader.handle());
        offsets[i] = c.reader.u64();
    }
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBindVertexBuffers(cb, first, count, buffers.data(), offsets.data());
}

void handle_CmdBindIndexBuffer(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkBuffer buffer = c.tables.buffer(c.reader.handle());
    const uint64_t offset = c.reader.u64();
    const int32_t index_type = c.reader.i32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBindIndexBuffer(cb, buffer, offset, static_cast<VkIndexType>(index_type));
}

void handle_CmdSetViewport(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t first = c.reader.u32();
    const uint32_t count = c.reader.u32();
    Arena arena;
    std::vector<VkViewport> viewports(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_Viewport(c.reader, arena, c.tables, &viewports[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdSetViewport(cb, first, count, viewports.data());
}

void handle_CmdSetScissor(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t first = c.reader.u32();
    const uint32_t count = c.reader.u32();
    Arena arena;
    std::vector<VkRect2D> scissors(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_Rect2D(c.reader, arena, c.tables, &scissors[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdSetScissor(cb, first, count, scissors.data());
}

void handle_CmdDraw(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t vertex_count = c.reader.u32();
    const uint32_t instance_count = c.reader.u32();
    const uint32_t first_vertex = c.reader.u32();
    const uint32_t first_instance = c.reader.u32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdDraw(cb, vertex_count, instance_count, first_vertex, first_instance);
}

void handle_CmdDrawIndexed(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t index_count = c.reader.u32();
    const uint32_t instance_count = c.reader.u32();
    const uint32_t first_index = c.reader.u32();
    const int32_t vertex_offset = c.reader.i32();
    const uint32_t first_instance = c.reader.u32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdDrawIndexed(cb, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void handle_CmdPipelineBarrier(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t src_stage = c.reader.u32();
    const uint32_t dst_stage = c.reader.u32();
    const uint32_t dep_flags = c.reader.u32();

    Arena arena;
    const uint32_t mem_count = c.reader.u32();
    std::vector<VkMemoryBarrier> mem_barriers(mem_count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < mem_count; ++i) {
        ok = remoting::read_MemoryBarrier(c.reader, arena, c.tables, &mem_barriers[i]);
    }
    const uint32_t buf_count = ok ? c.reader.u32() : 0;
    std::vector<VkBufferMemoryBarrier> buf_barriers(buf_count);
    for (uint32_t i = 0; ok && i < buf_count; ++i) {
        ok = remoting::read_BufferMemoryBarrier(c.reader, arena, c.tables, &buf_barriers[i]);
    }
    const uint32_t img_count = ok ? c.reader.u32() : 0;
    std::vector<VkImageMemoryBarrier> img_barriers(img_count);
    for (uint32_t i = 0; ok && i < img_count; ++i) {
        ok = remoting::read_ImageMemoryBarrier(c.reader, arena, c.tables, &img_barriers[i]);
    }

    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, dep_flags, mem_count, mem_barriers.data(),
                         buf_count, buf_barriers.data(), img_count, img_barriers.data());
}

void handle_CmdCopyBuffer(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkBuffer src = c.tables.buffer(c.reader.handle());
    VkBuffer dst = c.tables.buffer(c.reader.handle());
    const uint32_t count = c.reader.u32();
    Arena arena;
    std::vector<VkBufferCopy> regions(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_BufferCopy(c.reader, arena, c.tables, &regions[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdCopyBuffer(cb, src, dst, count, regions.data());
}

void handle_CmdCopyBufferToImage(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkBuffer src = c.tables.buffer(c.reader.handle());
    VkImage dst = c.tables.image(c.reader.handle());
    const int32_t layout = c.reader.i32();
    const uint32_t count = c.reader.u32();
    Arena arena;
    std::vector<VkBufferImageCopy> regions(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_BufferImageCopy(c.reader, arena, c.tables, &regions[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdCopyBufferToImage(cb, src, dst, static_cast<VkImageLayout>(layout), count,
                           regions.data());
}

void handle_CmdCopyImageToBuffer(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkImage src = c.tables.image(c.reader.handle());
    const int32_t layout = c.reader.i32();
    VkBuffer dst = c.tables.buffer(c.reader.handle());
    const uint32_t count = c.reader.u32();
    Arena arena;
    std::vector<VkBufferImageCopy> regions(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_BufferImageCopy(c.reader, arena, c.tables, &regions[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdCopyImageToBuffer(cb, src, static_cast<VkImageLayout>(layout), dst, count,
                           regions.data());
}

void handle_CmdClearColorImage(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkImage image = c.tables.image(c.reader.handle());
    const int32_t layout = c.reader.i32();
    std::vector<char> color_bytes;
    c.reader.bytes(&color_bytes);
    const uint32_t count = c.reader.u32();
    std::vector<VkImageSubresourceRange> ranges(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<char> raw;
        if (!c.reader.bytes(&raw) || raw.size() != sizeof(VkImageSubresourceRange)) continue;
        memcpy(&ranges[i], raw.data(), sizeof(VkImageSubresourceRange));
    }
    if (!c.reader.ok() || cb == VK_NULL_HANDLE || color_bytes.size() != sizeof(VkClearColorValue)) {
        mark_oneway_error(c);
        return;
    }
    VkClearColorValue color{};
    memcpy(&color, color_bytes.data(), sizeof(color));
    vkCmdClearColorImage(cb, image, static_cast<VkImageLayout>(layout), &color, count,
                        ranges.data());
}

void handle_CmdPushConstants(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkPipelineLayout layout = c.tables.pipeline_layout(c.reader.handle());
    const uint32_t stage_flags = c.reader.u32();
    const uint32_t offset = c.reader.u32();
    std::vector<char> values;
    if (!c.reader.bytes(&values) || !c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdPushConstants(cb, layout, stage_flags, offset, static_cast<uint32_t>(values.size()),
                       values.data());
}


}  // namespace

REGISTER_HANDLER(vkCreateCommandPool, handle_CreateCommandPool);
REGISTER_HANDLER(vkDestroyCommandPool, handle_DestroyCommandPool);
REGISTER_HANDLER(vkAllocateCommandBuffers, handle_AllocateCommandBuffers);
REGISTER_HANDLER(vkFreeCommandBuffers, handle_FreeCommandBuffers);
REGISTER_HANDLER(vkBeginCommandBuffer, handle_BeginCommandBuffer);
REGISTER_HANDLER(vkEndCommandBuffer, handle_EndCommandBuffer);
REGISTER_HANDLER(vkResetCommandBuffer, handle_ResetCommandBuffer);
REGISTER_HANDLER(vkCmdBeginRenderPass, handle_CmdBeginRenderPass);
REGISTER_HANDLER(vkCmdEndRenderPass, handle_CmdEndRenderPass);
REGISTER_HANDLER(vkCmdBindPipeline, handle_CmdBindPipeline);
REGISTER_HANDLER(vkCmdBindDescriptorSets, handle_CmdBindDescriptorSets);
REGISTER_HANDLER(vkCmdBindVertexBuffers, handle_CmdBindVertexBuffers);
REGISTER_HANDLER(vkCmdBindIndexBuffer, handle_CmdBindIndexBuffer);
REGISTER_HANDLER(vkCmdSetViewport, handle_CmdSetViewport);
REGISTER_HANDLER(vkCmdSetScissor, handle_CmdSetScissor);
REGISTER_HANDLER(vkCmdDraw, handle_CmdDraw);
REGISTER_HANDLER(vkCmdDrawIndexed, handle_CmdDrawIndexed);
REGISTER_HANDLER(vkCmdPipelineBarrier, handle_CmdPipelineBarrier);
REGISTER_HANDLER(vkCmdCopyBuffer, handle_CmdCopyBuffer);
REGISTER_HANDLER(vkCmdCopyBufferToImage, handle_CmdCopyBufferToImage);
REGISTER_HANDLER(vkCmdCopyImageToBuffer, handle_CmdCopyImageToBuffer);
REGISTER_HANDLER(vkCmdClearColorImage, handle_CmdClearColorImage);
REGISTER_HANDLER(vkCmdPushConstants, handle_CmdPushConstants);
