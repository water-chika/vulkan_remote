// Client-side device, queue, sync, pipeline, descriptor and resource
// creation entry points.
//
// Creation calls round-trip (the caller gets a handle back immediately and
// needs it); destructors and other calls nobody blocks on are sent with
// send_oneway (see remote_objects.hpp for why, and for the oneway error
// count reported back on the next call that does wait).

#include <stdio.h>
#include <string.h>

#include <mutex>
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
RemoteQueue* to_queue(VkQueue handle) { return reinterpret_cast<RemoteQueue*>(handle); }
RemoteCommandBuffer* to_cmd(VkCommandBuffer handle) {
    return reinterpret_cast<RemoteCommandBuffer*>(handle);
}

// Every reply to a round trip begins with a status word that Connection has
// already checked; call sites here only need the payload after it, so this
// walks the reader past it once and returns a Reader positioned there.
Reader payload_reader(const std::vector<char>& reply) {
    Reader r(reply.data(), reply.size());
    r.u32();
    return r;
}

void report_oneway_errors(uint32_t count) {
    if (count == 0) return;
    fprintf(stderr,
            "vulkan-remoting: %u fire-and-forget command(s) failed on the server since the last "
            "report; the failures happened during recording or a destroy call, not here\n",
            count);
}

// ---------------------------------------------------------------------------
// Device / queue
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice handle,
                                            const VkDeviceCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks*, VkDevice* pDevice) {
    auto* pd = reinterpret_cast<struct RemotePhysicalDevice*>(handle);
    Writer request;
    request.handle(pd->remote_id);
    write_DeviceCreateInfo(request, *pCreateInfo);

    std::vector<char> reply;
    if (!pd->instance->connection.round_trip(Opcode::vkCreateDevice, request, &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint64_t id = r.handle();
    if (!r.ok() || result != VK_SUCCESS) {
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    }

    auto* device = new RemoteDevice();
    set_loader_magic_value(device);
    device->instance = pd->instance;
    device->remote_id = id;
    *pDevice = reinterpret_cast<VkDevice>(device);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice handle, const VkAllocationCallbacks*) {
    if (handle == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    device->instance->connection.send_oneway(Opcode::vkDestroyDevice, request);
    for (auto* q : device->queues) delete q;
    for (auto* cb : device->command_buffers) delete cb;
    delete device;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice handle, uint32_t family, uint32_t index,
                                          VkQueue* pQueue) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.u32(family);
    request.u32(index);

    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetDeviceQueue, request, &reply)) {
        *pQueue = VK_NULL_HANDLE;
        return;
    }
    Reader r = payload_reader(reply);
    const uint64_t id = r.handle();
    if (!r.ok()) {
        *pQueue = VK_NULL_HANDLE;
        return;
    }
    auto* queue = new RemoteQueue();
    set_loader_magic_value(queue);
    queue->device = device;
    queue->remote_id = id;
    device->queues.push_back(queue);
    *pQueue = reinterpret_cast<VkQueue>(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue handle, uint32_t submitCount,
                                           const VkSubmitInfo* pSubmits, VkFence fence) {
    RemoteQueue* queue = to_queue(handle);
    RemoteDevice* device = queue->device;

    // The server must see whatever the application wrote through its
    // mappings before it can be part of what this submission renders with.
    device->flush_mapped();

    Writer request;
    request.handle(queue->remote_id);
    request.handle(id_from_handle(fence));
    request.u32(submitCount);
    for (uint32_t i = 0; i < submitCount; ++i) {
        const VkSubmitInfo& s = pSubmits[i];
        request.u32(s.waitSemaphoreCount);
        for (uint32_t j = 0; j < s.waitSemaphoreCount; ++j) {
            request.handle(id_from_handle(s.pWaitSemaphores[j]));
            request.u32(s.pWaitDstStageMask ? s.pWaitDstStageMask[j] : 0);
        }
        request.u32(s.commandBufferCount);
        for (uint32_t j = 0; j < s.commandBufferCount; ++j) {
            // Command buffers are dispatchable handles (real local structs),
            // unlike the non-dispatchable handles id_from_handle() decodes
            // directly from the handle value; look up their remote_id.
            request.handle(to_cmd(s.pCommandBuffers[j])->remote_id);
        }
        request.u32(s.signalSemaphoreCount);
        for (uint32_t j = 0; j < s.signalSemaphoreCount; ++j) {
            request.handle(id_from_handle(s.pSignalSemaphores[j]));
        }
    }

    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkQueueSubmit, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint32_t oneway_errors = r.u32();
    report_oneway_errors(oneway_errors);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue handle) {
    RemoteQueue* queue = to_queue(handle);
    Writer request;
    request.handle(queue->remote_id);
    std::vector<char> reply;
    if (!queue->device->instance->connection.round_trip(Opcode::vkQueueWaitIdle, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    return static_cast<VkResult>(r.i32());
}

VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice handle) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkDeviceWaitIdle, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint32_t oneway_errors = r.u32();
    report_oneway_errors(oneway_errors);
    return result;
}

// ---------------------------------------------------------------------------
// Sync
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice handle, const VkFenceCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks*, VkFence* pFence) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    write_FenceCreateInfo(request, *pCreateInfo);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkCreateFence, request, &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint64_t id = r.handle();
    if (result == VK_SUCCESS) *pFence = handle_from_id<VkFence>(id);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice handle, VkFence fence,
                                        const VkAllocationCallbacks*) {
    if (fence == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(fence));
    device->instance->connection.send_oneway(Opcode::vkDestroyFence, request);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice handle, uint32_t count,
                                           const VkFence* pFences) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.u32(count);
    for (uint32_t i = 0; i < count; ++i) request.handle(id_from_handle(pFences[i]));
    device->instance->connection.send_oneway(Opcode::vkResetFences, request);
    // Nobody waits for this one, per the fire-and-forget convention; a failure
    // surfaces later via the oneway error count on the next submit/wait-idle.
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(VkDevice handle, VkFence fence) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(fence));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetFenceStatus, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    return static_cast<VkResult>(r.i32());
}

VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice handle, uint32_t count,
                                             const VkFence* pFences, VkBool32 waitAll,
                                             uint64_t timeout) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.u32(waitAll ? 1 : 0);
    request.u32(count);
    for (uint32_t i = 0; i < count; ++i) request.handle(id_from_handle(pFences[i]));
    request.u64(timeout);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkWaitForFences, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    return static_cast<VkResult>(r.i32());
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice handle,
                                               const VkSemaphoreCreateInfo* pCreateInfo,
                                               const VkAllocationCallbacks*,
                                               VkSemaphore* pSemaphore) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    write_SemaphoreCreateInfo(request, *pCreateInfo);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkCreateSemaphore, request, &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint64_t id = r.handle();
    if (result == VK_SUCCESS) *pSemaphore = handle_from_id<VkSemaphore>(id);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice handle, VkSemaphore semaphore,
                                            const VkAllocationCallbacks*) {
    if (semaphore == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(semaphore));
    device->instance->connection.send_oneway(Opcode::vkDestroySemaphore, request);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

#define SIMPLE_CREATE(FnName, VkType, InfoType, WriteFn, OP)                                  \
    VKAPI_ATTR VkResult VKAPI_CALL FnName(VkDevice handle, const InfoType* pCreateInfo,        \
                                          const VkAllocationCallbacks*, VkType* pOut) {        \
        RemoteDevice* device = to_device(handle);                                             \
        Writer request;                                                                       \
        request.handle(device->remote_id);                                                    \
        WriteFn(request, *pCreateInfo);                                                       \
        std::vector<char> reply;                                                              \
        if (!device->instance->connection.round_trip(Opcode::OP, request, &reply)) {          \
            return VK_ERROR_INITIALIZATION_FAILED;                                            \
        }                                                                                      \
        Reader r = payload_reader(reply);                                                     \
        const VkResult result = static_cast<VkResult>(r.i32());                               \
        const uint64_t id = r.handle();                                                       \
        if (result == VK_SUCCESS) *pOut = handle_from_id<VkType>(id);                         \
        return result;                                                                        \
    }

#define SIMPLE_DESTROY(FnName, VkType, OP)                                                 \
    VKAPI_ATTR void VKAPI_CALL FnName(VkDevice handle, VkType obj,                          \
                                      const VkAllocationCallbacks*) {                       \
        if (obj == VK_NULL_HANDLE) return;                                                 \
        RemoteDevice* device = to_device(handle);                                          \
        Writer request;                                                                    \
        request.handle(device->remote_id);                                                 \
        request.handle(id_from_handle(obj));                                               \
        device->instance->connection.send_oneway(Opcode::OP, request);                     \
    }

SIMPLE_CREATE(CreateBuffer, VkBuffer, VkBufferCreateInfo, write_BufferCreateInfo, vkCreateBuffer)
SIMPLE_DESTROY(DestroyBuffer, VkBuffer, vkDestroyBuffer)

SIMPLE_CREATE(CreateImage, VkImage, VkImageCreateInfo, write_ImageCreateInfo, vkCreateImage)
SIMPLE_DESTROY(DestroyImage, VkImage, vkDestroyImage)

SIMPLE_CREATE(CreateSampler, VkSampler, VkSamplerCreateInfo, write_SamplerCreateInfo,
              vkCreateSampler)
SIMPLE_DESTROY(DestroySampler, VkSampler, vkDestroySampler)

SIMPLE_CREATE(CreateShaderModule, VkShaderModule, VkShaderModuleCreateInfo,
              write_ShaderModuleCreateInfo, vkCreateShaderModule)
SIMPLE_DESTROY(DestroyShaderModule, VkShaderModule, vkDestroyShaderModule)

SIMPLE_CREATE(CreatePipelineCache, VkPipelineCache, VkPipelineCacheCreateInfo,
              write_PipelineCacheCreateInfo, vkCreatePipelineCache)
SIMPLE_DESTROY(DestroyPipelineCache, VkPipelineCache, vkDestroyPipelineCache)

SIMPLE_CREATE(CreatePipelineLayout, VkPipelineLayout, VkPipelineLayoutCreateInfo,
              write_PipelineLayoutCreateInfo, vkCreatePipelineLayout)
SIMPLE_DESTROY(DestroyPipelineLayout, VkPipelineLayout, vkDestroyPipelineLayout)

SIMPLE_DESTROY(DestroyPipeline, VkPipeline, vkDestroyPipeline)

SIMPLE_CREATE(CreateRenderPass, VkRenderPass, VkRenderPassCreateInfo, write_RenderPassCreateInfo,
              vkCreateRenderPass)
SIMPLE_DESTROY(DestroyRenderPass, VkRenderPass, vkDestroyRenderPass)

SIMPLE_DESTROY(DestroyDescriptorSetLayout, VkDescriptorSetLayout, vkDestroyDescriptorSetLayout)

SIMPLE_CREATE(CreateDescriptorPool, VkDescriptorPool, VkDescriptorPoolCreateInfo,
              write_DescriptorPoolCreateInfo, vkCreateDescriptorPool)
SIMPLE_DESTROY(DestroyDescriptorPool, VkDescriptorPool, vkDestroyDescriptorPool)

SIMPLE_CREATE(CreateCommandPool, VkCommandPool, VkCommandPoolCreateInfo,
              write_CommandPoolCreateInfo, vkCreateCommandPool)
SIMPLE_DESTROY(DestroyCommandPool, VkCommandPool, vkDestroyCommandPool)

#undef SIMPLE_CREATE
#undef SIMPLE_DESTROY

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice handle,
                                               const VkImageViewCreateInfo* pCreateInfo,
                                               const VkAllocationCallbacks*, VkImageView* pView) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    write_ImageViewCreateInfo(request, *pCreateInfo);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkCreateImageView, request, &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint64_t id = r.handle();
    if (result == VK_SUCCESS) *pView = handle_from_id<VkImageView>(id);
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice handle, VkImageView view,
                                            const VkAllocationCallbacks*) {
    if (view == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(view));
    device->instance->connection.send_oneway(Opcode::vkDestroyImageView, request);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(VkDevice handle,
                                                 const VkFramebufferCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks*,
                                                 VkFramebuffer* pFramebuffer) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    write_FramebufferCreateInfo(request, *pCreateInfo);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkCreateFramebuffer, request, &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint64_t id = r.handle();
    if (result == VK_SUCCESS) *pFramebuffer = handle_from_id<VkFramebuffer>(id);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(VkDevice handle, VkFramebuffer framebuffer,
                                              const VkAllocationCallbacks*) {
    if (framebuffer == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(framebuffer));
    device->instance->connection.send_oneway(Opcode::vkDestroyFramebuffer, request);
}

VKAPI_ATTR void VKAPI_CALL GetImageSubresourceLayout(VkDevice handle, VkImage image,
                                                     const VkImageSubresource* pSubresource,
                                                     VkSubresourceLayout* pLayout) {
    memset(pLayout, 0, sizeof(*pLayout));
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(image));
    request.u32(pSubresource->aspectMask);
    request.u32(pSubresource->mipLevel);
    request.u32(pSubresource->arrayLayer);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetImageSubresourceLayout, request,
                                                  &reply)) {
        return;
    }
    Reader r = payload_reader(reply);
    pLayout->offset = r.u64();
    pLayout->size = r.u64();
    pLayout->rowPitch = r.u64();
    pLayout->arrayPitch = r.u64();
    pLayout->depthPitch = r.u64();
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
    VkDevice handle, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks*,
    VkPipeline* pPipelines) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(pipelineCache));
    request.u32(createInfoCount);
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        write_GraphicsPipelineCreateInfo(request, pCreateInfos[i]);
    }

    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkCreateGraphicsPipelines, request,
                                                  &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && i < createInfoCount; ++i) {
        const uint64_t id = r.handle();
        pPipelines[i] = id ? handle_from_id<VkPipeline>(id) : VK_NULL_HANDLE;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(
    VkDevice handle, const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*, VkDescriptorSetLayout* pLayout) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    write_DescriptorSetLayoutCreateInfo(request, *pCreateInfo);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkCreateDescriptorSetLayout, request,
                                                  &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint64_t id = r.handle();
    if (result == VK_SUCCESS) *pLayout = handle_from_id<VkDescriptorSetLayout>(id);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice handle,
                                                      const VkDescriptorSetAllocateInfo* pInfo,
                                                      VkDescriptorSet* pSets) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    write_DescriptorSetAllocateInfo(request, *pInfo);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkAllocateDescriptorSets, request,
                                                  &reply)) {
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && i < pInfo->descriptorSetCount; ++i) {
        pSets[i] = handle_from_id<VkDescriptorSet>(r.handle());
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice handle, uint32_t writeCount,
                                                const VkWriteDescriptorSet* pWrites,
                                                uint32_t copyCount,
                                                const VkCopyDescriptorSet* pCopies) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.u32(writeCount);
    for (uint32_t i = 0; i < writeCount; ++i) write_WriteDescriptorSet(request, pWrites[i]);
    request.u32(copyCount);
    for (uint32_t i = 0; i < copyCount; ++i) write_CopyDescriptorSet(request, pCopies[i]);
    device->instance->connection.send_oneway(Opcode::vkUpdateDescriptorSets, request);
}

}  // namespace

