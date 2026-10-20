// Server-side device-level handlers: device/queue lifecycle, fences and
// semaphores, every plain (non-command-buffer) resource creation
// (buffers, images, views, samplers, shader modules, pipeline caches,
// pipeline/descriptor-set layouts, render passes, framebuffers, graphics
// pipelines), and descriptor set allocation/update. Command pools and
// command buffers move with the vkCmd* recording functions instead, into
// handlers_command.cpp, since they are meaningless without each other.

#include <algorithm>
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
        c.reply_status(Status::DecodeError);
        return;
    }

    VkDevice device = VK_NULL_HANDLE;
    const VkResult result = vkCreateDevice(physdev, &info, nullptr, &device);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    uint64_t device_id = 0;
    if (result == VK_SUCCESS) {
        device_id = c.tables.devices.add(device);
        c.tables.device_physical_devices[device] = physdev;
    }
    c.writer.handle(device_id);
    c.reply();
}

void handle_DestroyDevice(Session& c) {
    const uint64_t id = c.reader.handle();
    VkDevice device = c.tables.devices.take(id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    remoting::destroy_device_objects(c.tables, device);
}

void handle_GetDeviceQueue(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t family = c.reader.u32();
    const uint32_t index = c.reader.u32();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.reply_status(Status::DecodeError);
        return;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, index, &queue);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.handle(c.tables.queues.add_unique(queue, device));
    c.reply();
}

void handle_GetDeviceQueue2(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkDeviceQueueInfo2 info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};info.flags=c.reader.u32();info.queueFamilyIndex=c.reader.u32();info.queueIndex=c.reader.u32();if(!c.reader.ok()||!d){c.reply_status(Status::DecodeError);return;}auto fn=reinterpret_cast<PFN_vkGetDeviceQueue2>(vkGetDeviceProcAddr(d,"vkGetDeviceQueue2"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}VkQueue q=VK_NULL_HANDLE;fn(d,&info,&q);c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.handle(q?c.tables.queues.add_unique(q, d):0);c.reply();}
void handle_GetDeviceGroupPeerMemoryFeatures(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());uint32_t heap=c.reader.u32(),local=c.reader.u32(),remote=c.reader.u32();if(!c.reader.ok()||!d){c.reply_status(Status::DecodeError);return;}auto fn=reinterpret_cast<PFN_vkGetDeviceGroupPeerMemoryFeatures>(vkGetDeviceProcAddr(d,"vkGetDeviceGroupPeerMemoryFeatures"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}VkPeerMemoryFeatureFlags flags=0;fn(d,heap,local,remote,&flags);c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.u32(flags);c.reply();}

