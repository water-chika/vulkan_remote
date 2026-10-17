// Client-side device, queue, sync, pipeline, descriptor and resource
// creation entry points.
//
// Creation calls round-trip (the caller gets a handle back immediately and
// needs it); destructors and other calls nobody blocks on are sent with
// send_oneway (see remote_objects.hpp for why, and for the oneway error
// count reported back on the next call that does wait).

#include <stdio.h>
#include <string.h>

#include <cstdlib>
#include <mutex>
#include <type_traits>
#include <vector>

#include <vulkan/vk_icd.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include "entry_table.hpp"
#include "extension_contract.hpp"
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
                                            const VkAllocationCallbacks* pAllocator,
                                            VkDevice* pDevice) {
    if (handle == VK_NULL_HANDLE || pCreateInfo == nullptr || pDevice == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // Host callbacks cannot cross the process boundary; remote objects use the
    // server allocator and local dispatch wrappers use the ICD allocator.
    (void)pAllocator;
    if (pCreateInfo->enabledLayerCount != 0) return VK_ERROR_LAYER_NOT_PRESENT;
    if (pCreateInfo->pEnabledFeatures &&
        (pCreateInfo->pEnabledFeatures->sparseBinding ||
         pCreateInfo->pEnabledFeatures->sparseResidencyBuffer ||
         pCreateInfo->pEnabledFeatures->sparseResidencyImage2D ||
         pCreateInfo->pEnabledFeatures->sparseResidencyImage3D ||
         pCreateInfo->pEnabledFeatures->sparseResidency2Samples ||
         pCreateInfo->pEnabledFeatures->sparseResidency4Samples ||
         pCreateInfo->pEnabledFeatures->sparseResidency8Samples ||
         pCreateInfo->pEnabledFeatures->sparseResidency16Samples ||
         pCreateInfo->pEnabledFeatures->sparseResidencyAliased))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const auto extension_enabled = [&](const char* name) {
        if (pCreateInfo->ppEnabledExtensionNames == nullptr) return false;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            if (pCreateInfo->ppEnabledExtensionNames[i] != nullptr &&
                strcmp(pCreateInfo->ppEnabledExtensionNames[i], name) == 0) return true;
        return false;
    };
    for(auto* node=static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);node;node=node->pNext){
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES){const auto&s=*reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(node);if(s.storageBuffer16BitAccess||s.uniformAndStorageBuffer16BitAccess||s.storagePushConstant16||s.storageInputOutput16||s.variablePointersStorageBuffer||s.variablePointers||s.protectedMemory||s.shaderDrawParameters)return VK_ERROR_FEATURE_NOT_PRESENT;}
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES&&reinterpret_cast<const VkPhysicalDeviceProtectedMemoryFeatures*>(node)->protectedMemory)return VK_ERROR_FEATURE_NOT_PRESENT;
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES&&!remoting::vulkan12_requested_features_supported(*reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node)))return VK_ERROR_FEATURE_NOT_PRESENT;
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES){const auto&f=*reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(node);if(f.bufferDeviceAddress||f.bufferDeviceAddressCaptureReplay||f.bufferDeviceAddressMultiDevice)return VK_ERROR_FEATURE_NOT_PRESENT;}
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES){const auto&f=*reinterpret_cast<const VkPhysicalDeviceDescriptorIndexingFeatures*>(node);VkPhysicalDeviceDescriptorIndexingFeatures z{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};if(std::memcmp(reinterpret_cast<const char*>(&f)+offsetof(VkPhysicalDeviceDescriptorIndexingFeatures,shaderInputAttachmentArrayDynamicIndexing),reinterpret_cast<const char*>(&z)+offsetof(VkPhysicalDeviceDescriptorIndexingFeatures,shaderInputAttachmentArrayDynamicIndexing),sizeof(f)-offsetof(VkPhysicalDeviceDescriptorIndexingFeatures,shaderInputAttachmentArrayDynamicIndexing))!=0)return VK_ERROR_FEATURE_NOT_PRESENT;}
    }
    auto* pd = reinterpret_cast<struct RemotePhysicalDevice*>(handle);
    const bool properties2_enabled = pd->instance->enabled_extensions.count(
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) != 0;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* requested = pCreateInfo->ppEnabledExtensionNames
            ? pCreateInfo->ppEnabledExtensionNames[i] : nullptr;
        if (requested == nullptr ||
            (strcmp(requested, VK_KHR_SWAPCHAIN_EXTENSION_NAME) != 0 &&
             !remoting::is_required_device_extension(requested)) ||
            (remoting::is_required_device_extension(requested) &&
             pd->api_version < VK_API_VERSION_1_2 &&
             !remoting::device_extension_dependencies_satisfied(
                 requested, extension_enabled, properties2_enabled))) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    Writer request;
    request.handle(pd->remote_id);
    if (!write_DeviceCreateInfo(request, *pCreateInfo)) return VK_ERROR_FEATURE_NOT_PRESENT;

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
    device->api_version = pd->api_version;
    device->map_alignment = pd->map_alignment;
    device->memory_type_flags = pd->memory_type_flags;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        device->enabled_extensions.insert(pCreateInfo->ppEnabledExtensionNames[i]);
    }
    *pDevice = reinterpret_cast<VkDevice>(device);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice handle, const VkAllocationCallbacks*) {
    if (handle == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    device->instance->connection.send_oneway(Opcode::vkDestroyDevice, request);
    {
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        for (const MappedRange& mapping : device->mapped) std::free(mapping.allocation_base);
        device->mapped.clear();
    }
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

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice handle,const VkDeviceQueueInfo2* info,VkQueue* out){
    if(!info||!out){return;}RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);w.u32(info->flags);w.u32(info->queueFamilyIndex);w.u32(info->queueIndex);std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkGetDeviceQueue2,w,&reply)){*out=VK_NULL_HANDLE;return;}Reader r=payload_reader(reply);uint64_t id=r.handle();if(!r.ok()||!id){*out=VK_NULL_HANDLE;return;}auto*q=new RemoteQueue();set_loader_magic_value(q);q->device=d;q->remote_id=id;d->queues.push_back(q);*out=reinterpret_cast<VkQueue>(q);
}
VKAPI_ATTR void VKAPI_CALL GetDeviceGroupPeerMemoryFeatures(VkDevice handle,uint32_t heap,uint32_t local,uint32_t remote,VkPeerMemoryFeatureFlags* out){if(!out)return;RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);w.u32(heap);w.u32(local);w.u32(remote);std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkGetDeviceGroupPeerMemoryFeatures,w,&reply)){*out=0;return;}Reader r=payload_reader(reply);*out=r.u32();if(!r.ok())*out=0;}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue handle, uint32_t submitCount,
                                           const VkSubmitInfo* pSubmits, VkFence fence) {
    if (handle == VK_NULL_HANDLE || submitCount > kMaxArrayElements ||
        (submitCount != 0 && pSubmits == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < submitCount; ++i) {
        const VkSubmitInfo& s = pSubmits[i];
        if (s.sType != VK_STRUCTURE_TYPE_SUBMIT_INFO ||
            s.waitSemaphoreCount > kMaxArrayElements ||
            s.commandBufferCount > kMaxArrayElements ||
            s.signalSemaphoreCount > kMaxArrayElements ||
            (s.waitSemaphoreCount != 0 &&
             (s.pWaitSemaphores == nullptr || s.pWaitDstStageMask == nullptr)) ||
            (s.commandBufferCount != 0 && s.pCommandBuffers == nullptr) ||
            (s.signalSemaphoreCount != 0 && s.pSignalSemaphores == nullptr)) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (uint32_t j = 0; j < s.waitSemaphoreCount; ++j) {
            if (s.pWaitSemaphores[j] == VK_NULL_HANDLE)
                return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (uint32_t j = 0; j < s.commandBufferCount; ++j) {
            if (s.pCommandBuffers[j] == VK_NULL_HANDLE)
                return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (uint32_t j = 0; j < s.signalSemaphoreCount; ++j) {
            if (s.pSignalSemaphores[j] == VK_NULL_HANDLE)
                return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    RemoteQueue* queue = to_queue(handle);
    RemoteDevice* device = queue->device;

    // Host-coherent mappings become visible without an explicit Vulkan flush.
    // Non-coherent mappings move only through vkFlushMappedMemoryRanges.
    if (!device->flush_coherent_mapped()) return VK_ERROR_DEVICE_LOST;

    Writer request;
    request.handle(queue->remote_id);
    request.handle(id_from_handle(fence));
    request.u32(submitCount);
    for (uint32_t i = 0; i < submitCount; ++i) {
        const VkSubmitInfo& s = pSubmits[i];
        if (!write_pnext_chain(request, PNextContext::Submit, s.pNext))
            return VK_ERROR_FEATURE_NOT_PRESENT;
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
    if (!r.ok() || r.remaining() != 0) return VK_ERROR_DEVICE_LOST;
    report_oneway_errors(oneway_errors);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(VkQueue handle, uint32_t bindInfoCount,
                                               const VkBindSparseInfo* pBindInfo,
                                               VkFence fence) {
    RemoteQueue* queue = to_queue(handle); RemoteDevice* device = queue->device;
    if (bindInfoCount > kMaxArrayElements || (bindInfoCount && !pBindInfo))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!device->flush_coherent_mapped()) return VK_ERROR_DEVICE_LOST;
    Writer w; w.handle(queue->remote_id); w.handle(id_from_handle(fence)); w.u32(bindInfoCount);
    for (uint32_t i=0;i<bindInfoCount;++i) {
        const auto& x=pBindInfo[i];
        if(!write_pnext_chain(w,PNextContext::Submit,x.pNext))return VK_ERROR_FEATURE_NOT_PRESENT;
        w.u32(x.waitSemaphoreCount); for(uint32_t j=0;j<x.waitSemaphoreCount;++j)w.handle(id_from_handle(x.pWaitSemaphores[j]));
        w.u32(x.bufferBindCount); for(uint32_t j=0;j<x.bufferBindCount;++j){const auto& bi=x.pBufferBinds[j];w.handle(id_from_handle(bi.buffer));w.u32(bi.bindCount);for(uint32_t k=0;k<bi.bindCount;++k){const auto& b=bi.pBinds[k];w.u64(b.resourceOffset);w.u64(b.size);w.handle(id_from_handle(b.memory));w.u64(b.memoryOffset);w.u32(b.flags);}}
        w.u32(x.imageOpaqueBindCount); for(uint32_t j=0;j<x.imageOpaqueBindCount;++j){const auto& bi=x.pImageOpaqueBinds[j];w.handle(id_from_handle(bi.image));w.u32(bi.bindCount);for(uint32_t k=0;k<bi.bindCount;++k){const auto& b=bi.pBinds[k];w.u64(b.resourceOffset);w.u64(b.size);w.handle(id_from_handle(b.memory));w.u64(b.memoryOffset);w.u32(b.flags);}}
        w.u32(x.imageBindCount); for(uint32_t j=0;j<x.imageBindCount;++j){const auto& bi=x.pImageBinds[j];w.handle(id_from_handle(bi.image));w.u32(bi.bindCount);for(uint32_t k=0;k<bi.bindCount;++k){const auto& b=bi.pBinds[k];w.u32(b.subresource.aspectMask);w.u32(b.subresource.mipLevel);w.u32(b.subresource.arrayLayer);w.i32(b.offset.x);w.i32(b.offset.y);w.i32(b.offset.z);w.u32(b.extent.width);w.u32(b.extent.height);w.u32(b.extent.depth);w.handle(id_from_handle(b.memory));w.u64(b.memoryOffset);w.u32(b.flags);}}
        w.u32(x.signalSemaphoreCount); for(uint32_t j=0;j<x.signalSemaphoreCount;++j)w.handle(id_from_handle(x.pSignalSemaphores[j]));
    }
    std::vector<char> reply; if(!device->instance->connection.round_trip(Opcode::vkQueueBindSparse,w,&reply))return VK_ERROR_DEVICE_LOST;
    Reader r=payload_reader(reply);VkResult result=static_cast<VkResult>(r.i32());uint32_t errors=r.u32();report_oneway_errors(errors);return r.ok()?result:VK_ERROR_DEVICE_LOST;
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
    const VkResult result = static_cast<VkResult>(r.i32());
    if (!r.ok()) return VK_ERROR_DEVICE_LOST;
    return result;
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
    if (!r.ok()) return VK_ERROR_DEVICE_LOST;
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
    if (handle == VK_NULL_HANDLE || count == 0 || count > kMaxArrayElements ||
        pFences == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (pFences[i] == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
    }
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.u32(count);
    for (uint32_t i = 0; i < count; ++i) request.handle(id_from_handle(pFences[i]));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkResetFences, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    return r.ok() && r.remaining() == 0 ? result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(VkDevice handle, VkFence fence) {
    if (handle == VK_NULL_HANDLE || fence == VK_NULL_HANDLE)
        return VK_ERROR_INITIALIZATION_FAILED;
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(fence));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetFenceStatus, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    if (!r.ok()) return VK_ERROR_DEVICE_LOST;
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice handle, uint32_t count,
                                             const VkFence* pFences, VkBool32 waitAll,
                                             uint64_t timeout) {
    if (handle == VK_NULL_HANDLE || count == 0 || count > kMaxArrayElements ||
        pFences == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (pFences[i] == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
    }
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
    const VkResult result = static_cast<VkResult>(r.i32());
    if (!r.ok()) return VK_ERROR_DEVICE_LOST;
    return result;
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

VKAPI_ATTR VkResult VKAPI_CALL GetSemaphoreCounterValue(VkDevice handle,VkSemaphore semaphore,uint64_t*value){if(!value)return VK_ERROR_INITIALIZATION_FAILED;auto*d=to_device(handle);Writer w;w.handle(d->remote_id);w.handle(id_from_handle(semaphore));std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkGetSemaphoreCounterValue,w,&reply))return VK_ERROR_DEVICE_LOST;Reader r=payload_reader(reply);VkResult result=static_cast<VkResult>(r.i32());uint64_t v=r.u64();if(!r.ok())return VK_ERROR_DEVICE_LOST;if(result==VK_SUCCESS)*value=v;return result;}
VKAPI_ATTR VkResult VKAPI_CALL WaitSemaphores(VkDevice handle,const VkSemaphoreWaitInfo*info,uint64_t timeout){if(!info||info->pNext||info->semaphoreCount>kMaxArrayElements||(info->semaphoreCount&&(!info->pSemaphores||!info->pValues)))return VK_ERROR_INITIALIZATION_FAILED;auto*d=to_device(handle);Writer w;w.handle(d->remote_id);w.u32(info->flags);w.u32(info->semaphoreCount);for(uint32_t i=0;i<info->semaphoreCount;++i){w.handle(id_from_handle(info->pSemaphores[i]));w.u64(info->pValues[i]);}w.u64(timeout);std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkWaitSemaphores,w,&reply))return VK_ERROR_DEVICE_LOST;Reader r=payload_reader(reply);auto result=static_cast<VkResult>(r.i32());return r.ok()?result:VK_ERROR_DEVICE_LOST;}
VKAPI_ATTR VkResult VKAPI_CALL SignalSemaphore(VkDevice handle,const VkSemaphoreSignalInfo*info){if(!info||info->pNext)return VK_ERROR_INITIALIZATION_FAILED;auto*d=to_device(handle);Writer w;w.handle(d->remote_id);w.handle(id_from_handle(info->semaphore));w.u64(info->value);std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkSignalSemaphore,w,&reply))return VK_ERROR_DEVICE_LOST;Reader r=payload_reader(reply);auto result=static_cast<VkResult>(r.i32());return r.ok()?result:VK_ERROR_DEVICE_LOST;}
VKAPI_ATTR void VKAPI_CALL ResetQueryPool(VkDevice handle,VkQueryPool pool,uint32_t first,uint32_t count){auto*d=to_device(handle);Writer w;w.handle(d->remote_id);w.handle(id_from_handle(pool));w.u32(first);w.u32(count);d->instance->connection.send_oneway(Opcode::vkResetQueryPool,w);}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetBufferDeviceAddress(VkDevice,VkBufferDeviceAddressInfo const*){return 0;}
VKAPI_ATTR uint64_t VKAPI_CALL GetBufferOpaqueCaptureAddress(VkDevice,VkBufferDeviceAddressInfo const*){return 0;}
VKAPI_ATTR uint64_t VKAPI_CALL GetDeviceMemoryOpaqueCaptureAddress(VkDevice,VkDeviceMemoryOpaqueCaptureAddressInfo const*){return 0;}

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

SIMPLE_CREATE(CreateBufferView, VkBufferView, VkBufferViewCreateInfo, write_BufferViewCreateInfo,
              vkCreateBufferView)
SIMPLE_DESTROY(DestroyBufferView, VkBufferView, vkDestroyBufferView)

SIMPLE_CREATE(CreateEvent, VkEvent, VkEventCreateInfo, write_EventCreateInfo, vkCreateEvent)
SIMPLE_DESTROY(DestroyEvent, VkEvent, vkDestroyEvent)

SIMPLE_CREATE(CreateQueryPool, VkQueryPool, VkQueryPoolCreateInfo, write_QueryPoolCreateInfo,
              vkCreateQueryPool)
SIMPLE_DESTROY(DestroyQueryPool, VkQueryPool, vkDestroyQueryPool)

SIMPLE_CREATE(CreateSampler, VkSampler, VkSamplerCreateInfo, write_SamplerCreateInfo,
              vkCreateSampler)
SIMPLE_DESTROY(DestroySampler, VkSampler, vkDestroySampler)

VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(
    VkDevice handle, const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*, VkShaderModule* pShaderModule) {
    if (handle == VK_NULL_HANDLE || pCreateInfo == nullptr || pShaderModule == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    if (!write_ShaderModuleCreateInfo(request, *pCreateInfo)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(
            Opcode::vkCreateShaderModule, request, &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint64_t id = r.handle();
    if (!r.ok()) return VK_ERROR_INITIALIZATION_FAILED;
    if (result == VK_SUCCESS) *pShaderModule = handle_from_id<VkShaderModule>(id);
    return result;
}
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

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice handle,
    const VkRenderPassCreateInfo2* pCreateInfo, const VkAllocationCallbacks*, VkRenderPass* pOut) {
    if (!pCreateInfo || !pOut) return VK_ERROR_INITIALIZATION_FAILED;
    RemoteDevice* device = to_device(handle); Writer request; request.handle(device->remote_id);
    if (!write_RenderPassCreateInfo2(request, *pCreateInfo)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkCreateRenderPass2, request, &reply))
        return VK_ERROR_INITIALIZATION_FAILED;
    Reader r = payload_reader(reply); VkResult result=static_cast<VkResult>(r.i32()); uint64_t id=r.handle();
    if (result==VK_SUCCESS) *pOut=handle_from_id<VkRenderPass>(id); return result;
}
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
    if (!write_ImageViewCreateInfo(request, *pCreateInfo)) return VK_ERROR_EXTENSION_NOT_PRESENT;
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
    if (!write_FramebufferCreateInfo(request, *pCreateInfo)) return VK_ERROR_EXTENSION_NOT_PRESENT;
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

VKAPI_ATTR VkResult VKAPI_CALL GetEventStatus(VkDevice handle, VkEvent event) {
    RemoteDevice* device = to_device(handle);
    Writer request; request.handle(device->remote_id); request.handle(id_from_handle(event));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetEventStatus, request, &reply))
        return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    return r.ok() ? result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL SetEvent(VkDevice handle, VkEvent event) {
    RemoteDevice* device = to_device(handle);
    Writer request; request.handle(device->remote_id); request.handle(id_from_handle(event));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkSetEvent, request, &reply))
        return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply); const VkResult result = static_cast<VkResult>(r.i32());
    return r.ok() ? result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL ResetEvent(VkDevice handle, VkEvent event) {
    RemoteDevice* device = to_device(handle);
    Writer request; request.handle(device->remote_id); request.handle(id_from_handle(event));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkResetEvent, request, &reply))
        return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply); const VkResult result = static_cast<VkResult>(r.i32());
    return r.ok() ? result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL GetQueryPoolResults(
    VkDevice handle, VkQueryPool pool, uint32_t firstQuery, uint32_t queryCount,
    size_t dataSize, void* pData, VkDeviceSize stride, VkQueryResultFlags flags) {
    if (dataSize > UINT32_MAX || (dataSize && pData == nullptr)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    RemoteDevice* device = to_device(handle); Writer request;
    request.handle(device->remote_id); request.handle(id_from_handle(pool));
    request.u32(firstQuery); request.u32(queryCount); request.u64(dataSize); request.u64(stride);
    request.u32(flags);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetQueryPoolResults, request, &reply))
        return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply); const VkResult result = static_cast<VkResult>(r.i32());
    std::vector<char> bytes;
    if (!r.bytes(&bytes, static_cast<size_t>(dataSize)) || bytes.size() != dataSize)
        return VK_ERROR_DEVICE_LOST;
    if (dataSize) memcpy(pData, bytes.data(), dataSize);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPipelineCacheData(VkDevice handle, VkPipelineCache cache,
                                                    size_t* pDataSize, void* pData) {
    if (!pDataSize) return VK_ERROR_INITIALIZATION_FAILED;
    RemoteDevice* device = to_device(handle); Writer request;
    request.handle(device->remote_id); request.handle(id_from_handle(cache));
    request.u64(pData ? *pDataSize : 0); request.u32(pData ? 1 : 0);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetPipelineCacheData, request, &reply))
        return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply); const VkResult result = static_cast<VkResult>(r.i32());
    const uint64_t required = r.u64(); std::vector<char> bytes;
    if (!r.bytes(&bytes) || required > SIZE_MAX) return VK_ERROR_DEVICE_LOST;
    if (pData && !bytes.empty()) memcpy(pData, bytes.data(), bytes.size());
    *pDataSize = static_cast<size_t>(required); return result;
}

