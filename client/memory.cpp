// Client-side memory allocation and mapped shadow ranges.

#include <string.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "entry_table.hpp"
#include "marshal.hpp"
#include "memory_contract.hpp"
#include "remote_objects.hpp"
#include "wire.hpp"

namespace remoting {
namespace {

RemoteDevice* to_device(VkDevice handle) { return reinterpret_cast<RemoteDevice*>(handle); }

Reader payload_reader(const std::vector<char>& reply) {
    Reader r(reply.data(), reply.size());
    r.u32();
    return r;
}

bool resolve_allocation_range(VkDeviceSize allocation_size, VkDeviceSize offset,
                              VkDeviceSize requested, VkDeviceSize* resolved) {
    if (offset >= allocation_size) return false;
    const VkDeviceSize available = allocation_size - offset;
    if (requested == VK_WHOLE_SIZE) {
        *resolved = available;
        return true;
    }
    if (requested == 0 || requested > available) return false;
    *resolved = requested;
    return true;
}

bool validate_explicit_ranges(RemoteDevice* device, const VkMappedMemoryRange* ranges,
                              uint32_t count) {
    std::lock_guard<std::mutex> lock(device->mapped_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        const auto allocation = device->memories.find(id_from_handle(ranges[i].memory));
        if (allocation == device->memories.end()) return false;
        VkDeviceSize size = 0;
        if (!resolve_allocation_range(allocation->second.size, ranges[i].offset,
                                      ranges[i].size, &size)) return false;
        if (allocation->second.emulated_coherent) continue;
        const VkDeviceSize atom = allocation->second.non_coherent_atom_size;
        if (atom == 0 || ranges[i].offset % atom != 0) return false;
        const VkDeviceSize end = ranges[i].offset + size;
        if (end != allocation->second.size && size % atom != 0) return false;
    }
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice handle,
                                              const VkMemoryAllocateInfo* pAllocateInfo,
                                              const VkAllocationCallbacks*,
                                              VkDeviceMemory* pMemory) {
    RemoteDevice* device = to_device(handle);
    if (!pAllocateInfo || !pMemory) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
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
    const VkMemoryPropertyFlags server_flags =
        public_memory_property_flags(r.u32(), device->instance->capabilities);
    const VkDeviceSize server_alignment = r.u64();
    const VkDeviceSize atom_size = r.u64();
    if (!r.ok() || atom_size == 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (result == VK_SUCCESS) {
        *pMemory = handle_from_id<VkDeviceMemory>(id);
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        device->memories[id] = MemoryAllocation{
            pAllocateInfo->allocationSize, pAllocateInfo->memoryTypeIndex, server_flags, atom_size,
            (server_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0};
        if (server_alignment != 0) device->map_alignment = server_alignment;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice handle, VkDeviceMemory memory,
                                      const VkAllocationCallbacks*) {
    if (memory == VK_NULL_HANDLE) return;
    RemoteDevice* device = to_device(handle);
    const uint64_t id = id_from_handle(memory);
    {
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        for (auto it = device->mapped.begin(); it != device->mapped.end(); ++it) {
            if (it->memory_id == id) {
                std::free(it->allocation_base);
                device->mapped.erase(it);
                break;
            }
        }
        device->memories.erase(id);
    }
    Writer request;
    request.handle(device->remote_id);
    request.handle(id);
    device->instance->connection.send_oneway(Opcode::vkFreeMemory, request);
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice handle, VkDeviceMemory memory,
                                         VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags,
                                         void** ppData) {
    if (!ppData || memory == VK_NULL_HANDLE) return VK_ERROR_MEMORY_MAP_FAILED;
    RemoteDevice* device = to_device(handle);
    const uint64_t memory_id = id_from_handle(memory);
    VkDeviceSize real_size = 0;
    bool emulated_coherent = false;
    {
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        const auto allocation = device->memories.find(memory_id);
        if (allocation == device->memories.end() ||
            !(allocation->second.property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
            !resolve_allocation_range(allocation->second.size, offset, size, &real_size)) {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
        for (const MappedRange& mapping : device->mapped) {
            if (mapping.memory_id == memory_id) return VK_ERROR_MEMORY_MAP_FAILED;
        }
        emulated_coherent = allocation->second.emulated_coherent;
    }

    const size_t alignment = static_cast<size_t>(device->map_alignment > alignof(void*)
                                                    ? device->map_alignment : alignof(void*));
    if (real_size > static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max()) ||
        real_size > std::numeric_limits<size_t>::max() - (alignment - 1)) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    void* base = std::malloc(static_cast<size_t>(real_size) + alignment - 1);
    if (!base) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const uintptr_t address = reinterpret_cast<uintptr_t>(base);
    const uintptr_t residue = static_cast<uintptr_t>(offset) & (alignment - 1);
    const uintptr_t adjustment = (residue - address) & (alignment - 1);
    void* shadow = reinterpret_cast<void*>(address + adjustment);

    {
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        MappedRange mapping{memory_id, offset, real_size, base, shadow};
        mapping.emulated_coherent = emulated_coherent;
        mapping.transfer_ready = !emulated_coherent;
        if (emulated_coherent) {
            try {
                mapping.baseline.resize(static_cast<size_t>(real_size));
            } catch (const std::bad_alloc&) {
                std::free(base);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        device->mapped.push_back(std::move(mapping));
    }

    VkMappedMemoryRange initial{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    initial.memory = memory;
    initial.offset = offset;
    initial.size = real_size;
    if (emulated_coherent && device->download_mapped(&initial, 1) != VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        for (auto it = device->mapped.begin(); it != device->mapped.end(); ++it) {
            if (it->memory_id == memory_id) {
                std::free(it->allocation_base);
                device->mapped.erase(it);
                break;
            }
        }
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    if (emulated_coherent) {
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        for (MappedRange& mapping : device->mapped) {
            if (mapping.memory_id == memory_id) {
                std::memcpy(mapping.baseline.data(), mapping.shadow,
                            static_cast<size_t>(mapping.size));
                mapping.transfer_ready = true;
                break;
            }
        }
    }

    *ppData = shadow;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice handle, VkDeviceMemory memory) {
    RemoteDevice* device = to_device(handle);
    const uint64_t memory_id = id_from_handle(memory);
    VkMappedMemoryRange coherent_range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    {
        std::lock_guard<std::mutex> lock(device->mapped_mutex);
        for (const MappedRange& mapping : device->mapped) {
            if (mapping.memory_id == memory_id && mapping.emulated_coherent) {
                coherent_range.memory = memory;
                coherent_range.offset = mapping.offset;
                coherent_range.size = mapping.size;
                break;
            }
        }
    }
    if (coherent_range.memory != VK_NULL_HANDLE)
        (void)device->flush_mapped(&coherent_range, 1);
    std::lock_guard<std::mutex> lock(device->mapped_mutex);
    for (auto it = device->mapped.begin(); it != device->mapped.end(); ++it) {
        if (it->memory_id == memory_id) {
            std::free(it->allocation_base);
            device->mapped.erase(it);
            break;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL FlushMappedMemoryRanges(VkDevice handle, uint32_t count,
                                                       const VkMappedMemoryRange* ranges) {
    if (count && !ranges) return VK_ERROR_MEMORY_MAP_FAILED;
    RemoteDevice* device = to_device(handle);
    if (!validate_explicit_ranges(device, ranges, count)) return VK_ERROR_MEMORY_MAP_FAILED;
    return device->flush_mapped(ranges, count);
}

VKAPI_ATTR VkResult VKAPI_CALL InvalidateMappedMemoryRanges(VkDevice handle, uint32_t count,
                                                            const VkMappedMemoryRange* ranges) {
    if (count && !ranges) return VK_ERROR_MEMORY_MAP_FAILED;
    RemoteDevice* device = to_device(handle);
    if (!validate_explicit_ranges(device, ranges, count)) return VK_ERROR_MEMORY_MAP_FAILED;
    return device->download_mapped(ranges, count);
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
    if (!device->instance->connection.round_trip(Opcode::vkBindBufferMemory, request, &reply)) return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    if (r.ok() && result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->buffer_bindings[id_from_handle(buffer)] = {id_from_handle(memory), offset};
    }
    return r.ok() ? result : VK_ERROR_DEVICE_LOST;
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
    if (!device->instance->connection.round_trip(Opcode::vkBindImageMemory, request, &reply)) return VK_ERROR_DEVICE_LOST;
    Reader r = payload_reader(reply);
    const VkResult result = static_cast<VkResult>(r.i32());
    if (r.ok() && result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(device->metadata_mutex);
        device->image_bindings[id_from_handle(image)] = {id_from_handle(memory), offset};
    }
    return r.ok() ? result : VK_ERROR_DEVICE_LOST;
}

void read_memory_requirements(Reader& r, VkMemoryRequirements* out) {
    out->size = r.u64(); out->alignment = r.u64(); out->memoryTypeBits = r.u32();
}

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(VkDevice handle, VkBuffer buffer,
                                                       VkMemoryRequirements* out) {
    memset(out, 0, sizeof(*out)); RemoteDevice* device = to_device(handle); Writer request;
    request.handle(device->remote_id); request.handle(id_from_handle(buffer)); std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetBufferMemoryRequirements, request, &reply)) return;
    Reader r = payload_reader(reply); read_memory_requirements(r, out);
}

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice handle, VkImage image,
                                                      VkMemoryRequirements* out) {
    memset(out, 0, sizeof(*out)); RemoteDevice* device = to_device(handle); Writer request;
    request.handle(device->remote_id); request.handle(id_from_handle(image)); std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetImageMemoryRequirements, request, &reply)) return;
    Reader r = payload_reader(reply); read_memory_requirements(r, out);
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice handle,uint32_t count,const VkBindBufferMemoryInfo* infos){
    if(count>kMaxArrayElements||(count&&!infos))return VK_ERROR_INITIALIZATION_FAILED;RemoteDevice* d=to_device(handle);Writer w;w.handle(d->remote_id);w.u32(count);
    for(uint32_t i=0;i<count;++i){if(infos[i].sType!=VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO||infos[i].pNext)return VK_ERROR_FEATURE_NOT_PRESENT;w.handle(id_from_handle(infos[i].buffer));w.handle(id_from_handle(infos[i].memory));w.u64(infos[i].memoryOffset);}
    std::vector<char> reply;if(!d->instance->connection.round_trip(Opcode::vkBindBufferMemory2,w,&reply))return VK_ERROR_DEVICE_LOST;Reader r=payload_reader(reply);auto result=static_cast<VkResult>(r.i32());if(r.ok()&&result==VK_SUCCESS){std::lock_guard<std::mutex>lock(d->metadata_mutex);for(uint32_t i=0;i<count;++i)d->buffer_bindings[id_from_handle(infos[i].buffer)]={id_from_handle(infos[i].memory),infos[i].memoryOffset};}return r.ok()?result:VK_ERROR_DEVICE_LOST;
}
VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2(VkDevice handle,uint32_t count,const VkBindImageMemoryInfo* infos){
    if(count>kMaxArrayElements||(count&&!infos))return VK_ERROR_INITIALIZATION_FAILED;RemoteDevice* d=to_device(handle);Writer w;w.handle(d->remote_id);w.u32(count);
    for(uint32_t i=0;i<count;++i){if(infos[i].sType!=VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO)return VK_ERROR_INITIALIZATION_FAILED;w.handle(id_from_handle(infos[i].image));w.handle(id_from_handle(infos[i].memory));w.u64(infos[i].memoryOffset);if(!write_pnext_chain(w,PNextContext::BindImageMemory2,infos[i].pNext))return VK_ERROR_FEATURE_NOT_PRESENT;}
    std::vector<char> reply;if(!d->instance->connection.round_trip(Opcode::vkBindImageMemory2,w,&reply))return VK_ERROR_DEVICE_LOST;Reader r=payload_reader(reply);auto result=static_cast<VkResult>(r.i32());if(r.ok()&&result==VK_SUCCESS){std::lock_guard<std::mutex>lock(d->metadata_mutex);for(uint32_t i=0;i<count;++i)d->image_bindings[id_from_handle(infos[i].image)]={id_from_handle(infos[i].memory),infos[i].memoryOffset};}return r.ok()?result:VK_ERROR_DEVICE_LOST;
}
VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements2(VkDevice handle,const VkBufferMemoryRequirementsInfo2* info,VkMemoryRequirements2* out){
    if(!info||!out)return;RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);w.handle(id_from_handle(info->buffer));if(!write_pnext_output_topology(w,PNextContext::MemoryRequirements2,out->pNext))return;std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkGetBufferMemoryRequirements2,w,&reply))return;Reader r=payload_reader(reply);read_memory_requirements(r,&out->memoryRequirements);apply_pnext_output_values(r,PNextContext::MemoryRequirements2,out->pNext);
}
VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2(VkDevice handle,const VkImageMemoryRequirementsInfo2* info,VkMemoryRequirements2* out){
    if(!info||!out)return;RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);w.handle(id_from_handle(info->image));if(!write_pnext_chain(w,PNextContext::MemoryRequirements2,info->pNext)||!write_pnext_output_topology(w,PNextContext::MemoryRequirements2,out->pNext))return;std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkGetImageMemoryRequirements2,w,&reply))return;Reader r=payload_reader(reply);read_memory_requirements(r,&out->memoryRequirements);apply_pnext_output_values(r,PNextContext::MemoryRequirements2,out->pNext);
}
VKAPI_ATTR void VKAPI_CALL GetImageSparseMemoryRequirements2(VkDevice handle,const VkImageSparseMemoryRequirementsInfo2* info,uint32_t* count,VkSparseImageMemoryRequirements2* out){
    if(!info||!count)return;RemoteDevice*d=to_device(handle);Writer w;w.handle(d->remote_id);w.handle(id_from_handle(info->image));std::vector<char>reply;if(!d->instance->connection.round_trip(Opcode::vkGetImageSparseMemoryRequirements2,w,&reply)){*count=0;return;}Reader r=payload_reader(reply);uint32_t n=r.u32();if(n>kMaxArrayElements){*count=0;return;}std::vector<VkSparseImageMemoryRequirements> vals(n);for(auto&v:vals){v.formatProperties.aspectMask=r.u32();v.formatProperties.imageGranularity.width=r.u32();v.formatProperties.imageGranularity.height=r.u32();v.formatProperties.imageGranularity.depth=r.u32();v.formatProperties.flags=r.u32();v.imageMipTailFirstLod=r.u32();v.imageMipTailSize=r.u64();v.imageMipTailOffset=r.u64();v.imageMipTailStride=r.u64();}if(!r.ok()){*count=0;return;}if(!out){*count=n;return;}uint32_t m=*count<n?*count:n;for(uint32_t i=0;i<m;++i)out[i].memoryRequirements=vals[i];*count=m;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceMemoryCommitment(VkDevice handle, VkDeviceMemory memory,
                                                     VkDeviceSize* pCommitted) {
    if (!pCommitted) return; *pCommitted = 0;
    RemoteDevice* device = to_device(handle); Writer request;
    request.handle(device->remote_id); request.handle(id_from_handle(memory));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetDeviceMemoryCommitment, request, &reply)) return;
    Reader r = payload_reader(reply); *pCommitted = r.u64(); if (!r.ok()) *pCommitted = 0;
}

VKAPI_ATTR void VKAPI_CALL GetImageSparseMemoryRequirements(
    VkDevice handle, VkImage image, uint32_t* pCount, VkSparseImageMemoryRequirements* pRequirements) {
    if (!pCount) return; RemoteDevice* device = to_device(handle); Writer request;
    request.handle(device->remote_id); request.handle(id_from_handle(image));
    std::vector<char> reply;
    if (!device->instance->connection.round_trip(Opcode::vkGetImageSparseMemoryRequirements, request, &reply)) {
        *pCount = 0; return;
    }
    Reader r = payload_reader(reply); const uint32_t count = r.u32();
    std::vector<VkSparseImageMemoryRequirements> values(count);
    if (count > kMaxArrayElements) { *pCount = 0; return; }
    for (uint32_t i = 0; i < count; ++i) {
        auto& v = values[i]; v.formatProperties.aspectMask = r.u32();
        v.formatProperties.imageGranularity.width = r.u32();
        v.formatProperties.imageGranularity.height = r.u32();
        v.formatProperties.imageGranularity.depth = r.u32();
        v.formatProperties.flags = r.u32(); v.imageMipTailFirstLod = r.u32();
        v.imageMipTailSize = r.u64(); v.imageMipTailOffset = r.u64();
        v.imageMipTailStride = r.u64();
    }
    if (!r.ok()) { *pCount = 0; return; }
    if (!pRequirements) { *pCount = count; return; }
    const uint32_t n = *pCount < count ? *pCount : count;
    for (uint32_t i = 0; i < n; ++i) pRequirements[i] = values[i]; *pCount = n;
}

}  // namespace