void handle_GetSemaphoreCounterValue(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkSemaphore sem=c.tables.semaphore(c.reader.handle());if(!c.reader.ok()||!d||!sem){c.reply_status(Status::DecodeError);return;}auto fn=reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(vkGetDeviceProcAddr(d,"vkGetSemaphoreCounterValue"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}uint64_t value=0;VkResult result=fn(d,sem,&value);c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.i32(result);c.writer.u64(value);c.reply();}
void handle_WaitSemaphores(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};info.flags=c.reader.u32();info.semaphoreCount=c.reader.u32();if(info.semaphoreCount>remoting::kMaxArrayElements||!count_fits(c.reader,info.semaphoreCount,16)){c.reply_status(Status::DecodeError);return;}std::vector<VkSemaphore>sems(info.semaphoreCount);std::vector<uint64_t>values(info.semaphoreCount);bool ok=d!=VK_NULL_HANDLE;for(uint32_t i=0;i<info.semaphoreCount;++i){sems[i]=c.tables.semaphore(c.reader.handle());values[i]=c.reader.u64();ok=ok&&sems[i]!=VK_NULL_HANDLE;}uint64_t timeout=c.reader.u64();if(!ok||!c.reader.ok()){c.reply_status(Status::DecodeError);return;}info.pSemaphores=sems.data();info.pValues=values.data();auto fn=reinterpret_cast<PFN_vkWaitSemaphores>(vkGetDeviceProcAddr(d,"vkWaitSemaphores"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.i32(fn(d,&info,timeout));c.reply();}
void handle_SignalSemaphore(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};info.semaphore=c.tables.semaphore(c.reader.handle());info.value=c.reader.u64();if(!c.reader.ok()||!d||!info.semaphore){c.reply_status(Status::DecodeError);return;}auto fn=reinterpret_cast<PFN_vkSignalSemaphore>(vkGetDeviceProcAddr(d,"vkSignalSemaphore"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.i32(fn(d,&info));c.reply();}
void handle_ResetQueryPool(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkQueryPool pool=c.tables.query_pool(c.reader.handle());uint32_t first=c.reader.u32(),count=c.reader.u32();if(!c.reader.ok()||!d||!pool){mark_oneway_error(c);return;}auto fn=reinterpret_cast<PFN_vkResetQueryPool>(vkGetDeviceProcAddr(d,"vkResetQueryPool"));if(!fn){mark_oneway_error(c);return;}fn(d,pool,first,count);}

void handle_QueueSubmit(Session& c) {
    const uint64_t queue_id = c.reader.handle();
    VkQueue queue = c.tables.queues.get(queue_id);
    const uint64_t fence_id = c.reader.handle();
    VkFence fence = c.tables.fence(fence_id);
    const uint32_t submit_count = c.reader.u32();
    // Each submit has an empty pNext chain plus three array counts.
    if (submit_count > remoting::kMaxArrayElements ||
        !count_fits(c.reader, submit_count, 16)) {
        c.reply_status(Status::DecodeError);
        return;
    }

    std::vector<VkSubmitInfo> submits(submit_count);
    // Kept alive until vkQueueSubmit returns; one entry per submit so each
    // submit's arrays do not alias another's.
    std::vector<std::vector<VkSemaphore>> wait_semaphores(submit_count);
    std::vector<std::vector<VkPipelineStageFlags>> wait_stages(submit_count);
    std::vector<std::vector<VkCommandBuffer>> command_buffers(submit_count);
    std::vector<std::vector<VkSemaphore>> signal_semaphores(submit_count);
    Arena arena;

    bool ok = c.reader.ok() && queue != VK_NULL_HANDLE &&
              (fence_id == 0 || fence != VK_NULL_HANDLE);
    for (uint32_t i = 0; ok && i < submit_count; ++i) {
        VkSubmitInfo& s = submits[i];
        s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        ok = remoting::read_pnext_chain(c.reader, arena, c.tables,
                                        remoting::PNextContext::Submit, &s.pNext);

        const uint32_t wait_count = c.reader.u32();
        if (ok && (wait_count > remoting::kMaxArrayElements ||
                   !count_fits(c.reader, wait_count, 12))) ok = false;
        if (ok) {
            wait_semaphores[i].resize(wait_count);
            wait_stages[i].resize(wait_count);
        }
        for (uint32_t j = 0; j < wait_count && ok; ++j) {
            wait_semaphores[i][j] = c.tables.semaphore(c.reader.handle());
            wait_stages[i][j] = c.reader.u32();
            ok = wait_semaphores[i][j] != VK_NULL_HANDLE;
        }
        s.waitSemaphoreCount = wait_count;
        s.pWaitSemaphores = wait_count ? wait_semaphores[i].data() : nullptr;
        s.pWaitDstStageMask = wait_count ? wait_stages[i].data() : nullptr;

        const uint32_t cb_count = c.reader.u32();
        if (ok && (cb_count > remoting::kMaxArrayElements ||
                   !count_fits(c.reader, cb_count, 8))) ok = false;
        if (ok) command_buffers[i].resize(cb_count);
        for (uint32_t j = 0; j < cb_count && ok; ++j) {
            command_buffers[i][j] = c.tables.command_buffer(c.reader.handle());
            ok = command_buffers[i][j] != VK_NULL_HANDLE;
        }
        s.commandBufferCount = cb_count;
        s.pCommandBuffers = cb_count ? command_buffers[i].data() : nullptr;

        const uint32_t signal_count = c.reader.u32();
        if (ok && (signal_count > remoting::kMaxArrayElements ||
                   !count_fits(c.reader, signal_count, 8))) ok = false;
        if (ok) signal_semaphores[i].resize(signal_count);
        for (uint32_t j = 0; j < signal_count && ok; ++j) {
            signal_semaphores[i][j] = c.tables.semaphore(c.reader.handle());
            ok = signal_semaphores[i][j] != VK_NULL_HANDLE;
        }
        s.signalSemaphoreCount = signal_count;
        s.pSignalSemaphores = signal_count ? signal_semaphores[i].data() : nullptr;

        ok = ok && c.reader.ok();
    }

    ok = ok && c.reader.remaining() == 0;
    if (!ok) {
        c.reply_status(Status::DecodeError);
        return;
    }

    const VkResult result = vkQueueSubmit(queue, submit_count, submits.data(), fence);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(c.oneway_errors);
    c.oneway_errors = 0;
    c.reply();
}

void handle_QueueBindSparse(Session& c) {
    VkQueue queue=c.tables.queues.get(c.reader.handle());VkFence fence=c.tables.fence(c.reader.handle());uint32_t n=c.reader.u32();
    if(n>remoting::kMaxArrayElements||!count_fits(c.reader,n,16)){c.reply_status(Status::DecodeError);return;}
    std::vector<VkBindSparseInfo> infos(n);std::vector<std::vector<VkSemaphore>> waits(n),signals(n);std::vector<std::vector<VkSparseBufferMemoryBindInfo>> buffers(n);std::vector<std::vector<std::vector<VkSparseMemoryBind>>> buffer_binds(n);std::vector<std::vector<VkSparseImageOpaqueMemoryBindInfo>> opaques(n);std::vector<std::vector<std::vector<VkSparseMemoryBind>>> opaque_binds(n);std::vector<std::vector<VkSparseImageMemoryBindInfo>> images(n);std::vector<std::vector<std::vector<VkSparseImageMemoryBind>>> image_binds(n);bool ok=queue!=VK_NULL_HANDLE;
    Arena arena;for(uint32_t i=0;i<n&&ok;++i){auto&x=infos[i];x.sType=VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;ok=remoting::read_pnext_chain(c.reader,arena,c.tables,remoting::PNextContext::Submit,&x.pNext);uint32_t wn=c.reader.u32();if(wn>remoting::kMaxArrayElements||!count_fits(c.reader,wn,8)){ok=false;break;}waits[i].resize(wn);for(auto&v:waits[i])v=c.tables.semaphore(c.reader.handle());x.waitSemaphoreCount=wn;x.pWaitSemaphores=waits[i].data();uint32_t bn=c.reader.u32();if(bn>remoting::kMaxArrayElements||!count_fits(c.reader,bn,12)){ok=false;break;}buffers[i].resize(bn);buffer_binds[i].resize(bn);for(uint32_t j=0;j<bn;++j){auto&bi=buffers[i][j];bi.buffer=c.tables.buffer(c.reader.handle());uint32_t kcount=c.reader.u32();if(kcount>remoting::kMaxArrayElements||!count_fits(c.reader,kcount,36)){ok=false;break;}buffer_binds[i][j].resize(kcount);for(auto&b:buffer_binds[i][j]){b.resourceOffset=c.reader.u64();b.size=c.reader.u64();b.memory=c.tables.memory(c.reader.handle());b.memoryOffset=c.reader.u64();b.flags=c.reader.u32();}bi.bindCount=kcount;bi.pBinds=buffer_binds[i][j].data();}x.bufferBindCount=bn;x.pBufferBinds=buffers[i].data();uint32_t on=c.reader.u32();if(on>remoting::kMaxArrayElements||!count_fits(c.reader,on,12)){ok=false;break;}opaques[i].resize(on);opaque_binds[i].resize(on);for(uint32_t j=0;j<on;++j){auto&bi=opaques[i][j];bi.image=c.tables.image(c.reader.handle());uint32_t kcount=c.reader.u32();if(kcount>remoting::kMaxArrayElements||!count_fits(c.reader,kcount,36)){ok=false;break;}opaque_binds[i][j].resize(kcount);for(auto&b:opaque_binds[i][j]){b.resourceOffset=c.reader.u64();b.size=c.reader.u64();b.memory=c.tables.memory(c.reader.handle());b.memoryOffset=c.reader.u64();b.flags=c.reader.u32();}bi.bindCount=kcount;bi.pBinds=opaque_binds[i][j].data();}x.imageOpaqueBindCount=on;x.pImageOpaqueBinds=opaques[i].data();uint32_t in=c.reader.u32();if(in>remoting::kMaxArrayElements||!count_fits(c.reader,in,12)){ok=false;break;}images[i].resize(in);image_binds[i].resize(in);for(uint32_t j=0;j<in;++j){auto&bi=images[i][j];bi.image=c.tables.image(c.reader.handle());uint32_t kcount=c.reader.u32();if(kcount>remoting::kMaxArrayElements||!count_fits(c.reader,kcount,56)){ok=false;break;}image_binds[i][j].resize(kcount);for(auto&b:image_binds[i][j]){b.subresource.aspectMask=c.reader.u32();b.subresource.mipLevel=c.reader.u32();b.subresource.arrayLayer=c.reader.u32();b.offset.x=c.reader.i32();b.offset.y=c.reader.i32();b.offset.z=c.reader.i32();b.extent.width=c.reader.u32();b.extent.height=c.reader.u32();b.extent.depth=c.reader.u32();b.memory=c.tables.memory(c.reader.handle());b.memoryOffset=c.reader.u64();b.flags=c.reader.u32();}bi.bindCount=kcount;bi.pBinds=image_binds[i][j].data();}x.imageBindCount=in;x.pImageBinds=images[i].data();uint32_t sn=c.reader.u32();if(sn>remoting::kMaxArrayElements||!count_fits(c.reader,sn,8)){ok=false;break;}signals[i].resize(sn);for(auto&v:signals[i])v=c.tables.semaphore(c.reader.handle());x.signalSemaphoreCount=sn;x.pSignalSemaphores=signals[i].data();ok=ok&&c.reader.ok();}
    if(!ok){c.reply_status(Status::DecodeError);return;}VkResult result=vkQueueBindSparse(queue,n,infos.data(),fence);c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.i32(result);c.writer.u32(c.oneway_errors);c.oneway_errors=0;c.reply();
}

void handle_QueueWaitIdle(Session& c) {
    const uint64_t queue_id = c.reader.handle();
    VkQueue queue = c.tables.queues.get(queue_id);
    if (!c.reader.ok() || queue == VK_NULL_HANDLE) {
        c.reply_status(Status::DecodeError);
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
        c.reply_status(Status::DecodeError);
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
        c.reply_status(Status::DecodeError);
        return;
    }
    VkFence fence = VK_NULL_HANDLE;
    const VkResult result = vkCreateFence(device, &info, nullptr, &fence);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.fences.add(fence, device) : 0);
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
    if (count == 0 || count > remoting::kMaxArrayElements ||
        !count_fits(c.reader, count, 8)) {
        c.reply_status(Status::DecodeError);
        return;
    }
    std::vector<VkFence> fences(count);
    bool ok = device != VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        fences[i] = c.tables.fence(c.reader.handle());
        ok = ok && fences[i] != VK_NULL_HANDLE;
    }
    if (!ok || !c.reader.ok() || c.reader.remaining() != 0) {
        c.reply_status(Status::DecodeError);
        return;
    }
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(vkResetFences(device, count, fences.data()));
    c.reply();
}

void handle_GetFenceStatus(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t fence_id = c.reader.handle();
    VkFence fence = c.tables.fence(fence_id);
    if (!c.reader.ok() || c.reader.remaining() != 0 ||
        device == VK_NULL_HANDLE || fence == VK_NULL_HANDLE) {
        c.reply_status(Status::DecodeError);
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
    if (count == 0 || count > remoting::kMaxArrayElements ||
        !count_fits(c.reader, count, 8)) {
        c.reply_status(Status::DecodeError);
        return;
    }
    std::vector<VkFence> fences(count);
    bool ok = device != VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        fences[i] = c.tables.fence(c.reader.handle());
        ok = ok && fences[i] != VK_NULL_HANDLE;
    }
    const uint64_t timeout = c.reader.u64();
    if (!ok || !c.reader.ok() || c.reader.remaining() != 0) {
        c.reply_status(Status::DecodeError);
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
        c.reply_status(Status::DecodeError);
        return;
    }
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult result = vkCreateSemaphore(device, &info, nullptr, &semaphore);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.semaphores.add(semaphore, device) : 0);
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
            c.reply_status(Status::DecodeError);                                           \
            return;                                                                         \
        }                                                                                   \
        VkType obj = VK_NULL_HANDLE;                                                        \
        const VkResult result = VkCreateFn(device, &info, nullptr, &obj);                   \
        c.writer.u32(static_cast<uint32_t>(Status::Ok));                                    \
        c.writer.i32(result);                                                               \
        c.writer.handle(result == VK_SUCCESS ? c.tables.table.add(obj, device) : 0);                \
        c.reply();                                                                          \
    }