VKAPI_ATTR VkResult VKAPI_CALL MergePipelineCaches(VkDevice handle, VkPipelineCache dst,
                                                   uint32_t count,
                                                   const VkPipelineCache* pSources) {
    RemoteDevice* device = to_device(handle); Writer request;
    request.handle(device->remote_id); request.handle(id_from_handle(dst)); request.u32(count);
    for (uint32_t i = 0; i < count; ++i) request.handle(id_from_handle(pSources[i]));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkMergePipelineCaches, request, &reply))
        return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply); const VkResult result = static_cast<VkResult>(r.i32());
    return r.ok() ? result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR void VKAPI_CALL GetRenderAreaGranularity(VkDevice handle, VkRenderPass pass,
                                                    VkExtent2D* pGranularity) {
    if (!pGranularity) return;
    RemoteDevice* device = to_device(handle); Writer request;
    request.handle(device->remote_id); request.handle(id_from_handle(pass));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetRenderAreaGranularity, request, &reply)) {
        *pGranularity = {}; return;
    }
    Reader r = payload_reader(reply); pGranularity->width = r.u32(); pGranularity->height = r.u32();
    if (!r.ok()) *pGranularity = {};
}

VKAPI_ATTR VkResult VKAPI_CALL FreeDescriptorSets(VkDevice handle, VkDescriptorPool pool,
                                                  uint32_t count, const VkDescriptorSet* pSets) {
    if (pool == VK_NULL_HANDLE || count > kMaxArrayElements ||
        (count != 0 && pSets == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (pSets[i] == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
    }
    RemoteDevice* device = to_device(handle); Writer request;
    request.handle(device->remote_id); request.handle(id_from_handle(pool)); request.u32(count);
    for (uint32_t i = 0; i < count; ++i) request.handle(id_from_handle(pSets[i]));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkFreeDescriptorSets, request, &reply))
        return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply); const VkResult result = static_cast<VkResult>(r.i32());
    return r.ok() && r.remaining() == 0 ? result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool(VkDevice handle, VkDescriptorPool pool,
                                                   VkDescriptorPoolResetFlags flags) {
    if (pool == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
    RemoteDevice* device = to_device(handle); Writer request;
    request.handle(device->remote_id); request.handle(id_from_handle(pool)); request.u32(flags);
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkResetDescriptorPool, request, &reply))
        return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply); const VkResult result = static_cast<VkResult>(r.i32());
    return r.ok() && r.remaining() == 0 ? result : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
    VkDevice handle, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks*,
    VkPipeline* pPipelines) {
    if (createInfoCount > kMaxArrayElements ||
        (createInfoCount != 0 && (pCreateInfos == nullptr || pPipelines == nullptr))) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(pipelineCache));
    request.u32(createInfoCount);
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        if (!write_GraphicsPipelineCreateInfo(request, pCreateInfos[i]))
            return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkCreateGraphicsPipelines, request,
                                                  &reply)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint32_t count = r.u32();
    if (!r.ok() || count > kMaxArrayElements || count != createInfoCount ||
        count > r.remaining() / sizeof(uint64_t)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t id = r.handle();
        pPipelines[i] = id ? handle_from_id<VkPipeline>(id) : VK_NULL_HANDLE;
    }
    return r.ok() && r.remaining() == 0 ? result : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(
    VkDevice handle, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks*,
    VkPipeline* pPipelines) {
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.handle(id_from_handle(pipelineCache));
    request.u32(createInfoCount);
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        write_ComputePipelineCreateInfo(request, pCreateInfos[i]);
    }

    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkCreateComputePipelines, request,
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
    return r.ok() && count == createInfoCount ? result : VK_ERROR_INITIALIZATION_FAILED;
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
    if (pInfo == nullptr || pInfo->descriptorPool == VK_NULL_HANDLE ||
        pInfo->descriptorSetCount > kMaxArrayElements ||
        (pInfo->descriptorSetCount != 0 &&
         (pInfo->pSetLayouts == nullptr || pSets == nullptr))) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < pInfo->descriptorSetCount; ++i) {
        if (pInfo->pSetLayouts[i] == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
    }
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
    if (!r.ok() || count > kMaxArrayElements || count != pInfo->descriptorSetCount ||
        count > r.remaining() / sizeof(uint64_t)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t id = r.handle();
        pSets[i] = id ? handle_from_id<VkDescriptorSet>(id) : VK_NULL_HANDLE;
    }
    return r.ok() && r.remaining() == 0 ? result : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(VkDevice handle,const VkDescriptorUpdateTemplateCreateInfo* info,const VkAllocationCallbacks*,VkDescriptorUpdateTemplate* out){if(!info||!out)return VK_ERROR_INITIALIZATION_FAILED;RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);if(!write_DescriptorUpdateTemplateCreateInfo(w,*info))return VK_ERROR_INITIALIZATION_FAILED;std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkCreateDescriptorUpdateTemplate,w,&reply))return VK_ERROR_INITIALIZATION_FAILED;Reader r=payload_reader(reply);VkResult result=static_cast<VkResult>(r.i32());uint64_t id=r.handle();if(result==VK_SUCCESS&&r.ok()){*out=handle_from_id<VkDescriptorUpdateTemplate>(id);d->descriptor_templates[id]={info->templateType,std::vector<VkDescriptorUpdateTemplateEntry>(info->pDescriptorUpdateEntries,info->pDescriptorUpdateEntries+info->descriptorUpdateEntryCount)};}return result;}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(VkDevice handle,VkDescriptorUpdateTemplate object,const VkAllocationCallbacks*){if(!object)return;RemoteDevice*d=to_device(handle);uint64_t id=id_from_handle(object);d->descriptor_templates.erase(id);Writer w;w.handle(d->remote_id);w.handle(id);d->instance->connection.send_oneway(Opcode::vkDestroyDescriptorUpdateTemplate,w);}
VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate(VkDevice handle,VkDescriptorSet set,VkDescriptorUpdateTemplate object,const void* data){RemoteDevice*d=to_device(handle);auto it=d->descriptor_templates.find(id_from_handle(object));if(it==d->descriptor_templates.end()||!data)return;Writer w;w.handle(d->remote_id);w.handle(id_from_handle(set));w.handle(id_from_handle(object));w.u32(static_cast<uint32_t>(it->second.entries.size()));for(const auto&e:it->second.entries){w.u32(e.descriptorCount);w.i32(e.descriptorType);for(uint32_t i=0;i<e.descriptorCount;++i){const char*p=static_cast<const char*>(data)+e.offset+i*e.stride;switch(e.descriptorType){case VK_DESCRIPTOR_TYPE_SAMPLER:case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:{auto*x=reinterpret_cast<const VkDescriptorImageInfo*>(p);w.handle(id_from_handle(x->sampler));w.handle(id_from_handle(x->imageView));w.i32(x->imageLayout);break;}case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:w.handle(id_from_handle(*reinterpret_cast<const VkBufferView*>(p)));break;default:{auto*x=reinterpret_cast<const VkDescriptorBufferInfo*>(p);w.handle(id_from_handle(x->buffer));w.u64(x->offset);w.u64(x->range);break;}}}}d->instance->connection.send_oneway(Opcode::vkUpdateDescriptorSetWithTemplate,w);}
VKAPI_ATTR void VKAPI_CALL GetDescriptorSetLayoutSupport(VkDevice handle,const VkDescriptorSetLayoutCreateInfo* info,VkDescriptorSetLayoutSupport* out){if(!info||!out)return;RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);write_DescriptorSetLayoutCreateInfo(w,*info);std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkGetDescriptorSetLayoutSupport,w,&reply)){out->supported=VK_FALSE;return;}Reader r=payload_reader(reply);out->supported=r.u32();if(!r.ok())out->supported=VK_FALSE;}
VKAPI_ATTR VkResult VKAPI_CALL CreateSamplerYcbcrConversion(VkDevice handle,const VkSamplerYcbcrConversionCreateInfo* info,const VkAllocationCallbacks*,VkSamplerYcbcrConversion*out){if(!info||!out||info->pNext)return VK_ERROR_INITIALIZATION_FAILED;RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);write_SamplerYcbcrConversionCreateInfo(w,*info);std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkCreateSamplerYcbcrConversion,w,&reply))return VK_ERROR_INITIALIZATION_FAILED;Reader r=payload_reader(reply);auto result=static_cast<VkResult>(r.i32());uint64_t id=r.handle();if(result==VK_SUCCESS)*out=handle_from_id<VkSamplerYcbcrConversion>(id);return r.ok()?result:VK_ERROR_INITIALIZATION_FAILED;}
VKAPI_ATTR void VKAPI_CALL DestroySamplerYcbcrConversion(VkDevice handle,VkSamplerYcbcrConversion object,const VkAllocationCallbacks*){if(!object)return;RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);w.handle(id_from_handle(object));d->instance->connection.send_oneway(Opcode::vkDestroySamplerYcbcrConversion,w);}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice handle, uint32_t writeCount,
                                                const VkWriteDescriptorSet* pWrites,
                                                uint32_t copyCount,
                                                const VkCopyDescriptorSet* pCopies) {
    if (writeCount > kMaxArrayElements || copyCount > kMaxArrayElements ||
        (writeCount != 0 && pWrites == nullptr) ||
        (copyCount != 0 && pCopies == nullptr)) {
        return;
    }
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    request.u32(writeCount);
    for (uint32_t i = 0; i < writeCount; ++i) {
        if (!write_WriteDescriptorSet(request, pWrites[i])) return;
    }
    request.u32(copyCount);
    for (uint32_t i = 0; i < copyCount; ++i) {
        if (pCopies[i].srcSet == VK_NULL_HANDLE || pCopies[i].dstSet == VK_NULL_HANDLE) return;
        write_CopyDescriptorSet(request, pCopies[i]);
    }
    device->instance->connection.send_oneway(Opcode::vkUpdateDescriptorSets, request);
}

}  // namespace

