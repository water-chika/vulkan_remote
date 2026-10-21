// Client-side device, queue, sync, pipeline, descriptor and resource
// creation entry points.
//
// Creation calls round-trip (the caller gets a handle back immediately and
// needs it); destructors and other calls nobody blocks on are sent with
// send_oneway (see remote_objects.hpp for why, and for the oneway error
// count reported back on the next call that does wait).

#include <stdio.h>
#include <string.h>

#include <algorithm>
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

struct DescriptorSlotCursor {
    const std::vector<DescriptorBindingMetadata>* bindings = nullptr;
    size_t binding_index = 0;
    uint32_t element = 0;

    bool current(uint64_t* key) const {
        if (!bindings || binding_index >= bindings->size() ||
            element >= (*bindings)[binding_index].descriptor_count) return false;
        *key = descriptor_slot_key((*bindings)[binding_index].binding, element);
        return true;
    }

    bool advance() {
        if (!bindings || binding_index >= bindings->size()) return false;
        if (++element < (*bindings)[binding_index].descriptor_count) return true;
        element = 0;
        do { ++binding_index; }
        while (binding_index < bindings->size() &&
               (*bindings)[binding_index].descriptor_count == 0);
        return binding_index < bindings->size();
    }
};

DescriptorSlotCursor descriptor_cursor(
    const std::vector<DescriptorBindingMetadata>* bindings,
    uint32_t binding, uint32_t element) {
    DescriptorSlotCursor cursor{bindings, 0, element};
    if (!bindings) return cursor;
    while (cursor.binding_index < bindings->size() &&
           (*bindings)[cursor.binding_index].binding < binding)
        ++cursor.binding_index;
    if (cursor.binding_index == bindings->size() ||
        (*bindings)[cursor.binding_index].binding != binding)
        cursor.bindings = nullptr;
    return cursor;
}

void propagate_queue_writes_locked(RemoteDevice* device, uint64_t queue_id,
                                   uint64_t after_serial,
                                   const std::unordered_set<uint64_t>& writes) {
    auto& queue_writes = device->queue_writes[queue_id];
    queue_writes.insert(writes.begin(), writes.end());
    for (auto& item : device->fence_writes) {
        if (item.second.queue_id == queue_id && item.second.submission_serial >= after_serial)
            item.second.memory_ids.insert(writes.begin(), writes.end());
    }
    for (auto& item : device->semaphores) {
        if (item.second.binary_signaled && item.second.binary_queue_id == queue_id &&
            item.second.binary_submission_serial >= after_serial)
            item.second.binary_writes.insert(writes.begin(), writes.end());
        for (TimelineSignalWrites& signal : item.second.timeline_writes) {
            if (signal.queue_id == queue_id && signal.submission_serial >= after_serial)
                signal.memory_ids.insert(writes.begin(), writes.end());
        }
    }
}