#define SERVER_DESTROY(HandleName, VkType, VkDestroyFn, table)               \
    void handle_Destroy##HandleName(Session& c) {                                \
        const uint64_t device_id = c.reader.handle();                       \
        VkDevice device = c.tables.devices.get(device_id);                  \
        const uint64_t id = c.reader.handle();                              \
        if (!c.reader.ok() || device == VK_NULL_HANDLE ||                  \
            !c.tables.table.owned_by(id, device)) {                         \
            mark_oneway_error(c);                                           \
            return;                                                         \
        }                                                                   \
        VkType obj = c.tables.table.take(id);                               \
        if (obj == VK_NULL_HANDLE) {                                        \
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

SERVER_CREATE(BufferView, VkBufferView, VkBufferViewCreateInfo,
              remoting::read_BufferViewCreateInfo, vkCreateBufferView, buffer_views)
SERVER_DESTROY(BufferView, VkBufferView, vkDestroyBufferView, buffer_views)

SERVER_CREATE(Event, VkEvent, VkEventCreateInfo, remoting::read_EventCreateInfo,
              vkCreateEvent, events)
SERVER_DESTROY(Event, VkEvent, vkDestroyEvent, events)

SERVER_CREATE(QueryPool, VkQueryPool, VkQueryPoolCreateInfo,
              remoting::read_QueryPoolCreateInfo, vkCreateQueryPool, query_pools)
SERVER_DESTROY(QueryPool, VkQueryPool, vkDestroyQueryPool, query_pools)

SERVER_CREATE(Sampler, VkSampler, VkSamplerCreateInfo, remoting::read_SamplerCreateInfo,
              vkCreateSampler, samplers)
SERVER_DESTROY(Sampler, VkSampler, vkDestroySampler, samplers)

void handle_CreateShaderModule(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkShaderModuleCreateInfo info{};
    // Decode the complete request even when the handle is invalid so malformed
    // shader payloads are classified deterministically and never reach Vulkan.
    if (!remoting::read_ShaderModuleCreateInfo(c.reader, arena, c.tables, &info) ||
        !c.reader.ok() || c.reader.remaining() != 0 || device == VK_NULL_HANDLE) {
        c.reply_status(Status::DecodeError);
        return;
    }
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(device, &info, nullptr, &module);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.shader_modules.add(module, device) : 0);
    c.reply();
}
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

