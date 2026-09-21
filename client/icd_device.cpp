// Device-level entry points for the client-side ICD.
//
// Everything here either round-trips (a creation call, because the caller
// gets a handle back immediately and needs it) or is sent with send_oneway
// (recording calls and destructors nobody blocks on). See remote_objects.hpp
// for why the two kinds of dispatchable handle behave the way they do, and
// wire.hpp / marshal.hpp for the framing and struct marshalling reused here.
//
// Mapped memory is not coherent in the strict Vulkan sense: a write through a
// mapped pointer becomes visible on the server only when this file uploads
// the shadow buffer, which happens at vkUnmapMemory, vkFlushMappedMemoryRanges
// and immediately before every vkQueueSubmit. A program that expects a write
// to be visible to the GPU without one of those would be wrong on real
// coherent memory too, in spirit if not in the letter of the spec; this
// project just makes the cost of that visibility explicit instead of hiding
// it in cache flush instructions.

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

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
// Memory
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice handle,
                                              const VkMemoryAllocateInfo* pAllocateInfo,
                                              const VkAllocationCallbacks*,
                                              VkDeviceMemory* pMemory) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    write_MemoryAllocateInfo(request, *pAllocateInfo);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkAllocateMemory, request, &reply)) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint64_t id = r.handle();
    if (result == VK_SUCCESS) {
        *pMemory = handle_from_id<VkDeviceMemory>(id);
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        device->memory_sizes[id] = pAllocateInfo->allocationSize;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice handle, VkDeviceMemory memory,
                                      const VkAllocationCallbacks*) {
    if (memory == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    {
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        device->memory_sizes.erase(id_from_handle(memory));
    }
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(memory));
    device->instance->connection.send_oneway(Opcode::vkFreeMemory, request);
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice handle, VkDeviceMemory memory,
                                         VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags,
                                         void** ppData) {
    RemoteDevice* device = to_device(handle);
    const uint64_t memory_id = id_from_handle(memory);

    VkDeviceSize real_size = size;
    if (size == VK_WHOLE_SIZE) {
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        auto it = device->memory_sizes.find(memory_id);
        if (it == device->memory_sizes.end()) return VK_ERROR_MEMORY_MAP_FAILED;
        real_size = it->second - offset;
    }

    void* shadow = std::malloc(static_cast<size_t>(real_size));
    if (shadow == nullptr) return VK_ERROR_OUT_OF_HOST_MEMORY;

    {
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        device->mapped.push_back(MappedRange{memory_id, offset, real_size, shadow});
    }

    // Download what the server actually holds now, so a caller that maps
    // memory to read back a render sees real pixels rather than whatever was
    // in freshly malloc'd host memory.
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = memory;
    range.offset = offset;
    range.size = real_size;
    device->download_mapped(&range, 1);

    *ppData = shadow;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice handle, VkDeviceMemory memory) {
    RemoteDevice* device = to_device(handle);
    // Upload every mapped range rather than tracking exactly which bytes the
    // application touched; flushing more than strictly necessary is never
    // wrong, only more bytes on the wire.
    device->flush_mapped();

    std::lock_guard<std::mutex> lock(device->mapped_mutex);
    const uint64_t memory_id = id_from_handle(memory);
    for (auto it = device->mapped.begin(); it != device->mapped.end(); ++it) {
        if (it->memory_id == memory_id) {
            std::free(it->shadow);
            device->mapped.erase(it);
            break;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL FlushMappedMemoryRanges(VkDevice handle, uint32_t,
                                                       const VkMappedMemoryRange*) {
    to_device(handle)->flush_mapped();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL InvalidateMappedMemoryRanges(VkDevice handle, uint32_t count,
                                                            const VkMappedMemoryRange* pRanges) {
    RemoteDevice* device = to_device(handle);
    return device->download_mapped(pRanges, count) ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice handle, VkBuffer buffer,
                                                VkDeviceMemory memory, VkDeviceSize offset) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(buffer));
    request.handle(id_from_handle(memory));
    request.u64(offset);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkBindBufferMemory, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    return static_cast<VkResult>(r.i32());
}

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(VkDevice handle, VkImage image,
                                               VkDeviceMemory memory, VkDeviceSize offset) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(image));
    request.handle(id_from_handle(memory));
    request.u64(offset);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkBindImageMemory, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    return static_cast<VkResult>(r.i32());
}

void read_memory_requirements(Reader& r, VkMemoryRequirements* out) {
    out->size = r.u64();
    out->alignment = r.u64();
    out->memoryTypeBits = r.u32();
}

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(VkDevice handle, VkBuffer buffer,
                                                       VkMemoryRequirements* pRequirements) {
    memset(pRequirements, 0, sizeof(*pRequirements));
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(buffer));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetBufferMemoryRequirements, request,
                                                  &reply)) {
        return;
    }
    Reader r = payload_reader(reply);
    read_memory_requirements(r, pRequirements);
}

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice handle, VkImage image,
                                                      VkMemoryRequirements* pRequirements) {
    memset(pRequirements, 0, sizeof(*pRequirements));
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(image));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetImageMemoryRequirements, request,
                                                  &reply)) {
        return;
    }
    Reader r = payload_reader(reply);
    read_memory_requirements(r, pRequirements);
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