bool acquire_timeline_writes(RemoteDevice* device, uint64_t semaphore_id, uint64_t value) {
    std::unordered_set<uint64_t> completed;
    {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        const auto records = device->semaphores.find(semaphore_id);
        if (records == device->semaphores.end()) return true;
        SemaphoreProvenance& semaphore = records->second;
        completed = semaphore.acquired_timeline_writes;
        for (const TimelineSignalWrites& signal : semaphore.timeline_writes) {
            if (signal.value <= value)
                completed.insert(signal.memory_ids.begin(), signal.memory_ids.end());
        }
    }
    if (!device->acquire_coherent_mapped(&completed)) return false;
    {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        const auto records = device->semaphores.find(semaphore_id);
        if (records == device->semaphores.end()) return true;
        SemaphoreProvenance& semaphore = records->second;
        semaphore.acquired_timeline_value =
            (std::max)(semaphore.acquired_timeline_value, value);
        semaphore.acquired_timeline_writes.insert(completed.begin(), completed.end());
        semaphore.timeline_writes.erase(
            std::remove_if(semaphore.timeline_writes.begin(), semaphore.timeline_writes.end(),
                           [value](const TimelineSignalWrites& signal) {
                               return signal.value <= value;
                           }),
            semaphore.timeline_writes.end());
    }
    return true;
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
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES){const auto&f=*reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(node);if(f.bufferDeviceAddressCaptureReplay||f.bufferDeviceAddressMultiDevice)return VK_ERROR_FEATURE_NOT_PRESENT;}
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES){const auto&f=*reinterpret_cast<const VkPhysicalDeviceDescriptorIndexingFeatures*>(node);auto allowed=f;remoting::apply_descriptor_indexing_feature_policy(&allowed);const auto*requested=reinterpret_cast<const VkBool32*>(&f.shaderInputAttachmentArrayDynamicIndexing);const auto*filtered=reinterpret_cast<const VkBool32*>(&allowed.shaderInputAttachmentArrayDynamicIndexing);constexpr size_t n=(sizeof(f)-offsetof(VkPhysicalDeviceDescriptorIndexingFeatures,shaderInputAttachmentArrayDynamicIndexing))/sizeof(VkBool32);for(size_t i=0;i<n;++i)if(requested[i]&&!filtered[i])return VK_ERROR_FEATURE_NOT_PRESENT;}
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES&&reinterpret_cast<const VkPhysicalDeviceSynchronization2Features*>(node)->synchronization2&&!extension_enabled(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME))return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    auto* pd = reinterpret_cast<struct RemotePhysicalDevice*>(handle);
    const bool properties2_enabled = pd->instance->enabled_extensions.count(
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) != 0;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* requested = pCreateInfo->ppEnabledExtensionNames
            ? pCreateInfo->ppEnabledExtensionNames[i] : nullptr;
        if (requested == nullptr ||
            (strcmp(requested, VK_KHR_SWAPCHAIN_EXTENSION_NAME) != 0 &&
             strcmp(requested, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) != 0 &&
             strcmp(requested, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) != 0 &&
             strcmp(requested, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) != 0 &&
             !remoting::is_required_device_extension(requested)) ||
            ((remoting::is_required_device_extension(requested) ||
              strcmp(requested, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0 ||
              strcmp(requested, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0 ||
              strcmp(requested, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) == 0) &&
             pd->api_version < VK_API_VERSION_1_1 &&
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
    std::lock_guard<std::mutex> lock(device->metadata_mutex);
    for (RemoteQueue* existing : device->queues) {
        if (existing->remote_id == id) {
            *pQueue = reinterpret_cast<VkQueue>(existing);
            return;
        }
    }
    auto* queue = new RemoteQueue();
    set_loader_magic_value(queue);
    queue->device = device;
    queue->remote_id = id;
    device->queues.push_back(queue);
    *pQueue = reinterpret_cast<VkQueue>(queue);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice handle,const VkDeviceQueueInfo2* info,VkQueue* out){
    if(!info||!out){return;}RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);w.u32(info->flags);w.u32(info->queueFamilyIndex);w.u32(info->queueIndex);std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkGetDeviceQueue2,w,&reply)){*out=VK_NULL_HANDLE;return;}Reader r=payload_reader(reply);uint64_t id=r.handle();if(!r.ok()||!id){*out=VK_NULL_HANDLE;return;}std::lock_guard<std::mutex>lock(d->metadata_mutex);for(RemoteQueue*q:d->queues){if(q->remote_id==id){*out=reinterpret_cast<VkQueue>(q);return;}}auto*q=new RemoteQueue();set_loader_magic_value(q);q->device=d;q->remote_id=id;d->queues.push_back(q);*out=reinterpret_cast<VkQueue>(q);
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
    std::lock_guard<std::mutex> queue_lock(queue->mutex);

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
    if (!device->publish_coherent_mapped()) return VK_ERROR_DEVICE_LOST;

    struct PlannedSignal {
        uint64_t semaphore_id = 0;
        bool timeline = false;
        uint64_t value = 0;
        uint64_t submission_serial = 0;
        std::unordered_set<uint64_t> memory_ids;
    };
    std::unique_lock<std::mutex> metadata_lock(device->metadata_mutex);
    const uint64_t first_submission_serial = device->next_submission_serial;
    device->next_submission_serial += submitCount ? submitCount : 1;
    uint64_t submission_serial = first_submission_serial;
    std::unordered_set<uint64_t> next_queue_writes = device->queue_writes[queue->remote_id];
    std::unordered_set<uint64_t> consumed_binary;
    struct PlannedWait { uint64_t semaphore_id; PendingSemaphoreWait wait; };
    std::vector<PlannedWait> planned_waits;
    std::vector<PlannedSignal> planned_signals;
    for (uint32_t i = 0; i < submitCount; ++i, ++submission_serial) {
        const VkSubmitInfo& submit = pSubmits[i];
        const VkTimelineSemaphoreSubmitInfo* timeline = nullptr;
        for (const auto* node = static_cast<const VkBaseInStructure*>(submit.pNext);
             node; node = node->pNext)
            if (node->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
                timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(node);
        std::unordered_set<uint64_t> submit_writes;
        for (uint32_t j = 0; j < submit.commandBufferCount; ++j) {
            RemoteCommandBuffer* cb = to_cmd(submit.pCommandBuffers[j]);
            submit_writes.insert(cb->host_visible_writes.begin(), cb->host_visible_writes.end());
            if (!cb->descriptor_writes_host_visible) continue;
            for (uint64_t set_id : cb->writable_descriptor_sets) {
                const auto resources = device->descriptor_resources.find(set_id);
                if (resources == device->descriptor_resources.end()) continue;
                for (const auto& item : resources->second) {
                    const DescriptorResource& resource = item.second;
                    uint64_t memory_id = 0;
                    if (resource.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                        resource.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                        const auto binding = device->buffer_bindings.find(resource.resource_id);
                        if (binding != device->buffer_bindings.end()) memory_id = binding->second.memory_id;
                    } else if (resource.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                        const auto view = device->image_view_images.find(resource.resource_id);
                        if (view != device->image_view_images.end()) {
                            const auto binding = device->image_bindings.find(view->second);
                            if (binding != device->image_bindings.end()) memory_id = binding->second.memory_id;
                        }
                    } else if (resource.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                        const auto view = device->buffer_view_buffers.find(resource.resource_id);
                        if (view != device->buffer_view_buffers.end()) {
                            const auto binding = device->buffer_bindings.find(view->second);
                            if (binding != device->buffer_bindings.end()) memory_id = binding->second.memory_id;
                        }
                    }
                    if (memory_id) submit_writes.insert(memory_id);
                }
            }
        }
        for (uint32_t j = 0; j < submit.waitSemaphoreCount; ++j) {
            const uint64_t id = id_from_handle(submit.pWaitSemaphores[j]);
            auto semaphore = device->semaphores.find(id);
            if (semaphore == device->semaphores.end()) continue;
            bool resolved_wait = false;
            if (semaphore->second.timeline && timeline && j < timeline->waitSemaphoreValueCount) {
                const uint64_t wait_value = timeline->pWaitSemaphoreValues[j];
                submit_writes.insert(semaphore->second.acquired_timeline_writes.begin(),
                                     semaphore->second.acquired_timeline_writes.end());
                if (semaphore->second.acquired_timeline_value >= wait_value)
                    resolved_wait = true;
                for (const TimelineSignalWrites& signal : semaphore->second.timeline_writes)
                    if (!resolved_wait && signal.value >= wait_value) {
                        submit_writes.insert(signal.memory_ids.begin(), signal.memory_ids.end());
                        resolved_wait = true;
                        break;
                    }
            } else if (!semaphore->second.timeline) {
                submit_writes.insert(semaphore->second.binary_writes.begin(),
                                     semaphore->second.binary_writes.end());
                resolved_wait = semaphore->second.binary_signaled;
                consumed_binary.insert(id);
            }
            if (!resolved_wait) {
                const uint64_t wait_value = semaphore->second.timeline && timeline &&
                                            j < timeline->waitSemaphoreValueCount
                    ? timeline->pWaitSemaphoreValues[j] : 0;
                PendingSemaphoreWait wait{wait_value, queue->remote_id, submission_serial};
                for (uint32_t k = 0; k < submit.signalSemaphoreCount; ++k)
                    wait.downstream_semaphores.push_back(
                        id_from_handle(submit.pSignalSemaphores[k]));
                planned_waits.push_back(PlannedWait{id, std::move(wait)});
            }
        }
        next_queue_writes.insert(submit_writes.begin(), submit_writes.end());
        for (uint32_t j = 0; j < submit.signalSemaphoreCount; ++j) {
            const uint64_t id = id_from_handle(submit.pSignalSemaphores[j]);
            const auto semaphore = device->semaphores.find(id);
            const bool is_timeline = semaphore != device->semaphores.end() && semaphore->second.timeline;
            PlannedSignal signal;
            signal.semaphore_id = id;
            signal.timeline = is_timeline;
            signal.value = is_timeline && timeline && j < timeline->signalSemaphoreValueCount
                ? timeline->pSignalSemaphoreValues[j] : 0;
            signal.submission_serial = submission_serial;
            signal.memory_ids = next_queue_writes;
            planned_signals.push_back(std::move(signal));
        }
    }
    metadata_lock.unlock();
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkQueueSubmit, request, &reply))
        return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    const uint32_t oneway_errors = r.u32();
    if (!r.ok() || r.remaining() != 0) return VK_ERROR_DEVICE_LOST;
    report_oneway_errors(oneway_errors);
    if (result != VK_SUCCESS) return result;
    metadata_lock.lock();
    device->queue_writes[queue->remote_id] = next_queue_writes;
    for (const PlannedWait& planned : planned_waits)
        device->semaphores[planned.semaphore_id].pending_waits.push_back(planned.wait);
    for (uint64_t id : consumed_binary) {
        device->semaphores[id].binary_writes.clear();
        device->semaphores[id].binary_signaled = false;
        device->semaphores[id].binary_queue_id = 0;
        device->semaphores[id].binary_submission_serial = 0;
    }
    for (PlannedSignal& signal : planned_signals) {
        SemaphoreProvenance& semaphore = device->semaphores[signal.semaphore_id];
        if (signal.timeline) semaphore.timeline_writes.push_back(
            TimelineSignalWrites{signal.value, queue->remote_id,
                                  signal.submission_serial, signal.memory_ids});
        else {
            semaphore.binary_writes = signal.memory_ids;
            semaphore.binary_signaled = true;
            semaphore.binary_queue_id = queue->remote_id;
            semaphore.binary_submission_serial = signal.submission_serial;
        }
        for (const PendingSemaphoreWait& wait : semaphore.pending_waits) {
            if (signal.timeline && signal.value < wait.value) continue;
            propagate_queue_writes_locked(device, wait.queue_id,
                                          wait.submission_serial, signal.memory_ids);
            for (uint64_t downstream_id : wait.downstream_semaphores) {
                SemaphoreProvenance& downstream = device->semaphores[downstream_id];
                if (downstream.timeline) {
                    for (TimelineSignalWrites& record : downstream.timeline_writes)
                        if (record.queue_id == wait.queue_id &&
                            record.submission_serial >= wait.submission_serial)
                            record.memory_ids.insert(signal.memory_ids.begin(),
                                                     signal.memory_ids.end());
                } else if (downstream.binary_signaled &&
                           downstream.binary_queue_id == wait.queue_id &&
                           downstream.binary_submission_serial >= wait.submission_serial) {
                    downstream.binary_writes.insert(signal.memory_ids.begin(),
                                                    signal.memory_ids.end());
                }
            }
        }
        semaphore.pending_waits.erase(
            std::remove_if(semaphore.pending_waits.begin(), semaphore.pending_waits.end(),
                           [&signal](const PendingSemaphoreWait& wait) {
                               return !signal.timeline || signal.value >= wait.value;
                           }),
            semaphore.pending_waits.end());
    }
    if (fence != VK_NULL_HANDLE)
        device->fence_writes[id_from_handle(fence)] =
            PendingCoherentWrites{queue->remote_id,
                                  submitCount ? submission_serial - 1 : first_submission_serial,
                                  next_queue_writes};
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue handle, uint32_t submitCount,
                                             const VkSubmitInfo2* submits, VkFence fence) {
    if (handle == VK_NULL_HANDLE || submitCount > kMaxArrayElements ||
        (submitCount && !submits)) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i=0;i<submitCount;++i) {
        const auto& submit=submits[i];
        for(uint32_t j=0;j<submit.waitSemaphoreInfoCount;++j)
            if(submit.pWaitSemaphoreInfos[j].semaphore==VK_NULL_HANDLE)
                return VK_ERROR_INITIALIZATION_FAILED;
        for(uint32_t j=0;j<submit.commandBufferInfoCount;++j)
            if(submit.pCommandBufferInfos[j].commandBuffer==VK_NULL_HANDLE)
                return VK_ERROR_INITIALIZATION_FAILED;
        for(uint32_t j=0;j<submit.signalSemaphoreInfoCount;++j)
            if(submit.pSignalSemaphoreInfos[j].semaphore==VK_NULL_HANDLE)
                return VK_ERROR_INITIALIZATION_FAILED;
    }
    RemoteQueue* queue = to_queue(handle);
    RemoteDevice* device = queue->device;
    std::lock_guard<std::mutex> queue_lock(queue->mutex);
    Writer request;
    request.handle(queue->remote_id);
    request.handle(id_from_handle(fence));
    request.u32(submitCount);
    const auto command_buffer_id = [](VkCommandBuffer handle) -> uint64_t {
        return handle ? to_cmd(handle)->remote_id : 0;
    };
    for (uint32_t i = 0; i < submitCount; ++i)
        if (!write_SubmitInfo2(request, submits[i], command_buffer_id))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!device->publish_coherent_mapped()) return VK_ERROR_DEVICE_LOST;

    struct PlannedWait { uint64_t semaphore_id; PendingSemaphoreWait wait; };
    struct PlannedSignal { uint64_t semaphore_id; bool timeline; uint64_t value;
                           uint64_t serial; std::unordered_set<uint64_t> writes; };
    std::vector<PlannedWait> planned_waits;
    std::vector<PlannedSignal> planned_signals;
    std::unordered_set<uint64_t> consumed_binary;
    std::unique_lock<std::mutex> lock(device->metadata_mutex);
    const uint64_t first_serial = device->next_submission_serial;
    device->next_submission_serial += submitCount ? submitCount : 1;
    uint64_t serial = first_serial;
    auto next_writes = device->queue_writes[queue->remote_id];
    for (uint32_t i = 0; i < submitCount; ++i, ++serial) {
        const VkSubmitInfo2& submit = submits[i];
        std::unordered_set<uint64_t> writes;
        for (uint32_t j = 0; j < submit.commandBufferInfoCount; ++j) {
            auto* cb = to_cmd(submit.pCommandBufferInfos[j].commandBuffer);
            writes.insert(cb->host_visible_writes.begin(), cb->host_visible_writes.end());
            if (!cb->descriptor_writes_host_visible) continue;
            for (uint64_t set_id : cb->writable_descriptor_sets) {
                const auto resources = device->descriptor_resources.find(set_id);
                if (resources == device->descriptor_resources.end()) continue;
                for (const auto& item : resources->second) {
                    const DescriptorResource& resource = item.second;
                    uint64_t memory_id = 0;
                    if (resource.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                        resource.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                        const auto binding = device->buffer_bindings.find(resource.resource_id);
                        if (binding != device->buffer_bindings.end()) memory_id = binding->second.memory_id;
                    } else if (resource.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                        const auto view = device->image_view_images.find(resource.resource_id);
                        if (view != device->image_view_images.end()) {
                            const auto binding = device->image_bindings.find(view->second);
                            if (binding != device->image_bindings.end()) memory_id = binding->second.memory_id;
                        }
                    } else if (resource.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                        const auto view = device->buffer_view_buffers.find(resource.resource_id);
                        if (view != device->buffer_view_buffers.end()) {
                            const auto binding = device->buffer_bindings.find(view->second);
                            if (binding != device->buffer_bindings.end()) memory_id = binding->second.memory_id;
                        }
                    }
                    if (memory_id) writes.insert(memory_id);
                }
            }
        }
        for (uint32_t j = 0; j < submit.waitSemaphoreInfoCount; ++j) {
            const auto& wait_info = submit.pWaitSemaphoreInfos[j];
            const uint64_t id = id_from_handle(wait_info.semaphore);
            auto found = device->semaphores.find(id);
            if (found == device->semaphores.end()) continue;
            auto& semaphore = found->second;
            bool resolved = false;
            if (semaphore.timeline) {
                writes.insert(semaphore.acquired_timeline_writes.begin(),
                              semaphore.acquired_timeline_writes.end());
                resolved = semaphore.acquired_timeline_value >= wait_info.value;
                for (const auto& signal : semaphore.timeline_writes) {
                    if (resolved || signal.value < wait_info.value) continue;
                    writes.insert(signal.memory_ids.begin(), signal.memory_ids.end());
                    resolved = true;
                }
            } else {
                writes.insert(semaphore.binary_writes.begin(), semaphore.binary_writes.end());
                resolved = semaphore.binary_signaled;
                consumed_binary.insert(id);
            }
            if (!resolved) {
                PendingSemaphoreWait wait{wait_info.value, queue->remote_id, serial};
                for (uint32_t k=0;k<submit.signalSemaphoreInfoCount;++k)
                    wait.downstream_semaphores.push_back(
                        id_from_handle(submit.pSignalSemaphoreInfos[k].semaphore));
                planned_waits.push_back({id, std::move(wait)});
            }
        }
        next_writes.insert(writes.begin(), writes.end());
        for (uint32_t j=0;j<submit.signalSemaphoreInfoCount;++j) {
            const auto& info=submit.pSignalSemaphoreInfos[j];
            const uint64_t id=id_from_handle(info.semaphore);
            const auto found=device->semaphores.find(id);
            const bool timeline=found!=device->semaphores.end()&&found->second.timeline;
            planned_signals.push_back({id,timeline,timeline?info.value:0,serial,next_writes});
        }
    }
    lock.unlock();
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkQueueSubmit2,request,&reply))
        return VK_ERROR_DEVICE_LOST;
    Reader reader=payload_reader(reply);
    VkResult result=static_cast<VkResult>(reader.i32());
    uint32_t errors=reader.u32();
    report_oneway_errors(errors);
    if(!reader.ok()||reader.remaining()!=0)return VK_ERROR_DEVICE_LOST;
    if(result!=VK_SUCCESS)return result;
    lock.lock();
    device->queue_writes[queue->remote_id]=next_writes;
    for(const auto&wait:planned_waits)
        device->semaphores[wait.semaphore_id].pending_waits.push_back(wait.wait);
    for(uint64_t id:consumed_binary){auto&s=device->semaphores[id];s.binary_writes.clear();s.binary_signaled=false;s.binary_queue_id=0;s.binary_submission_serial=0;}
    for(const auto&signal:planned_signals){auto&s=device->semaphores[signal.semaphore_id];if(signal.timeline)s.timeline_writes.push_back({signal.value,queue->remote_id,signal.serial,signal.writes});else{s.binary_signaled=true;s.binary_queue_id=queue->remote_id;s.binary_submission_serial=signal.serial;s.binary_writes=signal.writes;}for(const auto&wait:s.pending_waits){if(signal.timeline&&signal.value<wait.value)continue;propagate_queue_writes_locked(device,wait.queue_id,wait.submission_serial,signal.writes);for(uint64_t downstream_id:wait.downstream_semaphores){auto&downstream=device->semaphores[downstream_id];if(downstream.timeline){for(auto&record:downstream.timeline_writes)if(record.queue_id==wait.queue_id&&record.submission_serial>=wait.submission_serial)record.memory_ids.insert(signal.writes.begin(),signal.writes.end());}else if(downstream.binary_signaled&&downstream.binary_queue_id==wait.queue_id&&downstream.binary_submission_serial>=wait.submission_serial)downstream.binary_writes.insert(signal.writes.begin(),signal.writes.end());}}s.pending_waits.erase(std::remove_if(s.pending_waits.begin(),s.pending_waits.end(),[&signal](const PendingSemaphoreWait&wait){return !signal.timeline||signal.value>=wait.value;}),s.pending_waits.end());}
    if(fence!=VK_NULL_HANDLE)device->fence_writes[id_from_handle(fence)]={queue->remote_id,submitCount?serial-1:first_serial,next_writes};
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(VkQueue handle, uint32_t bindInfoCount,
                                               const VkBindSparseInfo* pBindInfo,
                                               VkFence fence) {
    RemoteQueue* queue = to_queue(handle); RemoteDevice* device = queue->device;
    std::lock_guard<std::mutex> queue_lock(queue->mutex);
    if (bindInfoCount > kMaxArrayElements || (bindInfoCount && !pBindInfo))
        return VK_ERROR_INITIALIZATION_FAILED;
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
    if (!device->publish_coherent_mapped()) return VK_ERROR_DEVICE_LOST;
    std::vector<char> reply; if(!device->instance->connection.round_trip(Opcode::vkQueueBindSparse,w,&reply))return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply);
    VkResult result = static_cast<VkResult>(r.i32());
    uint32_t errors = r.u32();
    report_oneway_errors(errors);
    if (!r.ok()) return VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) return result;

    std::lock_guard<std::mutex> lock(device->metadata_mutex);
    const uint64_t serial = device->next_submission_serial++;
    auto& writes = device->queue_writes[queue->remote_id];
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        const auto& info = pBindInfo[i];
        const VkTimelineSemaphoreSubmitInfo* timeline = nullptr;
        for (const auto* node = static_cast<const VkBaseInStructure*>(info.pNext);
             node; node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
                timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(node);
        }
        for (uint32_t j = 0; j < info.waitSemaphoreCount; ++j) {
            const uint64_t id = id_from_handle(info.pWaitSemaphores[j]);
            auto semaphore = device->semaphores.find(id);
            if (semaphore == device->semaphores.end()) continue;
            bool resolved = false;
            if (semaphore->second.timeline && timeline &&
                j < timeline->waitSemaphoreValueCount) {
                const uint64_t wait_value = timeline->pWaitSemaphoreValues[j];
                writes.insert(semaphore->second.acquired_timeline_writes.begin(),
                              semaphore->second.acquired_timeline_writes.end());
                if (semaphore->second.acquired_timeline_value >= wait_value)
                    resolved = true;
                for (const auto& signal : semaphore->second.timeline_writes) {
                    if (resolved || signal.value < wait_value) continue;
                    writes.insert(signal.memory_ids.begin(), signal.memory_ids.end());
                    resolved = true;
                    break;
                }
            } else if (!semaphore->second.timeline && semaphore->second.binary_signaled) {
                writes.insert(semaphore->second.binary_writes.begin(),
                              semaphore->second.binary_writes.end());
                semaphore->second.binary_signaled = false;
                semaphore->second.binary_writes.clear();
                resolved = true;
            }
            if (!resolved) {
                const uint64_t value = semaphore->second.timeline && timeline &&
                                       j < timeline->waitSemaphoreValueCount
                    ? timeline->pWaitSemaphoreValues[j] : 0;
                PendingSemaphoreWait wait{value, queue->remote_id, serial};
                for (uint32_t k = 0; k < info.signalSemaphoreCount; ++k)
                    wait.downstream_semaphores.push_back(
                        id_from_handle(info.pSignalSemaphores[k]));
                semaphore->second.pending_waits.push_back(std::move(wait));
            }
        }
        for (uint32_t j = 0; j < info.signalSemaphoreCount; ++j) {
            const uint64_t id = id_from_handle(info.pSignalSemaphores[j]);
            auto& semaphore = device->semaphores[id];
            uint64_t signal_value = 0;
            if (semaphore.timeline && timeline && j < timeline->signalSemaphoreValueCount) {
                signal_value = timeline->pSignalSemaphoreValues[j];
                semaphore.timeline_writes.push_back(
                    {signal_value, queue->remote_id, serial, writes});
            } else if (!semaphore.timeline) {
                semaphore.binary_signaled = true;
                semaphore.binary_queue_id = queue->remote_id;
                semaphore.binary_submission_serial = serial;
                semaphore.binary_writes = writes;
            }
            for (const PendingSemaphoreWait& wait : semaphore.pending_waits) {
                if (semaphore.timeline && signal_value < wait.value) continue;
                propagate_queue_writes_locked(device, wait.queue_id,
                                              wait.submission_serial, writes);
                for (uint64_t downstream_id : wait.downstream_semaphores) {
                    auto& downstream = device->semaphores[downstream_id];
                    if (downstream.timeline) {
                        for (auto& record : downstream.timeline_writes)
                            if (record.queue_id == wait.queue_id &&
                                record.submission_serial >= wait.submission_serial)
                                record.memory_ids.insert(writes.begin(), writes.end());
                    } else if (downstream.binary_signaled &&
                               downstream.binary_queue_id == wait.queue_id &&
                               downstream.binary_submission_serial >= wait.submission_serial) {
                        downstream.binary_writes.insert(writes.begin(), writes.end());
                    }
                }
            }
            semaphore.pending_waits.erase(
                std::remove_if(semaphore.pending_waits.begin(), semaphore.pending_waits.end(),
                    [&semaphore, signal_value](const PendingSemaphoreWait& wait) {
                        return !semaphore.timeline || signal_value >= wait.value;
                    }), semaphore.pending_waits.end());
        }
    }
    if (fence != VK_NULL_HANDLE)
        device->fence_writes[id_from_handle(fence)] =
            {queue->remote_id, serial, writes};
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue handle) {
    RemoteQueue* queue = to_queue(handle);
    std::lock_guard<std::mutex> queue_lock(queue->mutex);
    Writer request;
    request.handle(queue->remote_id);
    std::vector<char> reply;
    if (!queue->device->instance->connection.round_trip(Opcode::vkQueueWaitIdle, request, &reply)) {
        return VK_ERROR_DEVICE_LOST;
    }
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    if (!r.ok()) return VK_ERROR_DEVICE_LOST;
    if (result == VK_SUCCESS) {
        std::unordered_set<uint64_t> completed;
        {
            std::lock_guard<std::mutex> lock(queue->device->metadata_mutex);
            completed = queue->device->queue_writes[queue->remote_id];
        }
        if (!queue->device->acquire_coherent_mapped(&completed))
            return VK_ERROR_DEVICE_LOST;
        std::lock_guard<std::mutex> lock(queue->device->metadata_mutex);
        queue->device->queue_writes[queue->remote_id].clear();
    }
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
    if (result == VK_SUCCESS) {
        if (!device->acquire_coherent_mapped()) return VK_ERROR_DEVICE_LOST;
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->queue_writes.clear();
        device->fence_writes.clear();
    }
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
    const uint64_t fence_id = id_from_handle(fence);
    request.handle(fence_id);
    {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->fence_writes.erase(fence_id);
    }
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
    if (r.ok() && r.remaining() == 0 && result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        for (uint32_t i = 0; i < count; ++i)
            device->fence_writes.erase(id_from_handle(pFences[i]));
    }
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
    if (result == VK_SUCCESS) {
        std::unordered_set<uint64_t> completed;
        {
            std::lock_guard<std::mutex> lock(device->metadata_mutex);
            const auto writes = device->fence_writes.find(id_from_handle(fence));
            if (writes != device->fence_writes.end()) completed = writes->second.memory_ids;
        }
        if (!device->acquire_coherent_mapped(&completed)) return VK_ERROR_DEVICE_LOST;
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->fence_writes.erase(id_from_handle(fence));
    }
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
    if (result == VK_SUCCESS) {
        std::unordered_set<uint64_t> completed;
        std::vector<uint64_t> completed_fences;
        for (uint32_t i = 0; i < count; ++i) {
            if (!waitAll) {
                Writer status_request;
                status_request.handle(device->remote_id);
                status_request.handle(id_from_handle(pFences[i]));
                std::vector<char> status_reply;
                if (!device->instance->connection.round_trip(
                        Opcode::vkGetFenceStatus, status_request, &status_reply))
                    return VK_ERROR_DEVICE_LOST;
                Reader status_reader = payload_reader(status_reply);
                const VkResult status = static_cast<VkResult>(status_reader.i32());
                if (!status_reader.ok()) return VK_ERROR_DEVICE_LOST;
                if (status == VK_NOT_READY) continue;
                if (status != VK_SUCCESS) return status;
            }
            const uint64_t id = id_from_handle(pFences[i]);
            std::lock_guard<std::mutex> lock(device->metadata_mutex);
            const auto writes = device->fence_writes.find(id);
            if (writes != device->fence_writes.end()) {
                completed.insert(writes->second.memory_ids.begin(), writes->second.memory_ids.end());
                completed_fences.push_back(id);
            }
        }
        if (!device->acquire_coherent_mapped(&completed)) return VK_ERROR_DEVICE_LOST;
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        for (uint64_t id : completed_fences) device->fence_writes.erase(id);
    }
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
    if (result == VK_SUCCESS) {
        *pSemaphore = handle_from_id<VkSemaphore>(id);
        bool timeline = false;
        for (const auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node; node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO)
                timeline = reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(node)->semaphoreType ==
                           VK_SEMAPHORE_TYPE_TIMELINE;
        }
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->semaphores[id].timeline = timeline;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice handle, VkSemaphore semaphore,
                                            const VkAllocationCallbacks*) {
    if (semaphore == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    Writer request;
    request.handle(device->remote_id);
    const uint64_t id = id_from_handle(semaphore);
    request.handle(id);
    {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->semaphores.erase(id);
    }
    device->instance->connection.send_oneway(Opcode::vkDestroySemaphore, request);
}

VKAPI_ATTR VkResult VKAPI_CALL GetSemaphoreCounterValue(VkDevice handle,VkSemaphore semaphore,uint64_t*value){if(!value)return VK_ERROR_INITIALIZATION_FAILED;auto*d=to_device(handle);uint64_t id=id_from_handle(semaphore);Writer w;w.handle(d->remote_id);w.handle(id);std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkGetSemaphoreCounterValue,w,&reply))return VK_ERROR_DEVICE_LOST;Reader r=payload_reader(reply);VkResult result=static_cast<VkResult>(r.i32());uint64_t v=r.u64();if(!r.ok())return VK_ERROR_DEVICE_LOST;if(result==VK_SUCCESS){*value=v;if(!acquire_timeline_writes(d,id,v))return VK_ERROR_DEVICE_LOST;}return result;}
VKAPI_ATTR VkResult VKAPI_CALL WaitSemaphores(VkDevice handle,const VkSemaphoreWaitInfo*info,uint64_t timeout){if(!info||info->pNext||info->semaphoreCount>kMaxArrayElements||(info->semaphoreCount&&(!info->pSemaphores||!info->pValues)))return VK_ERROR_INITIALIZATION_FAILED;auto*d=to_device(handle);Writer w;w.handle(d->remote_id);w.u32(info->flags);w.u32(info->semaphoreCount);for(uint32_t i=0;i<info->semaphoreCount;++i){w.handle(id_from_handle(info->pSemaphores[i]));w.u64(info->pValues[i]);}w.u64(timeout);std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkWaitSemaphores,w,&reply))return VK_ERROR_DEVICE_LOST;Reader r=payload_reader(reply);auto result=static_cast<VkResult>(r.i32());if(!r.ok())return VK_ERROR_DEVICE_LOST;if(result==VK_SUCCESS){for(uint32_t i=0;i<info->semaphoreCount;++i){uint64_t id=id_from_handle(info->pSemaphores[i]);uint64_t reached=info->pValues[i];if(info->flags&VK_SEMAPHORE_WAIT_ANY_BIT){VkResult query=GetSemaphoreCounterValue(handle,info->pSemaphores[i],&reached);if(query!=VK_SUCCESS||reached<info->pValues[i])continue;}if(!acquire_timeline_writes(d,id,reached))return VK_ERROR_DEVICE_LOST;}}return result;}
VKAPI_ATTR VkResult VKAPI_CALL SignalSemaphore(VkDevice handle,const VkSemaphoreSignalInfo*info){if(!info||info->pNext)return VK_ERROR_INITIALIZATION_FAILED;auto*d=to_device(handle);if(!d->publish_coherent_mapped())return VK_ERROR_DEVICE_LOST;uint64_t id=id_from_handle(info->semaphore);Writer w;w.handle(d->remote_id);w.handle(id);w.u64(info->value);std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkSignalSemaphore,w,&reply))return VK_ERROR_DEVICE_LOST;Reader r=payload_reader(reply);auto result=static_cast<VkResult>(r.i32());if(!r.ok())return VK_ERROR_DEVICE_LOST;if(result==VK_SUCCESS){std::lock_guard<std::mutex>lock(d->metadata_mutex);auto semaphore=d->semaphores.find(id);if(semaphore!=d->semaphores.end()){auto&waits=semaphore->second.pending_waits;waits.erase(std::remove_if(waits.begin(),waits.end(),[info](const PendingSemaphoreWait&wait){return wait.value<=info->value;}),waits.end());}}return result;}
VKAPI_ATTR void VKAPI_CALL ResetQueryPool(VkDevice handle,VkQueryPool pool,uint32_t first,uint32_t count){auto*d=to_device(handle);Writer w;w.handle(d->remote_id);w.handle(id_from_handle(pool));w.u32(first);w.u32(count);d->instance->connection.send_oneway(Opcode::vkResetQueryPool,w);}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetBufferDeviceAddress(VkDevice handle,const VkBufferDeviceAddressInfo*info){if(!handle||!info||info->sType!=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO||info->pNext||!info->buffer)return 0;auto*d=to_device(handle);Writer w;w.handle(d->remote_id);w.handle(id_from_handle(info->buffer));std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkGetBufferDeviceAddress,w,&reply))return 0;Reader r=payload_reader(reply);VkDeviceAddress address=r.u64();return r.ok()&&r.remaining()==0?address:0;}
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
VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice handle,VkBuffer buffer,const VkAllocationCallbacks*){if(buffer==VK_NULL_HANDLE)return;RemoteDevice*device=to_device(handle);const uint64_t id=id_from_handle(buffer);{std::lock_guard<std::mutex>lock(device->metadata_mutex);device->buffer_bindings.erase(id);}Writer request;request.handle(device->remote_id);request.handle(id);device->instance->connection.send_oneway(Opcode::vkDestroyBuffer,request);}

