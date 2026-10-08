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
            c.reply_status(Status::DecodeError);                                           \
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
        const uint64_t id = result == VK_SUCCESS ? c.tables.command_buffers.add(cbs[i]) : 0;
        if (id) c.tables.command_buffer_devices[cbs[i]] = device;
        c.writer.handle(id);
    }
    c.reply();
}

void handle_FreeCommandBuffers(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkCommandPool pool = c.tables.command_pool(c.reader.handle());
    const uint32_t count = c.reader.u32();
    if (!count_fits(c.reader, count, 8)) {
        mark_oneway_error(c);
        return;
    }
    std::vector<VkCommandBuffer> cbs(count);
    for (uint32_t i = 0; i < count; ++i) {
        cbs[i] = c.tables.command_buffers.take(c.reader.handle());
        c.tables.command_buffer_devices.erase(cbs[i]);
    }
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

void handle_TrimCommandPool(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkCommandPool p=c.tables.command_pool(c.reader.handle());auto flags=c.reader.u32();if(!c.reader.ok()||!d||!p){mark_oneway_error(c);return;}auto fn=reinterpret_cast<PFN_vkTrimCommandPool>(vkGetDeviceProcAddr(d,"vkTrimCommandPool"));if(!fn){mark_oneway_error(c);return;}fn(d,p,flags);}

void handle_ResetCommandPool(Session& c) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    VkCommandPool pool = c.tables.command_pool(c.reader.handle());
    const uint32_t flags = c.reader.u32();
    if (!c.reader.ok() || device == VK_NULL_HANDLE || pool == VK_NULL_HANDLE) {
        c.reply_status(Status::DecodeError);
        return;
    }
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(vkResetCommandPool(device, pool, flags));
    c.reply();
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

void handle_CmdNextSubpass(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const int32_t contents = c.reader.i32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdNextSubpass(cb, static_cast<VkSubpassContents>(contents));
}

void handle_CmdEndRenderPass(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdEndRenderPass(cb);
}

void handle_CmdBeginRenderPass2(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    Arena arena;
    VkRenderPassBeginInfo render_pass_begin{};
    VkSubpassBeginInfo subpass_begin{};
    if (!c.reader.ok() || cb == VK_NULL_HANDLE ||
        !remoting::read_RenderPassBeginInfo(c.reader, arena, c.tables, &render_pass_begin) ||
        !remoting::read_SubpassBeginInfo(c.reader, arena, c.tables, &subpass_begin)) {
        mark_oneway_error(c);
        return;
    }
    const auto owner = c.tables.command_buffer_devices.find(cb);
    if (owner == c.tables.command_buffer_devices.end()) { mark_oneway_error(c); return; }
    auto fn = reinterpret_cast<PFN_vkCmdBeginRenderPass2KHR>(
        vkGetDeviceProcAddr(owner->second, "vkCmdBeginRenderPass2KHR"));
    if (!fn) { mark_oneway_error(c); return; }
    fn(cb, &render_pass_begin, &subpass_begin);
}

void handle_CmdNextSubpass2(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    Arena arena;
    VkSubpassBeginInfo subpass_begin{};
    VkSubpassEndInfo subpass_end{};
    if (!c.reader.ok() || cb == VK_NULL_HANDLE ||
        !remoting::read_SubpassBeginInfo(c.reader, arena, c.tables, &subpass_begin) ||
        !remoting::read_SubpassEndInfo(c.reader, arena, c.tables, &subpass_end)) {
        mark_oneway_error(c);
        return;
    }
    const auto owner = c.tables.command_buffer_devices.find(cb);
    if (owner == c.tables.command_buffer_devices.end()) { mark_oneway_error(c); return; }
    auto fn = reinterpret_cast<PFN_vkCmdNextSubpass2KHR>(
        vkGetDeviceProcAddr(owner->second, "vkCmdNextSubpass2KHR"));
    if (!fn) { mark_oneway_error(c); return; }
    fn(cb, &subpass_begin, &subpass_end);
}

void handle_CmdEndRenderPass2(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    Arena arena;
    VkSubpassEndInfo subpass_end{};
    if (!c.reader.ok() || cb == VK_NULL_HANDLE ||
        !remoting::read_SubpassEndInfo(c.reader, arena, c.tables, &subpass_end)) {
        mark_oneway_error(c);
        return;
    }
    const auto owner = c.tables.command_buffer_devices.find(cb);
    if (owner == c.tables.command_buffer_devices.end()) { mark_oneway_error(c); return; }
    auto fn = reinterpret_cast<PFN_vkCmdEndRenderPass2KHR>(
        vkGetDeviceProcAddr(owner->second, "vkCmdEndRenderPass2KHR"));
    if (!fn) { mark_oneway_error(c); return; }
    fn(cb, &subpass_end);
}

void handle_CmdSetFragmentShadingRateKHR(Session& c) {
    VkCommandBuffer cb=c.tables.command_buffer(c.reader.handle()); VkExtent2D size{c.reader.u32(),c.reader.u32()};
    VkFragmentShadingRateCombinerOpKHR ops[2]{static_cast<VkFragmentShadingRateCombinerOpKHR>(c.reader.i32()),static_cast<VkFragmentShadingRateCombinerOpKHR>(c.reader.i32())};
    auto owner=c.tables.command_buffer_devices.find(cb);
    if(!c.reader.ok()||cb==VK_NULL_HANDLE||owner==c.tables.command_buffer_devices.end()){mark_oneway_error(c);return;}
    auto fn=reinterpret_cast<PFN_vkCmdSetFragmentShadingRateKHR>(vkGetDeviceProcAddr(owner->second,"vkCmdSetFragmentShadingRateKHR"));
    if(!fn){mark_oneway_error(c);return;} fn(cb,&size,ops);
}

void handle_CmdSetDeviceMask(Session& c){VkCommandBuffer cb=c.tables.command_buffer(c.reader.handle());uint32_t mask=c.reader.u32();if(!c.reader.ok()||!cb){mark_oneway_error(c);return;}auto d=c.tables.command_buffer_devices.find(cb);if(d==c.tables.command_buffer_devices.end()){mark_oneway_error(c);return;}auto fn=reinterpret_cast<PFN_vkCmdSetDeviceMask>(vkGetDeviceProcAddr(d->second,"vkCmdSetDeviceMask"));if(!fn){mark_oneway_error(c);return;}fn(cb,mask);}
void handle_CmdDispatchBase(Session& c){VkCommandBuffer cb=c.tables.command_buffer(c.reader.handle());uint32_t bx=c.reader.u32(),by=c.reader.u32(),bz=c.reader.u32(),x=c.reader.u32(),y=c.reader.u32(),z=c.reader.u32();if(!c.reader.ok()||!cb){mark_oneway_error(c);return;}auto d=c.tables.command_buffer_devices.find(cb);if(d==c.tables.command_buffer_devices.end()){mark_oneway_error(c);return;}auto fn=reinterpret_cast<PFN_vkCmdDispatchBase>(vkGetDeviceProcAddr(d->second,"vkCmdDispatchBase"));if(!fn){mark_oneway_error(c);return;}fn(cb,bx,by,bz,x,y,z);}

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
    if (!count_fits(c.reader, count, 8)) {
        mark_oneway_error(c);
        return;
    }
    std::vector<VkDescriptorSet> sets(count);
    for (uint32_t i = 0; i < count; ++i) sets[i] = c.tables.descriptor_set(c.reader.handle());
    const uint32_t dyn_count = c.reader.u32();
    if (!count_fits(c.reader, dyn_count, 4)) {
        mark_oneway_error(c);
        return;
    }
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
    if (!count_fits(c.reader, count, 16)) {
        mark_oneway_error(c);
        return;
    }
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
    if (!count_fits(c.reader, count, 4)) {
        mark_oneway_error(c);
        return;
    }
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
    if (!count_fits(c.reader, count, 4)) {
        mark_oneway_error(c);
        return;
    }
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

void handle_CmdDispatch(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t group_count_x = c.reader.u32();
    const uint32_t group_count_y = c.reader.u32();
    const uint32_t group_count_z = c.reader.u32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdDispatch(cb, group_count_x, group_count_y, group_count_z);
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
    // read_MemoryBarrier is read_raw() directly, whose bytes() call consumes
    // at least a 4-byte length prefix.
    if (!count_fits(c.reader, mem_count, 4)) {
        mark_oneway_error(c);
        return;
    }
    std::vector<VkMemoryBarrier> mem_barriers(mem_count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < mem_count; ++i) {
        ok = remoting::read_MemoryBarrier(c.reader, arena, c.tables, &mem_barriers[i]);
    }
    const uint32_t buf_count = ok ? c.reader.u32() : 0;
    if (ok && !count_fits(c.reader, buf_count, 4)) ok = false;
    std::vector<VkBufferMemoryBarrier> buf_barriers(ok ? buf_count : 0);
    for (uint32_t i = 0; ok && i < buf_count; ++i) {
        ok = remoting::read_BufferMemoryBarrier(c.reader, arena, c.tables, &buf_barriers[i]);
    }
    const uint32_t img_count = ok ? c.reader.u32() : 0;
    if (ok && !count_fits(c.reader, img_count, 4)) ok = false;
    std::vector<VkImageMemoryBarrier> img_barriers(ok ? img_count : 0);
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
    if (!count_fits(c.reader, count, 4)) {
        mark_oneway_error(c);
        return;
    }
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
    if (!count_fits(c.reader, count, 4)) {
        mark_oneway_error(c);
        return;
    }
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
    if (!count_fits(c.reader, count, 4)) {
        mark_oneway_error(c);
        return;
    }
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

void handle_CmdCopyImage(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkImage src = c.tables.image(c.reader.handle());
    const int32_t src_layout = c.reader.i32();
    VkImage dst = c.tables.image(c.reader.handle());
    const int32_t dst_layout = c.reader.i32();
    const uint32_t count = c.reader.u32();
    if (count == 0 || !count_fits(c.reader, count, 4)) {
        mark_oneway_error(c);
        return;
    }
    Arena arena;
    std::vector<VkImageCopy> regions(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_ImageCopy(c.reader, arena, c.tables, &regions[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE || src == VK_NULL_HANDLE || dst == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdCopyImage(cb, src, static_cast<VkImageLayout>(src_layout), dst,
                   static_cast<VkImageLayout>(dst_layout), count, regions.data());
}

void handle_CmdBlitImage(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkImage src = c.tables.image(c.reader.handle());
    const int32_t src_layout = c.reader.i32();
    VkImage dst = c.tables.image(c.reader.handle());
    const int32_t dst_layout = c.reader.i32();
    const uint32_t count = c.reader.u32();
    if (count == 0 || !count_fits(c.reader, count, 4)) {
        mark_oneway_error(c);
        return;
    }
    Arena arena;
    std::vector<VkImageBlit> regions(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_ImageBlit(c.reader, arena, c.tables, &regions[i]);
    }
    const int32_t filter = ok ? c.reader.i32() : 0;
    if (!ok || !c.reader.ok() || cb == VK_NULL_HANDLE || src == VK_NULL_HANDLE ||
        dst == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBlitImage(cb, src, static_cast<VkImageLayout>(src_layout), dst,
                   static_cast<VkImageLayout>(dst_layout), count, regions.data(),
                   static_cast<VkFilter>(filter));
}

void handle_CmdFillBuffer(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkBuffer dst = c.tables.buffer(c.reader.handle());
    const VkDeviceSize dst_offset = c.reader.u64();
    const VkDeviceSize size = c.reader.u64();
    const uint32_t data = c.reader.u32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE || dst == VK_NULL_HANDLE ||
        (dst_offset & 3) != 0 || (size != VK_WHOLE_SIZE && (size == 0 || (size & 3) != 0))) {
        mark_oneway_error(c);
        return;
    }
    vkCmdFillBuffer(cb, dst, dst_offset, size, data);
}

void handle_CmdUpdateBuffer(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkBuffer dst = c.tables.buffer(c.reader.handle());
    const VkDeviceSize dst_offset = c.reader.u64();
    const VkDeviceSize data_size = c.reader.u64();
    std::vector<char> data;
    const bool bytes_ok = c.reader.bytes(&data, 65536);
    if (!bytes_ok || !c.reader.ok() || cb == VK_NULL_HANDLE || dst == VK_NULL_HANDLE ||
        (dst_offset & 3) != 0 || data_size == 0 || data_size > 65536 ||
        (data_size & 3) != 0 || data.size() != data_size) {
        mark_oneway_error(c);
        return;
    }
    vkCmdUpdateBuffer(cb, dst, dst_offset, data_size, data.data());
}

void handle_CmdClearColorImage(Session& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkImage image = c.tables.image(c.reader.handle());
    const int32_t layout = c.reader.i32();
    std::vector<char> color_bytes;
    c.reader.bytes(&color_bytes);
    const uint32_t count = c.reader.u32();
    if (!count_fits(c.reader, count, 4)) {
        mark_oneway_error(c);
        return;
    }
    std::vector<VkImageSubresourceRange> ranges(count);
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; ++i) {
        std::vector<char> raw;
        if (!c.reader.bytes(&raw) || raw.size() != sizeof(VkImageSubresourceRange)) {
            ok = false;
            break;
        }
        memcpy(&ranges[i], raw.data(), sizeof(VkImageSubresourceRange));
    }
    if (!ok || !c.reader.ok() || cb == VK_NULL_HANDLE || color_bytes.size() != sizeof(VkClearColorValue)) {
        mark_oneway_error(c);
        return;
    }
    VkClearColorValue color{};
    memcpy(&color, color_bytes.data(), sizeof(color));
    vkCmdClearColorImage(cb, image, static_cast<VkImageLayout>(layout), &color, count,
                        ranges.data());
}

void handle_CmdDrawIndirect(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto b=c.tables.buffer(c.reader.handle());auto off=c.reader.u64();auto n=c.reader.u32();auto stride=c.reader.u32();if(!c.reader.ok()||!cb||!b){mark_oneway_error(c);return;}vkCmdDrawIndirect(cb,b,off,n,stride);}
void handle_CmdDrawIndexedIndirect(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto b=c.tables.buffer(c.reader.handle());auto off=c.reader.u64();auto n=c.reader.u32();auto stride=c.reader.u32();if(!c.reader.ok()||!cb||!b){mark_oneway_error(c);return;}vkCmdDrawIndexedIndirect(cb,b,off,n,stride);}
void handle_CmdDispatchIndirect(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto b=c.tables.buffer(c.reader.handle());auto off=c.reader.u64();if(!c.reader.ok()||!cb||!b){mark_oneway_error(c);return;}vkCmdDispatchIndirect(cb,b,off);}
void handle_CmdDrawIndirectCount(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto b=c.tables.buffer(c.reader.handle());auto off=c.reader.u64();auto count=c.tables.buffer(c.reader.handle());auto countOffset=c.reader.u64();auto maxDrawCount=c.reader.u32();auto stride=c.reader.u32();if(!c.reader.ok()||!cb||!b||!count){mark_oneway_error(c);return;}auto fn=reinterpret_cast<PFN_vkCmdDrawIndirectCount>(vkGetDeviceProcAddr(c.tables.command_buffer_devices[cb],"vkCmdDrawIndirectCount"));if(!fn){mark_oneway_error(c);return;}fn(cb,b,off,count,countOffset,maxDrawCount,stride);}
void handle_CmdDrawIndexedIndirectCount(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto b=c.tables.buffer(c.reader.handle());auto off=c.reader.u64();auto count=c.tables.buffer(c.reader.handle());auto countOffset=c.reader.u64();auto maxDrawCount=c.reader.u32();auto stride=c.reader.u32();if(!c.reader.ok()||!cb||!b||!count){mark_oneway_error(c);return;}auto fn=reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(vkGetDeviceProcAddr(c.tables.command_buffer_devices[cb],"vkCmdDrawIndexedIndirectCount"));if(!fn){mark_oneway_error(c);return;}fn(cb,b,off,count,countOffset,maxDrawCount,stride);}
void handle_CmdExecuteCommands(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto n=c.reader.u32();if(n>remoting::kMaxArrayElements||!count_fits(c.reader,n,8)){mark_oneway_error(c);return;}std::vector<VkCommandBuffer> v(n);for(uint32_t i=0;i<n;++i)v[i]=c.tables.command_buffer(c.reader.handle());if(!c.reader.ok()||!cb){mark_oneway_error(c);return;}vkCmdExecuteCommands(cb,n,v.data());}
void handle_CmdBeginQuery(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto p=c.tables.query_pool(c.reader.handle());auto q=c.reader.u32();auto f=c.reader.u32();if(!c.reader.ok()||!cb||!p){mark_oneway_error(c);return;}vkCmdBeginQuery(cb,p,q,f);}
void handle_CmdEndQuery(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto p=c.tables.query_pool(c.reader.handle());auto q=c.reader.u32();if(!c.reader.ok()||!cb||!p){mark_oneway_error(c);return;}vkCmdEndQuery(cb,p,q);}
void handle_CmdResetQueryPool(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto p=c.tables.query_pool(c.reader.handle());auto first=c.reader.u32();auto n=c.reader.u32();if(!c.reader.ok()||!cb||!p){mark_oneway_error(c);return;}vkCmdResetQueryPool(cb,p,first,n);}
void handle_CmdWriteTimestamp(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto stage=c.reader.u32();auto p=c.tables.query_pool(c.reader.handle());auto q=c.reader.u32();if(!c.reader.ok()||!cb||!p){mark_oneway_error(c);return;}vkCmdWriteTimestamp(cb,static_cast<VkPipelineStageFlagBits>(stage),p,q);}
void handle_CmdCopyQueryPoolResults(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto p=c.tables.query_pool(c.reader.handle());auto first=c.reader.u32();auto n=c.reader.u32();auto b=c.tables.buffer(c.reader.handle());auto off=c.reader.u64();auto stride=c.reader.u64();auto flags=c.reader.u32();if(!c.reader.ok()||!cb||!p||!b){mark_oneway_error(c);return;}vkCmdCopyQueryPoolResults(cb,p,first,n,b,off,stride,flags);}
void handle_CmdSetEvent(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto e=c.tables.event(c.reader.handle());auto stage=c.reader.u32();if(!c.reader.ok()||!cb||!e){mark_oneway_error(c);return;}vkCmdSetEvent(cb,e,stage);}
void handle_CmdResetEvent(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto e=c.tables.event(c.reader.handle());auto stage=c.reader.u32();if(!c.reader.ok()||!cb||!e){mark_oneway_error(c);return;}vkCmdResetEvent(cb,e,stage);}
void handle_CmdSetLineWidth(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto v=c.reader.f32();if(!c.reader.ok()||!cb){mark_oneway_error(c);return;}vkCmdSetLineWidth(cb,v);}
void handle_CmdSetDepthBias(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto a=c.reader.f32(),b=c.reader.f32(),d=c.reader.f32();if(!c.reader.ok()||!cb){mark_oneway_error(c);return;}vkCmdSetDepthBias(cb,a,b,d);}
void handle_CmdSetBlendConstants(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());float v[4];for(auto&x:v)x=c.reader.f32();if(!c.reader.ok()||!cb){mark_oneway_error(c);return;}vkCmdSetBlendConstants(cb,v);}
void handle_CmdSetDepthBounds(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto a=c.reader.f32(),b=c.reader.f32();if(!c.reader.ok()||!cb){mark_oneway_error(c);return;}vkCmdSetDepthBounds(cb,a,b);}
void handle_CmdSetStencilCompareMask(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto f=c.reader.u32(),m=c.reader.u32();if(!c.reader.ok()||!cb){mark_oneway_error(c);return;}vkCmdSetStencilCompareMask(cb,f,m);}
void handle_CmdSetStencilWriteMask(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto f=c.reader.u32(),m=c.reader.u32();if(!c.reader.ok()||!cb){mark_oneway_error(c);return;}vkCmdSetStencilWriteMask(cb,f,m);}
void handle_CmdSetStencilReference(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto f=c.reader.u32(),m=c.reader.u32();if(!c.reader.ok()||!cb){mark_oneway_error(c);return;}vkCmdSetStencilReference(cb,f,m);}

void handle_CmdResolveImage(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto src=c.tables.image(c.reader.handle());auto sl=static_cast<VkImageLayout>(c.reader.i32());auto dst=c.tables.image(c.reader.handle());auto dl=static_cast<VkImageLayout>(c.reader.i32());auto n=c.reader.u32();if(n>remoting::kMaxArrayElements||!count_fits(c.reader,n,64)){mark_oneway_error(c);return;}std::vector<VkImageResolve> v(n);for(auto&r:v){r.srcSubresource.aspectMask=c.reader.u32();r.srcSubresource.mipLevel=c.reader.u32();r.srcSubresource.baseArrayLayer=c.reader.u32();r.srcSubresource.layerCount=c.reader.u32();r.srcOffset.x=c.reader.i32();r.srcOffset.y=c.reader.i32();r.srcOffset.z=c.reader.i32();r.dstSubresource.aspectMask=c.reader.u32();r.dstSubresource.mipLevel=c.reader.u32();r.dstSubresource.baseArrayLayer=c.reader.u32();r.dstSubresource.layerCount=c.reader.u32();r.dstOffset.x=c.reader.i32();r.dstOffset.y=c.reader.i32();r.dstOffset.z=c.reader.i32();r.extent.width=c.reader.u32();r.extent.height=c.reader.u32();r.extent.depth=c.reader.u32();}if(!c.reader.ok()||!cb||!src||!dst){mark_oneway_error(c);return;}vkCmdResolveImage(cb,src,sl,dst,dl,n,v.data());}
void handle_CmdClearDepthStencilImage(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto image=c.tables.image(c.reader.handle());auto layout=static_cast<VkImageLayout>(c.reader.i32());VkClearDepthStencilValue value{c.reader.f32(),c.reader.u32()};auto n=c.reader.u32();if(n>remoting::kMaxArrayElements||!count_fits(c.reader,n,20)){mark_oneway_error(c);return;}std::vector<VkImageSubresourceRange> v(n);for(auto&r:v){r.aspectMask=c.reader.u32();r.baseMipLevel=c.reader.u32();r.levelCount=c.reader.u32();r.baseArrayLayer=c.reader.u32();r.layerCount=c.reader.u32();}if(!c.reader.ok()||!cb||!image){mark_oneway_error(c);return;}vkCmdClearDepthStencilImage(cb,image,layout,&value,n,v.data());}
void handle_CmdClearAttachments(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto an=c.reader.u32();if(an>remoting::kMaxArrayElements||!count_fits(c.reader,an,12)){mark_oneway_error(c);return;}std::vector<VkClearAttachment>a(an);bool ok=true;for(auto&x:a){x.aspectMask=c.reader.u32();x.colorAttachment=c.reader.u32();std::vector<char>b;if(!c.reader.bytes(&b,sizeof(VkClearValue))||b.size()!=sizeof(VkClearValue)){ok=false;break;}memcpy(&x.clearValue,b.data(),sizeof(VkClearValue));}auto rn=ok?c.reader.u32():0;if(rn>remoting::kMaxArrayElements||!count_fits(c.reader,rn,24)){mark_oneway_error(c);return;}std::vector<VkClearRect>r(rn);for(auto&x:r){x.rect.offset.x=c.reader.i32();x.rect.offset.y=c.reader.i32();x.rect.extent.width=c.reader.u32();x.rect.extent.height=c.reader.u32();x.baseArrayLayer=c.reader.u32();x.layerCount=c.reader.u32();}if(!ok||!c.reader.ok()||!cb){mark_oneway_error(c);return;}vkCmdClearAttachments(cb,an,a.data(),rn,r.data());}
void handle_CmdWaitEvents(Session& c){auto cb=c.tables.command_buffer(c.reader.handle());auto en=c.reader.u32();if(en>remoting::kMaxArrayElements||!count_fits(c.reader,en,8)){mark_oneway_error(c);return;}std::vector<VkEvent>events(en);for(auto&e:events)e=c.tables.event(c.reader.handle());auto src=c.reader.u32(),dst=c.reader.u32();Arena arena;auto mn=c.reader.u32();if(mn>remoting::kMaxArrayElements||!count_fits(c.reader,mn,4)){mark_oneway_error(c);return;}std::vector<VkMemoryBarrier>m(mn);bool ok=true;for(auto&x:m)ok=ok&&remoting::read_MemoryBarrier(c.reader,arena,c.tables,&x);auto bn=ok?c.reader.u32():0;if(bn>remoting::kMaxArrayElements||!count_fits(c.reader,bn,4)){mark_oneway_error(c);return;}std::vector<VkBufferMemoryBarrier>b(bn);for(auto&x:b)ok=ok&&remoting::read_BufferMemoryBarrier(c.reader,arena,c.tables,&x);auto in=ok?c.reader.u32():0;if(in>remoting::kMaxArrayElements||!count_fits(c.reader,in,4)){mark_oneway_error(c);return;}std::vector<VkImageMemoryBarrier>i(in);for(auto&x:i)ok=ok&&remoting::read_ImageMemoryBarrier(c.reader,arena,c.tables,&x);if(!ok||!c.reader.ok()||!cb){mark_oneway_error(c);return;}vkCmdWaitEvents(cb,en,events.data(),src,dst,mn,m.data(),bn,b.data(),in,i.data());}

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
REGISTER_HANDLER(vkResetCommandPool, handle_ResetCommandPool);
REGISTER_HANDLER(vkTrimCommandPool, handle_TrimCommandPool);
REGISTER_HANDLER(vkCmdBeginRenderPass, handle_CmdBeginRenderPass);
REGISTER_HANDLER(vkCmdNextSubpass, handle_CmdNextSubpass);
REGISTER_HANDLER(vkCmdEndRenderPass, handle_CmdEndRenderPass);
REGISTER_HANDLER(vkCmdBeginRenderPass2, handle_CmdBeginRenderPass2);
REGISTER_HANDLER(vkCmdNextSubpass2, handle_CmdNextSubpass2);
REGISTER_HANDLER(vkCmdEndRenderPass2, handle_CmdEndRenderPass2);
REGISTER_HANDLER(vkCmdSetFragmentShadingRateKHR, handle_CmdSetFragmentShadingRateKHR);
REGISTER_HANDLER(vkCmdSetDeviceMask, handle_CmdSetDeviceMask);
REGISTER_HANDLER(vkCmdDispatchBase, handle_CmdDispatchBase);
REGISTER_HANDLER(vkCmdBindPipeline, handle_CmdBindPipeline);
REGISTER_HANDLER(vkCmdBindDescriptorSets, handle_CmdBindDescriptorSets);
REGISTER_HANDLER(vkCmdBindVertexBuffers, handle_CmdBindVertexBuffers);
REGISTER_HANDLER(vkCmdBindIndexBuffer, handle_CmdBindIndexBuffer);
REGISTER_HANDLER(vkCmdSetViewport, handle_CmdSetViewport);
REGISTER_HANDLER(vkCmdSetScissor, handle_CmdSetScissor);
REGISTER_HANDLER(vkCmdDraw, handle_CmdDraw);
REGISTER_HANDLER(vkCmdDispatch, handle_CmdDispatch);
REGISTER_HANDLER(vkCmdDrawIndexed, handle_CmdDrawIndexed);
REGISTER_HANDLER(vkCmdDrawIndirect, handle_CmdDrawIndirect);
REGISTER_HANDLER(vkCmdDrawIndexedIndirect, handle_CmdDrawIndexedIndirect);
REGISTER_HANDLER(vkCmdDrawIndirectCount, handle_CmdDrawIndirectCount);
REGISTER_HANDLER(vkCmdDrawIndexedIndirectCount, handle_CmdDrawIndexedIndirectCount);
REGISTER_HANDLER(vkCmdDispatchIndirect, handle_CmdDispatchIndirect);
REGISTER_HANDLER(vkCmdExecuteCommands, handle_CmdExecuteCommands);
REGISTER_HANDLER(vkCmdBeginQuery, handle_CmdBeginQuery);
REGISTER_HANDLER(vkCmdEndQuery, handle_CmdEndQuery);
REGISTER_HANDLER(vkCmdResetQueryPool, handle_CmdResetQueryPool);
REGISTER_HANDLER(vkCmdWriteTimestamp, handle_CmdWriteTimestamp);
REGISTER_HANDLER(vkCmdCopyQueryPoolResults, handle_CmdCopyQueryPoolResults);
REGISTER_HANDLER(vkCmdSetEvent, handle_CmdSetEvent);
REGISTER_HANDLER(vkCmdResetEvent, handle_CmdResetEvent);
REGISTER_HANDLER(vkCmdSetLineWidth, handle_CmdSetLineWidth);
REGISTER_HANDLER(vkCmdSetDepthBias, handle_CmdSetDepthBias);
REGISTER_HANDLER(vkCmdSetBlendConstants, handle_CmdSetBlendConstants);
REGISTER_HANDLER(vkCmdSetDepthBounds, handle_CmdSetDepthBounds);
REGISTER_HANDLER(vkCmdSetStencilCompareMask, handle_CmdSetStencilCompareMask);
REGISTER_HANDLER(vkCmdSetStencilWriteMask, handle_CmdSetStencilWriteMask);
REGISTER_HANDLER(vkCmdSetStencilReference, handle_CmdSetStencilReference);
REGISTER_HANDLER(vkCmdPipelineBarrier, handle_CmdPipelineBarrier);
REGISTER_HANDLER(vkCmdCopyBuffer, handle_CmdCopyBuffer);
REGISTER_HANDLER(vkCmdCopyBufferToImage, handle_CmdCopyBufferToImage);
REGISTER_HANDLER(vkCmdCopyImageToBuffer, handle_CmdCopyImageToBuffer);
REGISTER_HANDLER(vkCmdCopyImage, handle_CmdCopyImage);
REGISTER_HANDLER(vkCmdBlitImage, handle_CmdBlitImage);
REGISTER_HANDLER(vkCmdFillBuffer, handle_CmdFillBuffer);
REGISTER_HANDLER(vkCmdUpdateBuffer, handle_CmdUpdateBuffer);
REGISTER_HANDLER(vkCmdClearColorImage, handle_CmdClearColorImage);
REGISTER_HANDLER(vkCmdClearDepthStencilImage, handle_CmdClearDepthStencilImage);
REGISTER_HANDLER(vkCmdClearAttachments, handle_CmdClearAttachments);
REGISTER_HANDLER(vkCmdResolveImage, handle_CmdResolveImage);
REGISTER_HANDLER(vkCmdWaitEvents, handle_CmdWaitEvents);
REGISTER_HANDLER(vkCmdPushConstants, handle_CmdPushConstants);