void handle_CreateRenderPass2(Session& c) {
    VkDevice device=c.tables.devices.get(c.reader.handle()); Arena arena; VkRenderPassCreateInfo2 info{};
    if(!c.reader.ok()||device==VK_NULL_HANDLE||!remoting::read_RenderPassCreateInfo2(c.reader,arena,c.tables,&info)){c.reply_status(Status::DecodeError);return;}
    auto fn=reinterpret_cast<PFN_vkCreateRenderPass2>(vkGetDeviceProcAddr(device,"vkCreateRenderPass2"));
    if(!fn) fn=reinterpret_cast<PFN_vkCreateRenderPass2>(vkGetDeviceProcAddr(device,"vkCreateRenderPass2KHR"));
    if(!fn){c.reply_status(Status::UnsupportedCommand);return;}
    VkRenderPass pass=VK_NULL_HANDLE;VkResult result=fn(device,&info,nullptr,&pass);c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.i32(result);c.writer.handle(result==VK_SUCCESS?c.tables.render_passes.add(pass, device):0);c.reply();
}
SERVER_DESTROY(RenderPass, VkRenderPass, vkDestroyRenderPass, render_passes)

SERVER_CREATE(Framebuffer, VkFramebuffer, VkFramebufferCreateInfo,
              remoting::read_FramebufferCreateInfo, vkCreateFramebuffer, framebuffers)
SERVER_DESTROY(Framebuffer, VkFramebuffer, vkDestroyFramebuffer, framebuffers)

SERVER_DESTROY(DescriptorSetLayout, VkDescriptorSetLayout, vkDestroyDescriptorSetLayout,
               descriptor_set_layouts)

SERVER_CREATE(DescriptorPool, VkDescriptorPool, VkDescriptorPoolCreateInfo,
              remoting::read_DescriptorPoolCreateInfo, vkCreateDescriptorPool, descriptor_pools)

#undef SERVER_CREATE
#undef SERVER_DESTROY

void handle_DestroyDescriptorPool(Session& c) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    const uint64_t pool_id = c.reader.handle();
    VkDescriptorPool pool = c.tables.descriptor_pools.get(pool_id);
    if (!c.reader.ok() || c.reader.remaining() != 0 || device == VK_NULL_HANDLE ||
        pool == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    c.tables.descriptor_pools.take(pool_id);
    for (auto it = c.tables.descriptor_set_owners.begin();
         it != c.tables.descriptor_set_owners.end();) {
        if (it->second.pool == pool) {
            c.tables.descriptor_sets.take(it->first);
            it = c.tables.descriptor_set_owners.erase(it);
        } else {
            ++it;
        }
    }
    vkDestroyDescriptorPool(device, pool, nullptr);
}