const DeviceEntry* get_device_core_entries(size_t* count) {
    static const DeviceEntry kEntries[] = {
#define D(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name)}
        D(CreateDevice),
        D(DestroyDevice),
        D(GetDeviceQueue),
        D(GetDeviceQueue2),
        D(GetDeviceGroupPeerMemoryFeatures),
        {"vkGetDeviceGroupPeerMemoryFeaturesKHR", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceGroupPeerMemoryFeatures)},
        D(QueueSubmit),
        D(QueueBindSparse),
        D(QueueWaitIdle),
        D(DeviceWaitIdle),
        D(CreateFence),
        D(DestroyFence),
        D(ResetFences),
        D(GetFenceStatus),
        D(WaitForFences),
        D(CreateSemaphore),
        D(DestroySemaphore),
        D(GetSemaphoreCounterValue),
        {"vkGetSemaphoreCounterValueKHR", reinterpret_cast<PFN_vkVoidFunction>(GetSemaphoreCounterValue)},
        D(WaitSemaphores),
        {"vkWaitSemaphoresKHR", reinterpret_cast<PFN_vkVoidFunction>(WaitSemaphores)},
        D(SignalSemaphore),
        {"vkSignalSemaphoreKHR", reinterpret_cast<PFN_vkVoidFunction>(SignalSemaphore)},
        D(ResetQueryPool),
        {"vkResetQueryPoolEXT", reinterpret_cast<PFN_vkVoidFunction>(ResetQueryPool)},
        D(GetBufferDeviceAddress),
        {"vkGetBufferDeviceAddressKHR", reinterpret_cast<PFN_vkVoidFunction>(GetBufferDeviceAddress)},
        D(GetBufferOpaqueCaptureAddress),
        {"vkGetBufferOpaqueCaptureAddressKHR", reinterpret_cast<PFN_vkVoidFunction>(GetBufferOpaqueCaptureAddress)},
        D(GetDeviceMemoryOpaqueCaptureAddress),
        {"vkGetDeviceMemoryOpaqueCaptureAddressKHR", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceMemoryOpaqueCaptureAddress)},
        D(CreateBuffer),
        D(DestroyBuffer),
        D(CreateImage),
        D(DestroyImage),
        D(CreateImageView),
        D(DestroyImageView),
        D(CreateBufferView),
        D(DestroyBufferView),
        D(CreateEvent),
        D(DestroyEvent),
        D(GetEventStatus),
        D(SetEvent),
        D(ResetEvent),
        D(CreateQueryPool),
        D(DestroyQueryPool),
        D(GetQueryPoolResults),
        D(CreateSampler),
        D(DestroySampler),
        D(GetImageSubresourceLayout),
        D(CreateShaderModule),
        D(DestroyShaderModule),
        D(CreatePipelineCache),
        D(DestroyPipelineCache),
        D(GetPipelineCacheData),
        D(MergePipelineCaches),
        D(CreatePipelineLayout),
        D(DestroyPipelineLayout),
        D(CreateGraphicsPipelines),
        D(CreateComputePipelines),
        D(DestroyPipeline),
        D(CreateRenderPass),
        D(GetRenderAreaGranularity),
        D(CreateRenderPass2),
        {"vkCreateRenderPass2KHR", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass2)},
        D(DestroyRenderPass),
        D(CreateFramebuffer),
        D(DestroyFramebuffer),
        D(CreateDescriptorSetLayout),
        D(DestroyDescriptorSetLayout),
        D(CreateDescriptorPool),
        D(DestroyDescriptorPool),
        D(AllocateDescriptorSets),
        D(FreeDescriptorSets),
        D(ResetDescriptorPool),
        D(UpdateDescriptorSets),
        D(CreateDescriptorUpdateTemplate),
        {"vkCreateDescriptorUpdateTemplateKHR", reinterpret_cast<PFN_vkVoidFunction>(CreateDescriptorUpdateTemplate)},
        D(DestroyDescriptorUpdateTemplate),
        {"vkDestroyDescriptorUpdateTemplateKHR", reinterpret_cast<PFN_vkVoidFunction>(DestroyDescriptorUpdateTemplate)},
        D(UpdateDescriptorSetWithTemplate),
        {"vkUpdateDescriptorSetWithTemplateKHR", reinterpret_cast<PFN_vkVoidFunction>(UpdateDescriptorSetWithTemplate)},
        D(GetDescriptorSetLayoutSupport),
        {"vkGetDescriptorSetLayoutSupportKHR", reinterpret_cast<PFN_vkVoidFunction>(GetDescriptorSetLayoutSupport)},
        D(CreateSamplerYcbcrConversion),
        {"vkCreateSamplerYcbcrConversionKHR", reinterpret_cast<PFN_vkVoidFunction>(CreateSamplerYcbcrConversion)},
        D(DestroySamplerYcbcrConversion),
        {"vkDestroySamplerYcbcrConversionKHR", reinterpret_cast<PFN_vkVoidFunction>(DestroySamplerYcbcrConversion)},
        D(CreateCommandPool),
        D(DestroyCommandPool),
#undef D
    };
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

}  // namespace remoting