const DeviceEntry* get_memory_entries(size_t* count) {
    static const DeviceEntry entries[] = {
#define D(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name)}
        D(AllocateMemory), D(FreeMemory), D(MapMemory), D(UnmapMemory),
        D(FlushMappedMemoryRanges), D(InvalidateMappedMemoryRanges),
        D(BindBufferMemory), D(BindImageMemory), D(BindBufferMemory2), D(BindImageMemory2),
        {"vkBindBufferMemory2KHR", reinterpret_cast<PFN_vkVoidFunction>(BindBufferMemory2)},
        {"vkBindImageMemory2KHR", reinterpret_cast<PFN_vkVoidFunction>(BindImageMemory2)},
        D(GetBufferMemoryRequirements), D(GetImageMemoryRequirements), D(GetDeviceMemoryCommitment),
        D(GetImageSparseMemoryRequirements), D(GetBufferMemoryRequirements2), D(GetImageMemoryRequirements2),
        D(GetImageSparseMemoryRequirements2),
        {"vkGetBufferMemoryRequirements2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetBufferMemoryRequirements2)},
        {"vkGetImageMemoryRequirements2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetImageMemoryRequirements2)},
        {"vkGetImageSparseMemoryRequirements2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetImageSparseMemoryRequirements2)},
#undef D
    };
    *count = sizeof(entries) / sizeof(entries[0]); return entries;
}

}  // namespace remoting