void handle_CreateDescriptorSetLayout(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkDescriptorSetLayoutCreateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_DescriptorSetLayoutCreateInfo(c.reader, arena, c.tables, &info)) {
        c.reply_status(Status::DecodeError);
        return;
    }
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    const VkResult result = vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.descriptor_set_layouts.add(layout, device) : 0);
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
        c.reply_status(Status::DecodeError);
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

void handle_EventOperation(Session& c, int operation) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    VkEvent event = c.tables.event(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE || event == VK_NULL_HANDLE) {
        c.reply_status(Status::DecodeError); return;
    }
    VkResult result = operation == 0 ? vkGetEventStatus(device, event)
                      : operation == 1 ? vkSetEvent(device, event)
                                       : vkResetEvent(device, event);
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.i32(result); c.reply();
}
void handle_GetEventStatus(Session& c) { handle_EventOperation(c, 0); }
void handle_SetEvent(Session& c) { handle_EventOperation(c, 1); }
void handle_ResetEvent(Session& c) { handle_EventOperation(c, 2); }

void handle_GetQueryPoolResults(Session& c) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    VkQueryPool pool = c.tables.query_pool(c.reader.handle());
    const uint32_t first = c.reader.u32(), count = c.reader.u32();
    const uint64_t size64 = c.reader.u64(), stride = c.reader.u64();
    const VkQueryResultFlags flags = c.reader.u32();
    if (!c.reader.ok() || device == VK_NULL_HANDLE || pool == VK_NULL_HANDLE ||
        size64 > 64u * 1024u * 1024u || size64 > SIZE_MAX) {
        c.reply_status(Status::DecodeError); return;
    }
    std::vector<char> data(static_cast<size_t>(size64));
    VkResult result = vkGetQueryPoolResults(device, pool, first, count, data.size(),
                                            data.empty() ? nullptr : data.data(), stride, flags);
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.i32(result);
    c.writer.bytes(data.data(), data.size()); c.reply();
}

void handle_GetPipelineCacheData(Session& c) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    VkPipelineCache cache = c.tables.pipeline_cache(c.reader.handle());
    const uint64_t capacity64 = c.reader.u64(); const bool want_data = c.reader.u32() != 0;
    if (!c.reader.ok() || device == VK_NULL_HANDLE || cache == VK_NULL_HANDLE ||
        capacity64 > 64u * 1024u * 1024u || capacity64 > SIZE_MAX) {
        c.reply_status(Status::DecodeError); return;
    }
    size_t size = static_cast<size_t>(capacity64);
    std::vector<char> data(want_data ? size : 0);
    VkResult result = vkGetPipelineCacheData(device, cache, &size,
                                             want_data && !data.empty() ? data.data() : nullptr);
    if (data.size() > size) data.resize(size);
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.i32(result); c.writer.u64(size);
    c.writer.bytes(data.data(), data.size()); c.reply();
}

void handle_MergePipelineCaches(Session& c) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    VkPipelineCache dst = c.tables.pipeline_cache(c.reader.handle());
    const uint32_t count = c.reader.u32();
    if (count > remoting::kMaxArrayElements || !count_fits(c.reader, count, 8)) {
        c.reply_status(Status::DecodeError); return;
    }
    std::vector<VkPipelineCache> sources(count);
    bool valid = device != VK_NULL_HANDLE && dst != VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        sources[i] = c.tables.pipeline_cache(c.reader.handle()); valid &= sources[i] != VK_NULL_HANDLE;
    }
    if (!c.reader.ok() || !valid) { c.reply_status(Status::DecodeError); return; }
    VkResult result = vkMergePipelineCaches(device, dst, count, sources.data());
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.i32(result); c.reply();
}

void handle_GetRenderAreaGranularity(Session& c) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    VkRenderPass pass = c.tables.render_pass(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE || pass == VK_NULL_HANDLE) {
        c.reply_status(Status::DecodeError); return;
    }
    VkExtent2D value{}; vkGetRenderAreaGranularity(device, pass, &value);
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.u32(value.width);
    c.writer.u32(value.height); c.reply();
}

void handle_FreeDescriptorSets(Session& c) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    VkDescriptorPool pool = c.tables.descriptor_pool(c.reader.handle());
    const uint32_t count = c.reader.u32();
    if (count > remoting::kMaxArrayElements || !count_fits(c.reader, count, 8)) {
        c.reply_status(Status::DecodeError); return;
    }
    std::vector<uint64_t> ids(count);
    std::vector<VkDescriptorSet> sets(count);
    bool valid = device != VK_NULL_HANDLE && pool != VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        ids[i] = c.reader.handle();
        sets[i] = c.tables.descriptor_sets.get(ids[i]);
        const auto owner = c.tables.descriptor_set_owners.find(ids[i]);
        valid = valid && sets[i] != VK_NULL_HANDLE &&
                owner != c.tables.descriptor_set_owners.end() &&
                owner->second.device == device && owner->second.pool == pool;
    }
    if (!c.reader.ok() || c.reader.remaining() != 0 || !valid) {
        c.reply_status(Status::DecodeError); return;
    }
    const VkResult result = vkFreeDescriptorSets(device, pool, count, sets.data());
    if (result == VK_SUCCESS) {
        for (uint64_t id : ids) {
            c.tables.descriptor_sets.take(id);
            c.tables.descriptor_set_owners.erase(id);
        }
    }
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.i32(result); c.reply();
}

