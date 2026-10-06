// Server-side memory allocation, mapped transfer, binding, and requirements.

#include <cstring>
#include <limits>
#include <vector>

#include <vulkan/vulkan.h>

#include "marshal.hpp"
#include "session.hpp"

namespace {

using remoting::Arena;
using remoting::ServerMemory;
using remoting::Session;
using remoting::Status;
using remoting::mark_oneway_error;

constexpr uint64_t kMaxMappedChunk = 64ull * 1024ull * 1024ull - 64ull;

uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

bool validate_transfer(Session& c, uint64_t device_id, uint64_t memory_id,
                       uint64_t offset, uint64_t size, ServerMemory** metadata) {
    const auto it = c.tables.memory_metadata.find(memory_id);
    if (c.tables.devices.get(device_id) == VK_NULL_HANDLE ||
        it == c.tables.memory_metadata.end() || it->second.owner != c.tables.devices.get(device_id) ||
        !(it->second.property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || size == 0 ||
        size > kMaxMappedChunk || offset > it->second.size || size > it->second.size - offset) {
        return false;
    }
    if (!(it->second.property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        const VkDeviceSize atom = it->second.non_coherent_atom_size;
        if (atom == 0 || offset % atom != 0) return false;
        if (offset + size != it->second.size && size % atom != 0) return false;
    }
    *metadata = &it->second;
    return true;
}

VkResult map_transfer(ServerMemory& memory, uint64_t offset, uint64_t size,
                      const void* upload, std::vector<char>* download) {
    const uint64_t native_alignment =
        memory.map_alignment > memory.non_coherent_atom_size ? memory.map_alignment
                                                            : memory.non_coherent_atom_size;
    const uint64_t map_offset = align_down(offset, native_alignment);
    void* mapped = nullptr;
    VkResult result = vkMapMemory(memory.owner, memory.handle, map_offset, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) return result;

    const bool coherent =
        (memory.property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMappedMemoryRange native_range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    native_range.memory = memory.handle;
    if (!coherent) {
        native_range.offset = align_down(offset, memory.non_coherent_atom_size);
        const uint64_t requested_end = offset + size;
        if (requested_end == memory.size) {
            native_range.size = VK_WHOLE_SIZE;
        } else {
            const uint64_t atom = memory.non_coherent_atom_size;
            const uint64_t rounded = (requested_end + atom - 1) & ~(atom - 1);
            native_range.size = rounded - native_range.offset;
        }
    }

    char* bytes = static_cast<char*>(mapped) + (offset - map_offset);
    if (upload) {
        std::memcpy(bytes, upload, static_cast<size_t>(size));
        if (!coherent) result = vkFlushMappedMemoryRanges(memory.owner, 1, &native_range);
    } else {
        if (!coherent) result = vkInvalidateMappedMemoryRanges(memory.owner, 1, &native_range);
        if (result == VK_SUCCESS) {
            download->assign(bytes, bytes + static_cast<size_t>(size));
        }
    }
    vkUnmapMemory(memory.owner, memory.handle);
    return result;
}

void handle_AllocateMemory(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkMemoryAllocateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_MemoryAllocateInfo(c.reader, arena, c.tables, &info)) {
        c.reply_status(Status::DecodeError);
        return;
    }
    const auto physical = c.tables.device_physical_devices.find(device);
    if (physical == c.tables.device_physical_devices.end()) {
        c.reply_status(Status::DecodeError);
        return;
    }
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical->second, &memory_properties);
    vkGetPhysicalDeviceProperties(physical->second, &properties);
    if (info.memoryTypeIndex >= memory_properties.memoryTypeCount) {
        c.reply_status(Status::DecodeError);
        return;
    }

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = vkAllocateMemory(device, &info, nullptr, &memory);
    uint64_t id = 0;
    const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[info.memoryTypeIndex].propertyFlags;
    const VkDeviceSize map_alignment = properties.limits.minMemoryMapAlignment;
    const VkDeviceSize atom_size = properties.limits.nonCoherentAtomSize;
    if (result == VK_SUCCESS) {
        id = c.tables.memories.add(memory);
        c.tables.memory_metadata[id] = ServerMemory{memory, device, info.allocationSize, flags,
                                                    map_alignment, atom_size};
    }
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(id);
    c.writer.u32(flags);
    c.writer.u64(map_alignment);
    c.writer.u64(atom_size);
    c.reply();
}

void handle_FreeMemory(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t id = c.reader.handle();
    const auto metadata = c.tables.memory_metadata.find(id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE || metadata == c.tables.memory_metadata.end() ||
        metadata->second.owner != device) {
        mark_oneway_error(c);
        return;
    }
    VkDeviceMemory memory = c.tables.memories.take(id);
    c.tables.memory_metadata.erase(metadata);
    vkFreeMemory(device, memory, nullptr);
}

void handle_FlushMappedMemory(Session& c) {
    const uint64_t device_id = c.reader.handle();
    const uint64_t memory_id = c.reader.handle();
    const uint64_t offset = c.reader.u64();
    const uint64_t size = c.reader.u64();
    std::vector<char> bytes;
    ServerMemory* memory = nullptr;
    if (!c.reader.bytes(&bytes, static_cast<size_t>(kMaxMappedChunk)) || bytes.size() != size ||
        !c.reader.ok() || !validate_transfer(c, device_id, memory_id, offset, size, &memory)) {
        c.reply_status(Status::DecodeError);
        return;
    }
    const VkResult result = map_transfer(*memory, offset, size, bytes.data(), nullptr);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.reply();
}

void handle_DownloadMappedMemory(Session& c) {
    const uint64_t device_id = c.reader.handle();
    const uint64_t memory_id = c.reader.handle();
    const uint64_t offset = c.reader.u64();
    const uint64_t size = c.reader.u64();
    ServerMemory* memory = nullptr;
    if (!c.reader.ok() || !validate_transfer(c, device_id, memory_id, offset, size, &memory)) {
        c.reply_status(Status::DecodeError);
        return;
    }
    std::vector<char> data;
    const VkResult result = map_transfer(*memory, offset, size, nullptr, &data);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.bytes(data.data(), data.size());
    c.reply();
}

void handle_BindBufferMemory(Session& c) {
    const uint64_t device_id = c.reader.handle(); VkDevice device = c.tables.devices.get(device_id);
    VkBuffer buffer = c.tables.buffer(c.reader.handle()); VkDeviceMemory memory = c.tables.memory(c.reader.handle());
    const uint64_t offset = c.reader.u64();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) { c.reply_status(Status::DecodeError); return; }
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.i32(vkBindBufferMemory(device, buffer, memory, offset)); c.reply();
}

void handle_BindImageMemory(Session& c) {
    const uint64_t device_id = c.reader.handle(); VkDevice device = c.tables.devices.get(device_id);
    VkImage image = c.tables.image(c.reader.handle()); VkDeviceMemory memory = c.tables.memory(c.reader.handle());
    const uint64_t offset = c.reader.u64();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) { c.reply_status(Status::DecodeError); return; }
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.i32(vkBindImageMemory(device, image, memory, offset)); c.reply();
}