// ---------------------------------------------------------------------------
// Command pools / buffers
// ---------------------------------------------------------------------------

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
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && i < pAllocateInfo->commandBufferCount; ++i) {
        const uint64_t id = r.handle();
        auto* cb = new RemoteCommandBuffer();
        set_loader_magic_value(cb);
        cb->device = device;
        cb->remote_id = id;
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

// ---------------------------------------------------------------------------
// Recording (all fire-and-forget)
// ---------------------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer handle,
                                              const VkRenderPassBeginInfo* pInfo,
                                              VkSubpassContents contents) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    write_RenderPassBeginInfo(request, *pInfo);
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
                                           VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.i32(static_cast<int32_t>(bindPoint));
    request.handle(id_from_handle(pipeline));
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBindPipeline, request);
}

VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer handle,
                                                 VkPipelineBindPoint bindPoint,
                                                 VkPipelineLayout layout, uint32_t firstSet,
                                                 uint32_t count, const VkDescriptorSet* pSets,
                                                 uint32_t dynamicOffsetCount,
                                                 const uint32_t* pDynamicOffsets) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.i32(static_cast<int32_t>(bindPoint));
    request.handle(id_from_handle(layout));
    request.u32(firstSet);
    request.u32(count);
    for (uint32_t i = 0; i < count; ++i) request.handle(id_from_handle(pSets[i]));
    request.u32(dynamicOffsetCount);
    for (uint32_t i = 0; i < dynamicOffsetCount; ++i) request.u32(pDynamicOffsets[i]);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdBindDescriptorSets, request);
}

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer handle, uint32_t firstBinding,
                                                uint32_t count, const VkBuffer* pBuffers,
                                                const VkDeviceSize* pOffsets) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.u32(firstBinding);
    request.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
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
    const VkMemoryBarrier* pMemoryBarriers, uint32_t bufferBarrierCount,
    const VkBufferMemoryBarrier* pBufferBarriers, uint32_t imageBarrierCount,
    const VkImageMemoryBarrier* pImageBarriers) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.u32(srcStageMask);
    request.u32(dstStageMask);
    request.u32(dependencyFlags);
    request.u32(memoryBarrierCount);
    for (uint32_t i = 0; i < memoryBarrierCount; ++i) write_MemoryBarrier(request, pMemoryBarriers[i]);
    request.u32(bufferBarrierCount);
    for (uint32_t i = 0; i < bufferBarrierCount; ++i)
        write_BufferMemoryBarrier(request, pBufferBarriers[i]);
    request.u32(imageBarrierCount);
    for (uint32_t i = 0; i < imageBarrierCount; ++i)
        write_ImageMemoryBarrier(request, pImageBarriers[i]);
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
                                                VkImageLayout dstLayout, uint32_t regionCount,
                                                const VkBufferImageCopy* pRegions) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(src));
    request.handle(id_from_handle(dst));
    request.i32(static_cast<int32_t>(dstLayout));
    request.u32(regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) write_BufferImageCopy(request, pRegions[i]);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdCopyBufferToImage, request);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer(VkCommandBuffer handle, VkImage src,
                                                VkImageLayout srcLayout, VkBuffer dst,
                                                uint32_t regionCount,
                                                const VkBufferImageCopy* pRegions) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(src));
    request.i32(static_cast<int32_t>(srcLayout));
    request.handle(id_from_handle(dst));
    request.u32(regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) write_BufferImageCopy(request, pRegions[i]);
    cb->device->instance->connection.send_oneway(Opcode::vkCmdCopyImageToBuffer, request);
}

VKAPI_ATTR void VKAPI_CALL CmdClearColorImage(VkCommandBuffer handle, VkImage image,
                                              VkImageLayout layout,
                                              const VkClearColorValue* pColor, uint32_t rangeCount,
                                              const VkImageSubresourceRange* pRanges) {
    RemoteCommandBuffer* cb = to_cmd(handle);
    Writer request;
    request.handle(cb->remote_id);
    request.handle(id_from_handle(image));
    request.i32(static_cast<int32_t>(layout));
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
}  // namespace remoting

// Registered from icd.cpp's entry table, which is why these stay linkage-
// visible to that translation unit despite living in an anonymous namespace
// here: the table below is built once, at static-init time, in this file and
// consulted by name from icd.cpp through get_device_entries().
namespace remoting {

const DeviceEntry* get_device_entries(size_t* count) {
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
        D(AllocateMemory),
        D(FreeMemory),
        D(MapMemory),
        D(UnmapMemory),
        D(FlushMappedMemoryRanges),
        D(InvalidateMappedMemoryRanges),
        D(BindBufferMemory),
        D(BindImageMemory),
        D(GetBufferMemoryRequirements),
        D(GetImageMemoryRequirements),
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
