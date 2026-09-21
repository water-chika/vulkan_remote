// Client-side memory allocation and mapped ranges.
//
// vkMapMemory promises a pointer the caller can dereference, and no socket
// can deliver one, so this hands back an ordinary shadow allocation and
// pushes its contents to the server before anything can observe them there
// (see remote_objects.hpp's MappedRange / RemoteDevice::flush_mapped /
// RemoteDevice::download_mapped, implemented in remote_objects.cpp). Writes
// are the only direction that works: a program that maps memory to READ what
// the GPU wrote would see stale bytes.

#include <string.h>

#include <cstdlib>
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

Reader payload_reader(const std::vector<char>& reply) {
    Reader r(reply.data(), reply.size());
    r.u32();
    return r;
}

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

}  // namespace

const DeviceEntry* get_memory_entries(size_t* count) {
    static const DeviceEntry kEntries[] = {
#define D(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name)}
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
#undef D
    };
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

}  // namespace remoting