void write_memory_requirements(remoting::Writer& w, const VkMemoryRequirements& r) {
    w.u64(r.size); w.u64(r.alignment); w.u32(r.memoryTypeBits);
}

void handle_GetBufferMemoryRequirements(Session& c) {
    const uint64_t device_id = c.reader.handle(); VkDevice device = c.tables.devices.get(device_id);
    VkBuffer buffer = c.tables.buffer(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE) { c.reply_status(Status::DecodeError); return; }
    VkMemoryRequirements reqs{}; vkGetBufferMemoryRequirements(device, buffer, &reqs);
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); write_memory_requirements(c.writer, reqs); c.reply();
}

void handle_GetImageMemoryRequirements(Session& c) {
    const uint64_t device_id = c.reader.handle(); VkDevice device = c.tables.devices.get(device_id);
    VkImage image = c.tables.image(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE) { c.reply_status(Status::DecodeError); return; }
    VkMemoryRequirements reqs{}; vkGetImageMemoryRequirements(device, image, &reqs);
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); write_memory_requirements(c.writer, reqs); c.reply();
}

}  // namespace

REGISTER_HANDLER(vkAllocateMemory, handle_AllocateMemory);
REGISTER_HANDLER(vkFreeMemory, handle_FreeMemory);
REGISTER_HANDLER(FlushMappedMemory, handle_FlushMappedMemory);
REGISTER_HANDLER(DownloadMappedMemory, handle_DownloadMappedMemory);
REGISTER_HANDLER(vkBindBufferMemory, handle_BindBufferMemory);
REGISTER_HANDLER(vkBindImageMemory, handle_BindImageMemory);
REGISTER_HANDLER(vkGetBufferMemoryRequirements, handle_GetBufferMemoryRequirements);
REGISTER_HANDLER(vkGetImageMemoryRequirements, handle_GetImageMemoryRequirements);