void handle_ResetDescriptorPool(Session& c) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    VkDescriptorPool pool = c.tables.descriptor_pool(c.reader.handle());
    const VkDescriptorPoolResetFlags flags = c.reader.u32();
    if (!c.reader.ok() || device == VK_NULL_HANDLE || pool == VK_NULL_HANDLE) {
        c.reply_status(Status::DecodeError); return;
    }
    if (c.reader.remaining() != 0) { c.reply_status(Status::DecodeError); return; }
    const VkResult result = vkResetDescriptorPool(device, pool, flags);
    if (result == VK_SUCCESS) {
        for (auto it = c.tables.descriptor_set_owners.begin();
             it != c.tables.descriptor_set_owners.end();) {
            if (it->second.pool == pool) {
                c.tables.descriptor_sets.take(it->first);
                it = c.tables.descriptor_set_owners.erase(it);
            } else {
                ++it;
            }
        }
    }
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.i32(result); c.reply();
}

void handle_CreateGraphicsPipelines(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t cache_id = c.reader.handle();
    VkPipelineCache cache = c.tables.pipeline_cache(cache_id);
    const uint32_t count = c.reader.u32();
    // Each pipeline's read_GraphicsPipelineCreateInfo starts with a
    // read_raw() call, whose bytes() consumes at least a 4-byte length
    // prefix even on immediate failure.
    if (count > remoting::kMaxArrayElements || !count_fits(c.reader, count, 4)) {
        c.reply_status(Status::DecodeError);
        return;
    }

    Arena arena;
    std::vector<VkGraphicsPipelineCreateInfo> infos(count);
    bool decoded = c.reader.ok();
    for (uint32_t i = 0; decoded && i < count; ++i) {
        decoded = remoting::read_GraphicsPipelineCreateInfo(c.reader, arena, c.tables, &infos[i]);
    }
    const bool handles_valid = device != VK_NULL_HANDLE &&
                               (!cache_id || cache != VK_NULL_HANDLE);
    if (!decoded || !handles_valid) {
        c.reply_status(Status::DecodeError);
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
        c.writer.handle(pipelines[i] != VK_NULL_HANDLE ? c.tables.pipelines.add(pipelines[i], device) : 0);
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
        c.writer.handle(pipelines[i] != VK_NULL_HANDLE ? c.tables.pipelines.add(pipelines[i], device) : 0);
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
        c.reply_status(Status::DecodeError);
        return;
    }
    std::vector<VkDescriptorSet> sets(info.descriptorSetCount);
    const VkResult result = vkAllocateDescriptorSets(device, &info, sets.data());
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(info.descriptorSetCount);
    for (uint32_t i = 0; i < info.descriptorSetCount; ++i) {
        uint64_t id = 0;
        if (result == VK_SUCCESS) {
            id = c.tables.descriptor_sets.add(sets[i], device);
            c.tables.descriptor_set_owners[id] = {device, info.descriptorPool};
        }
        c.writer.handle(id);
    }
    c.reply();
}