const DeviceEntry* get_device_core_entries(size_t* count) {
    static const DeviceEntry kEntries[] = {
#define D(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name)}
        D(CreateDevice),
        D(DestroyDevice),
        D(GetDeviceQueue),
        D(QueueSubmit),
        D(QueueWaitIdle),
        D(DeviceWaitIdle),
        D(CreateFence),
        D(DestroyFence),
        D(ResetFences),
        D(GetFenceStatus),
        D(WaitForFences),
        D(CreateSemaphore),
        D(DestroySemaphore),
        D(CreateBuffer),
        D(DestroyBuffer),
        D(CreateImage),
        D(DestroyImage),
        D(CreateImageView),
        D(DestroyImageView),
        D(CreateSampler),
        D(DestroySampler),
        D(GetImageSubresourceLayout),
        D(CreateShaderModule),
        D(DestroyShaderModule),
        D(CreatePipelineCache),
        D(DestroyPipelineCache),
        D(CreatePipelineLayout),
        D(DestroyPipelineLayout),
        D(CreateGraphicsPipelines),
        D(DestroyPipeline),
        D(CreateRenderPass),
        D(DestroyRenderPass),
        D(CreateFramebuffer),
        D(DestroyFramebuffer),
        D(CreateDescriptorSetLayout),
        D(DestroyDescriptorSetLayout),
        D(CreateDescriptorPool),
        D(DestroyDescriptorPool),
        D(AllocateDescriptorSets),
        D(UpdateDescriptorSets),
        D(CreateCommandPool),
        D(DestroyCommandPool),
#undef D
    };
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

}  // namespace remoting