SIMPLE_CREATE(CreateImage, VkImage, VkImageCreateInfo, write_ImageCreateInfo, vkCreateImage)
VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice handle,VkImage image,const VkAllocationCallbacks*){if(image==VK_NULL_HANDLE)return;RemoteDevice*device=to_device(handle);const uint64_t id=id_from_handle(image);{std::lock_guard<std::mutex>lock(device->metadata_mutex);device->image_bindings.erase(id);}Writer request;request.handle(device->remote_id);request.handle(id);device->instance->connection.send_oneway(Opcode::vkDestroyImage,request);}

VKAPI_ATTR VkResult VKAPI_CALL CreateBufferView(VkDevice handle,const VkBufferViewCreateInfo*info,const VkAllocationCallbacks*,VkBufferView*out){if(!info||!out)return VK_ERROR_INITIALIZATION_FAILED;RemoteDevice*device=to_device(handle);Writer request;request.handle(device->remote_id);write_BufferViewCreateInfo(request,*info);std::vector<char>reply;if(!device->instance->connection.round_trip(Opcode::vkCreateBufferView,request,&reply))return VK_ERROR_INITIALIZATION_FAILED;Reader r=payload_reader(reply);VkResult result=static_cast<VkResult>(r.i32());uint64_t id=r.handle();if(result==VK_SUCCESS&&r.ok()){*out=handle_from_id<VkBufferView>(id);std::lock_guard<std::mutex>lock(device->metadata_mutex);device->buffer_view_buffers[id]=id_from_handle(info->buffer);}return r.ok()?result:VK_ERROR_INITIALIZATION_FAILED;}
VKAPI_ATTR void VKAPI_CALL DestroyBufferView(VkDevice handle,VkBufferView view,const VkAllocationCallbacks*){if(view==VK_NULL_HANDLE)return;RemoteDevice*device=to_device(handle);uint64_t id=id_from_handle(view);{std::lock_guard<std::mutex>lock(device->metadata_mutex);device->buffer_view_buffers.erase(id);}Writer request;request.handle(device->remote_id);request.handle(id);device->instance->connection.send_oneway(Opcode::vkDestroyBufferView,request);}

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

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(VkDevice handle,
    VkDescriptorSetLayout layout, const VkAllocationCallbacks*) {
    if (layout == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    const uint64_t id = id_from_handle(layout);
    {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->descriptor_layouts.erase(id);
    }
    Writer request; request.handle(device->remote_id); request.handle(id);
    device->instance->connection.send_oneway(Opcode::vkDestroyDescriptorSetLayout, request);
}

SIMPLE_CREATE(CreateDescriptorPool, VkDescriptorPool, VkDescriptorPoolCreateInfo,
              write_DescriptorPoolCreateInfo, vkCreateDescriptorPool)
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(VkDevice handle, VkDescriptorPool pool,
                                                  const VkAllocationCallbacks*) {
    if (pool == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    const uint64_t pool_id = id_from_handle(pool);
    {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        for (auto it = device->descriptor_set_pools.begin();
             it != device->descriptor_set_pools.end();) {
            if (it->second != pool_id) { ++it; continue; }
            device->descriptor_set_layouts.erase(it->first);
            device->descriptor_resources.erase(it->first);
            it = device->descriptor_set_pools.erase(it);
        }
    }
    Writer request; request.handle(device->remote_id); request.handle(pool_id);
    device->instance->connection.send_oneway(Opcode::vkDestroyDescriptorPool, request);
}

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
    if (result == VK_SUCCESS) {
        *pView = handle_from_id<VkImageView>(id);
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->image_view_images[id] = id_from_handle(pCreateInfo->image);
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice handle, VkImageView view,
                                            const VkAllocationCallbacks*) {
    if (view == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    const uint64_t id = id_from_handle(view);
    {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->image_view_images.erase(id);
    }
    Writer request;
    request.handle(device->remote_id);
    request.handle(id);
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
    if (result == VK_SUCCESS) {
        *pFramebuffer = handle_from_id<VkFramebuffer>(id);
        FramebufferMetadata metadata;
        metadata.imageless = (pCreateInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;
        if (!metadata.imageless) {
            for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
                metadata.image_views.push_back(id_from_handle(pCreateInfo->pAttachments[i]));
        }
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->framebuffers[id] = std::move(metadata);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(VkDevice handle, VkFramebuffer framebuffer,
                                              const VkAllocationCallbacks*) {
    if (framebuffer == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    const uint64_t id = id_from_handle(framebuffer);
    {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->framebuffers.erase(id);
    }
    Writer request;
    request.handle(device->remote_id);
    request.handle(id);
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
    if (!r.ok() || r.remaining() != 0) return VK_ERROR_DEVICE_LOST;
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t id = id_from_handle(pSets[i]);
            device->descriptor_set_layouts.erase(id);
            device->descriptor_set_pools.erase(id);
            device->descriptor_resources.erase(id);
        }
    }
    return result;
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
    if (!r.ok() || r.remaining() != 0) return VK_ERROR_DEVICE_LOST;
    if (result == VK_SUCCESS) {
        const uint64_t pool_id = id_from_handle(pool);
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        for (auto it = device->descriptor_set_pools.begin();
             it != device->descriptor_set_pools.end();) {
            if (it->second != pool_id) { ++it; continue; }
            device->descriptor_set_layouts.erase(it->first);
            device->descriptor_resources.erase(it->first);
            it = device->descriptor_set_pools.erase(it);
        }
    }
    return result;
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
    if (!pCreateInfo || !pLayout || pCreateInfo->pNext ||
        (pCreateInfo->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT))
        return VK_ERROR_FEATURE_NOT_PRESENT;
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
    if (result == VK_SUCCESS) {
        *pLayout = handle_from_id<VkDescriptorSetLayout>(id);
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        auto& bindings = device->descriptor_layouts[id];
        bindings.reserve(pCreateInfo->bindingCount);
        for (uint32_t i = 0; i < pCreateInfo->bindingCount; ++i) {
            bindings.push_back(
                {pCreateInfo->pBindings[i].binding,
                 pCreateInfo->pBindings[i].descriptorCount});
        }
        std::sort(bindings.begin(), bindings.end(),
                  [](const auto& a, const auto& b) { return a.binding < b.binding; });
    }
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
    std::vector<std::pair<uint64_t, uint64_t>> allocated;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t id = r.handle();
        pSets[i] = id ? handle_from_id<VkDescriptorSet>(id) : VK_NULL_HANDLE;
        if (id) allocated.emplace_back(id, id_from_handle(pInfo->pSetLayouts[i]));
    }
    if (r.ok() && r.remaining() == 0 && result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        for (const auto& item : allocated) {
            const auto layout = device->descriptor_layouts.find(item.second);
            if (layout != device->descriptor_layouts.end())
                device->descriptor_set_layouts[item.first] = layout->second;
            device->descriptor_set_pools[item.first] = id_from_handle(pInfo->descriptorPool);
        }
    }
    return r.ok() && r.remaining() == 0 ? result : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(VkDevice handle,const VkDescriptorUpdateTemplateCreateInfo* info,const VkAllocationCallbacks*,VkDescriptorUpdateTemplate* out){if(!info||!out)return VK_ERROR_INITIALIZATION_FAILED;RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);if(!write_DescriptorUpdateTemplateCreateInfo(w,*info))return VK_ERROR_INITIALIZATION_FAILED;std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkCreateDescriptorUpdateTemplate,w,&reply))return VK_ERROR_INITIALIZATION_FAILED;Reader r=payload_reader(reply);VkResult result=static_cast<VkResult>(r.i32());uint64_t id=r.handle();if(result==VK_SUCCESS&&r.ok()){*out=handle_from_id<VkDescriptorUpdateTemplate>(id);std::lock_guard<std::mutex>lock(d->metadata_mutex);d->descriptor_templates[id]={info->templateType,std::vector<VkDescriptorUpdateTemplateEntry>(info->pDescriptorUpdateEntries,info->pDescriptorUpdateEntries+info->descriptorUpdateEntryCount)};}return result;}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(VkDevice handle,VkDescriptorUpdateTemplate object,const VkAllocationCallbacks*){if(!object)return;RemoteDevice*d=to_device(handle);uint64_t id=id_from_handle(object);{std::lock_guard<std::mutex>lock(d->metadata_mutex);d->descriptor_templates.erase(id);}Writer w;w.handle(d->remote_id);w.handle(id);d->instance->connection.send_oneway(Opcode::vkDestroyDescriptorUpdateTemplate,w);}
VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate(VkDevice handle,VkDescriptorSet set,VkDescriptorUpdateTemplate object,const void* data){RemoteDevice*d=to_device(handle);if(!data)return;DescriptorTemplateMetadata metadata;std::vector<DescriptorBindingMetadata> layout;uint64_t set_id=id_from_handle(set);{std::lock_guard<std::mutex>lock(d->metadata_mutex);auto it=d->descriptor_templates.find(id_from_handle(object));if(it==d->descriptor_templates.end())return;metadata=it->second;auto set_layout=d->descriptor_set_layouts.find(set_id);if(set_layout!=d->descriptor_set_layouts.end())layout=set_layout->second;}Writer w;w.handle(d->remote_id);std::unordered_map<uint64_t,DescriptorResource> updates;w.handle(set_id);w.handle(id_from_handle(object));w.u32(static_cast<uint32_t>(metadata.entries.size()));for(const auto&e:metadata.entries){DescriptorSlotCursor cursor=descriptor_cursor(layout.empty()?nullptr:&layout,e.dstBinding,e.dstArrayElement);w.u32(e.descriptorCount);w.i32(e.descriptorType);for(uint32_t i=0;i<e.descriptorCount;++i){const char*p=static_cast<const char*>(data)+e.offset+i*e.stride;DescriptorResource resource;resource.type=e.descriptorType;switch(e.descriptorType){case VK_DESCRIPTOR_TYPE_SAMPLER:case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:{auto*x=reinterpret_cast<const VkDescriptorImageInfo*>(p);w.handle(id_from_handle(x->sampler));w.handle(id_from_handle(x->imageView));w.i32(x->imageLayout);if(e.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)resource.resource_id=id_from_handle(x->imageView);break;}case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:{auto view=*reinterpret_cast<const VkBufferView*>(p);w.handle(id_from_handle(view));if(e.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)resource.resource_id=id_from_handle(view);break;}default:{auto*x=reinterpret_cast<const VkDescriptorBufferInfo*>(p);w.handle(id_from_handle(x->buffer));w.u64(x->offset);w.u64(x->range);if(e.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||e.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)resource.resource_id=id_from_handle(x->buffer);break;}}uint64_t key=descriptor_slot_key(e.dstBinding,e.dstArrayElement+i);cursor.current(&key);updates[key]=resource;cursor.advance();}}{std::lock_guard<std::mutex>lock(d->metadata_mutex);auto&resources=d->descriptor_resources[set_id];for(const auto&update:updates){if(update.second.resource_id)resources[update.first]=update.second;else resources.erase(update.first);}}d->instance->connection.send_oneway(Opcode::vkUpdateDescriptorSetWithTemplate,w);}
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
    std::unordered_map<uint64_t, std::vector<DescriptorBindingMetadata>> set_layouts;
    {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        set_layouts = device->descriptor_set_layouts;
    }
    std::unordered_map<uint64_t,
        std::unordered_map<uint64_t, DescriptorResource>> touched_resources;
    request.handle(device->remote_id);
    request.u32(writeCount);
    for (uint32_t i = 0; i < writeCount; ++i) {
        if (!write_WriteDescriptorSet(request, pWrites[i])) return;
        const VkWriteDescriptorSet& write = pWrites[i];
        const uint64_t set_id = id_from_handle(write.dstSet);
        auto& resources = touched_resources[set_id];
        const auto layout = set_layouts.find(set_id);
        DescriptorSlotCursor cursor = descriptor_cursor(
            layout == set_layouts.end() ? nullptr : &layout->second,
            write.dstBinding, write.dstArrayElement);
        for (uint32_t j = 0; j < write.descriptorCount; ++j) {
            DescriptorResource resource;
            resource.type = write.descriptorType;
            if (write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                resource.resource_id = id_from_handle(write.pBufferInfo[j].buffer);
            else if (write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                resource.resource_id = id_from_handle(write.pImageInfo[j].imageView);
            else if (write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
                resource.resource_id = id_from_handle(write.pTexelBufferView[j]);
            uint64_t key = descriptor_slot_key(write.dstBinding,
                                               write.dstArrayElement + j);
            cursor.current(&key);
            resources[key] = resource;
            cursor.advance();
        }
    }
    request.u32(copyCount);
    for (uint32_t i = 0; i < copyCount; ++i) {
        if (pCopies[i].srcSet == VK_NULL_HANDLE || pCopies[i].dstSet == VK_NULL_HANDLE) return;
        write_CopyDescriptorSet(request, pCopies[i]);
        const VkCopyDescriptorSet& copy = pCopies[i];
        std::unordered_map<uint64_t, DescriptorResource> source_snapshot;
        const uint64_t source_id = id_from_handle(copy.srcSet);
        {
            std::lock_guard<std::mutex> metadata_lock(device->metadata_mutex);
            const auto source = device->descriptor_resources.find(source_id);
            if (source != device->descriptor_resources.end()) source_snapshot = source->second;
        }
        if (auto touched = touched_resources.find(source_id); touched != touched_resources.end()) {
            for (const auto& update : touched->second) {
                if (update.second.resource_id) source_snapshot[update.first] = update.second;
                else source_snapshot.erase(update.first);
            }
        }
        const auto source = &source_snapshot;
        const auto source_layout = set_layouts.find(source_id);
        DescriptorSlotCursor source_cursor = descriptor_cursor(
            source_layout == set_layouts.end()
                ? nullptr : &source_layout->second,
            copy.srcBinding, copy.srcArrayElement);
        std::vector<DescriptorResource> snapshot;
        snapshot.reserve(copy.descriptorCount);
        for (uint32_t j = 0; j < copy.descriptorCount; ++j) {
            uint64_t key = descriptor_slot_key(copy.srcBinding, copy.srcArrayElement + j);
            source_cursor.current(&key);
            const auto resource = source->find(key);
            snapshot.push_back(resource == source->end() ? DescriptorResource{}
                                                                : resource->second);
            source_cursor.advance();
        }
        const uint64_t destination_id = id_from_handle(copy.dstSet);
        auto& destination = touched_resources[destination_id];
        const auto destination_layout = set_layouts.find(destination_id);
        DescriptorSlotCursor destination_cursor = descriptor_cursor(
            destination_layout == set_layouts.end()
                ? nullptr : &destination_layout->second,
            copy.dstBinding, copy.dstArrayElement);
        for (uint32_t j = 0; j < copy.descriptorCount; ++j) {
            uint64_t key = descriptor_slot_key(copy.dstBinding,
                                               copy.dstArrayElement + j);
            destination_cursor.current(&key);
            destination[key] = snapshot[j];
            destination_cursor.advance();
        }
    }
    {
        std::lock_guard<std::mutex> metadata_lock(device->metadata_mutex);
        for (auto& touched : touched_resources) {
            auto& destination = device->descriptor_resources[touched.first];
            for (const auto& update : touched.second) {
                if (update.second.resource_id) destination[update.first] = update.second;
                else destination.erase(update.first);
            }
        }
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
        D(QueueSubmit2),
        {"vkQueueSubmit2KHR", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit2)},
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