void handle_CreateDescriptorUpdateTemplate(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());Arena arena;VkDescriptorUpdateTemplateCreateInfo info{};if(!d||!remoting::read_DescriptorUpdateTemplateCreateInfo(c.reader,arena,c.tables,&info)){c.reply_status(Status::DecodeError);return;}auto fn=reinterpret_cast<PFN_vkCreateDescriptorUpdateTemplate>(vkGetDeviceProcAddr(d,"vkCreateDescriptorUpdateTemplate"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}VkDescriptorUpdateTemplate object=VK_NULL_HANDLE;VkResult result=fn(d,&info,nullptr,&object);uint64_t id=result==VK_SUCCESS?c.tables.descriptor_update_templates.add(object, d):0;if(id){c.tables.descriptor_update_template_owners[id]=d;c.tables.descriptor_update_template_entries[id]=std::vector<VkDescriptorUpdateTemplateEntry>(info.pDescriptorUpdateEntries,info.pDescriptorUpdateEntries+info.descriptorUpdateEntryCount);}c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.i32(result);c.writer.handle(id);c.reply();}
void handle_DestroyDescriptorUpdateTemplate(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());uint64_t id=c.reader.handle();auto owner=c.tables.descriptor_update_template_owners.find(id);if(!c.reader.ok()||!d||owner==c.tables.descriptor_update_template_owners.end()||owner->second!=d){mark_oneway_error(c);return;}auto object=c.tables.descriptor_update_templates.take(id);auto fn=reinterpret_cast<PFN_vkDestroyDescriptorUpdateTemplate>(vkGetDeviceProcAddr(d,"vkDestroyDescriptorUpdateTemplate"));if(!fn||!object){mark_oneway_error(c);return;}fn(d,object,nullptr);c.tables.descriptor_update_template_owners.erase(id);c.tables.descriptor_update_template_entries.erase(id);}
void handle_UpdateDescriptorSetWithTemplate(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkDescriptorSet set=c.tables.descriptor_set(c.reader.handle());uint64_t id=c.reader.handle();auto object=c.tables.descriptor_update_templates.get(id);auto meta=c.tables.descriptor_update_template_entries.find(id);uint32_t n=c.reader.u32();if(!d||!set||!object||meta==c.tables.descriptor_update_template_entries.end()||n!=meta->second.size()){mark_oneway_error(c);return;}size_t size=0;for(const auto&e:meta->second){if(e.descriptorCount>remoting::kMaxArrayElements){mark_oneway_error(c);return;}size_t elem=(e.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||e.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)?sizeof(VkBufferView):(e.descriptorType<=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT&&e.descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&e.descriptorType!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER&&e.descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC&&e.descriptorType!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)?sizeof(VkDescriptorImageInfo):sizeof(VkDescriptorBufferInfo);if(e.descriptorCount&&(e.offset>SIZE_MAX-(e.descriptorCount-1)*e.stride||e.offset+(e.descriptorCount-1)*e.stride>SIZE_MAX-elem)){mark_oneway_error(c);return;}size=(std::max)(size,e.offset+(e.descriptorCount?e.descriptorCount-1:0)*e.stride+(e.descriptorCount?elem:0));}if(size>64u*1024u*1024u){mark_oneway_error(c);return;}std::vector<char>data(size);for(const auto&e:meta->second){uint32_t count=c.reader.u32();auto type=static_cast<VkDescriptorType>(c.reader.i32());if(count!=e.descriptorCount||type!=e.descriptorType){mark_oneway_error(c);return;}for(uint32_t i=0;i<count;++i){char*p=data.data()+e.offset+i*e.stride;if(type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER){*reinterpret_cast<VkBufferView*>(p)=c.tables.buffer_view(c.reader.handle());}else if(type==VK_DESCRIPTOR_TYPE_SAMPLER||type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE||type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE||type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT){auto*x=reinterpret_cast<VkDescriptorImageInfo*>(p);x->sampler=c.tables.sampler(c.reader.handle());x->imageView=c.tables.image_view(c.reader.handle());x->imageLayout=static_cast<VkImageLayout>(c.reader.i32());}else{auto*x=reinterpret_cast<VkDescriptorBufferInfo*>(p);x->buffer=c.tables.buffer(c.reader.handle());x->offset=c.reader.u64();x->range=c.reader.u64();}}}if(!c.reader.ok()||c.reader.remaining()){mark_oneway_error(c);return;}auto fn=reinterpret_cast<PFN_vkUpdateDescriptorSetWithTemplate>(vkGetDeviceProcAddr(d,"vkUpdateDescriptorSetWithTemplate"));if(!fn){mark_oneway_error(c);return;}fn(d,set,object,data.data());}
void handle_GetDescriptorSetLayoutSupport(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());Arena arena;VkDescriptorSetLayoutCreateInfo info{};if(!d||!remoting::read_DescriptorSetLayoutCreateInfo(c.reader,arena,c.tables,&info)){c.reply_status(Status::DecodeError);return;}VkDescriptorSetLayoutSupport out{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};auto fn=reinterpret_cast<PFN_vkGetDescriptorSetLayoutSupport>(vkGetDeviceProcAddr(d,"vkGetDescriptorSetLayoutSupport"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}fn(d,&info,&out);c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.u32(out.supported);c.reply();}
void handle_CreateSamplerYcbcrConversion(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());Arena arena;VkSamplerYcbcrConversionCreateInfo info{};if(!d||!remoting::read_SamplerYcbcrConversionCreateInfo(c.reader,arena,c.tables,&info)){c.reply_status(Status::DecodeError);return;}auto fn=reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(vkGetDeviceProcAddr(d,"vkCreateSamplerYcbcrConversion"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}VkSamplerYcbcrConversion object=VK_NULL_HANDLE;VkResult result=fn(d,&info,nullptr,&object);uint64_t id=result==VK_SUCCESS?c.tables.sampler_ycbcr_conversions.add(object, d):0;if(id)c.tables.sampler_ycbcr_conversion_owners[id]=d;c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.i32(result);c.writer.handle(id);c.reply();}
void handle_DestroySamplerYcbcrConversion(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());uint64_t id=c.reader.handle();auto owner=c.tables.sampler_ycbcr_conversion_owners.find(id);if(!c.reader.ok()||!d||owner==c.tables.sampler_ycbcr_conversion_owners.end()||owner->second!=d){mark_oneway_error(c);return;}auto object=c.tables.sampler_ycbcr_conversions.take(id);auto fn=reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(vkGetDeviceProcAddr(d,"vkDestroySamplerYcbcrConversion"));if(!fn||!object){mark_oneway_error(c);return;}fn(d,object,nullptr);c.tables.sampler_ycbcr_conversion_owners.erase(id);}

void handle_UpdateDescriptorSets(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t write_count = c.reader.u32();
    // read_WriteDescriptorSet starts with read_raw(), whose bytes() call
    // consumes at least a 4-byte length prefix.
    if (write_count > remoting::kMaxArrayElements || !count_fits(c.reader, write_count, 4)) {
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
    if (ok && (copy_count > remoting::kMaxArrayElements ||
               !count_fits(c.reader, copy_count, 4))) ok = false;
    std::vector<VkCopyDescriptorSet> copies(ok ? copy_count : 0);
    for (uint32_t i = 0; ok && i < copy_count; ++i) {
        ok = remoting::read_CopyDescriptorSet(c.reader, arena, c.tables, &copies[i]);
    }
    if (!ok || !c.reader.ok() || c.reader.remaining() != 0) {
        mark_oneway_error(c);
        return;
    }
    vkUpdateDescriptorSets(device, write_count, writes.data(), copy_count, copies.data());
}


}  // namespace

REGISTER_HANDLER(vkCreateDevice, handle_CreateDevice);
REGISTER_HANDLER(vkDestroyDevice, handle_DestroyDevice);
REGISTER_HANDLER(vkGetDeviceQueue, handle_GetDeviceQueue);
REGISTER_HANDLER(vkGetDeviceQueue2, handle_GetDeviceQueue2);
REGISTER_HANDLER(vkGetDeviceGroupPeerMemoryFeatures, handle_GetDeviceGroupPeerMemoryFeatures);
REGISTER_HANDLER(vkQueueSubmit, handle_QueueSubmit);
REGISTER_HANDLER(vkQueueBindSparse, handle_QueueBindSparse);
REGISTER_HANDLER(vkQueueWaitIdle, handle_QueueWaitIdle);
REGISTER_HANDLER(vkDeviceWaitIdle, handle_DeviceWaitIdle);
REGISTER_HANDLER(vkCreateFence, handle_CreateFence);
REGISTER_HANDLER(vkDestroyFence, handle_DestroyFence);
REGISTER_HANDLER(vkResetFences, handle_ResetFences);
REGISTER_HANDLER(vkGetFenceStatus, handle_GetFenceStatus);
REGISTER_HANDLER(vkWaitForFences, handle_WaitForFences);
REGISTER_HANDLER(vkCreateSemaphore, handle_CreateSemaphore);
REGISTER_HANDLER(vkDestroySemaphore, handle_DestroySemaphore);
REGISTER_HANDLER(vkGetSemaphoreCounterValue, handle_GetSemaphoreCounterValue);
REGISTER_HANDLER(vkWaitSemaphores, handle_WaitSemaphores);
REGISTER_HANDLER(vkSignalSemaphore, handle_SignalSemaphore);
REGISTER_HANDLER(vkResetQueryPool, handle_ResetQueryPool);
REGISTER_HANDLER(vkCreateBuffer, handle_CreateBuffer);
REGISTER_HANDLER(vkDestroyBuffer, handle_DestroyBuffer);
REGISTER_HANDLER(vkCreateImage, handle_CreateImage);
REGISTER_HANDLER(vkDestroyImage, handle_DestroyImage);
REGISTER_HANDLER(vkCreateImageView, handle_CreateImageView);
REGISTER_HANDLER(vkDestroyImageView, handle_DestroyImageView);
REGISTER_HANDLER(vkCreateBufferView, handle_CreateBufferView);
REGISTER_HANDLER(vkDestroyBufferView, handle_DestroyBufferView);
REGISTER_HANDLER(vkCreateEvent, handle_CreateEvent);
REGISTER_HANDLER(vkDestroyEvent, handle_DestroyEvent);
REGISTER_HANDLER(vkGetEventStatus, handle_GetEventStatus);
REGISTER_HANDLER(vkSetEvent, handle_SetEvent);
REGISTER_HANDLER(vkResetEvent, handle_ResetEvent);
REGISTER_HANDLER(vkCreateQueryPool, handle_CreateQueryPool);
REGISTER_HANDLER(vkDestroyQueryPool, handle_DestroyQueryPool);
REGISTER_HANDLER(vkGetQueryPoolResults, handle_GetQueryPoolResults);
REGISTER_HANDLER(vkCreateSampler, handle_CreateSampler);
REGISTER_HANDLER(vkDestroySampler, handle_DestroySampler);
REGISTER_HANDLER(vkCreateShaderModule, handle_CreateShaderModule);
REGISTER_HANDLER(vkDestroyShaderModule, handle_DestroyShaderModule);
REGISTER_HANDLER(vkCreatePipelineCache, handle_CreatePipelineCache);
REGISTER_HANDLER(vkDestroyPipelineCache, handle_DestroyPipelineCache);
REGISTER_HANDLER(vkGetPipelineCacheData, handle_GetPipelineCacheData);
REGISTER_HANDLER(vkMergePipelineCaches, handle_MergePipelineCaches);
REGISTER_HANDLER(vkCreatePipelineLayout, handle_CreatePipelineLayout);
REGISTER_HANDLER(vkDestroyPipelineLayout, handle_DestroyPipelineLayout);
REGISTER_HANDLER(vkDestroyPipeline, handle_DestroyPipeline);
REGISTER_HANDLER(vkCreateRenderPass, handle_CreateRenderPass);
REGISTER_HANDLER(vkGetRenderAreaGranularity, handle_GetRenderAreaGranularity);
REGISTER_HANDLER(vkCreateRenderPass2, handle_CreateRenderPass2);
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
REGISTER_HANDLER(vkFreeDescriptorSets, handle_FreeDescriptorSets);
REGISTER_HANDLER(vkResetDescriptorPool, handle_ResetDescriptorPool);
REGISTER_HANDLER(vkUpdateDescriptorSets, handle_UpdateDescriptorSets);
REGISTER_HANDLER(vkCreateDescriptorUpdateTemplate, handle_CreateDescriptorUpdateTemplate);
REGISTER_HANDLER(vkDestroyDescriptorUpdateTemplate, handle_DestroyDescriptorUpdateTemplate);
REGISTER_HANDLER(vkUpdateDescriptorSetWithTemplate, handle_UpdateDescriptorSetWithTemplate);
REGISTER_HANDLER(vkGetDescriptorSetLayoutSupport, handle_GetDescriptorSetLayoutSupport);
REGISTER_HANDLER(vkCreateSamplerYcbcrConversion, handle_CreateSamplerYcbcrConversion);
REGISTER_HANDLER(vkDestroySamplerYcbcrConversion, handle_DestroySamplerYcbcrConversion);
