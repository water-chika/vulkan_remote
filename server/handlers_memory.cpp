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
        id = c.tables.memories.add(memory, device);
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

void handle_GetDeviceMemoryCommitment(Session& c) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    VkDeviceMemory memory = c.tables.memory(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE || memory == VK_NULL_HANDLE) {
        c.reply_status(Status::DecodeError); return;
    }
    VkDeviceSize committed = 0; vkGetDeviceMemoryCommitment(device, memory, &committed);
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.u64(committed); c.reply();
}

void handle_GetImageSparseMemoryRequirements(Session& c) {
    VkDevice device = c.tables.devices.get(c.reader.handle());
    VkImage image = c.tables.image(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE || image == VK_NULL_HANDLE) {
        c.reply_status(Status::DecodeError); return;
    }
    uint32_t count = 0; vkGetImageSparseMemoryRequirements(device, image, &count, nullptr);
    if (count > remoting::kMaxArrayElements) { c.reply_status(Status::DecodeError); return; }
    std::vector<VkSparseImageMemoryRequirements> values(count);
    if (count) vkGetImageSparseMemoryRequirements(device, image, &count, values.data());
    c.writer.u32(static_cast<uint32_t>(Status::Ok)); c.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& v = values[i]; c.writer.u32(v.formatProperties.aspectMask);
        c.writer.u32(v.formatProperties.imageGranularity.width);
        c.writer.u32(v.formatProperties.imageGranularity.height);
        c.writer.u32(v.formatProperties.imageGranularity.depth);
        c.writer.u32(v.formatProperties.flags); c.writer.u32(v.imageMipTailFirstLod);
        c.writer.u64(v.imageMipTailSize); c.writer.u64(v.imageMipTailOffset);
        c.writer.u64(v.imageMipTailStride);
    }
    c.reply();
}

void handle_BindBufferMemory2(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());uint32_t n=c.reader.u32();if(n>remoting::kMaxArrayElements||!count_fits(c.reader,n,24)){c.reply_status(Status::DecodeError);return;}std::vector<VkBindBufferMemoryInfo> v(n);for(auto&x:v){x={VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};x.buffer=c.tables.buffer(c.reader.handle());x.memory=c.tables.memory(c.reader.handle());x.memoryOffset=c.reader.u64();}if(!c.reader.ok()||!d){c.reply_status(Status::DecodeError);return;}auto fn=reinterpret_cast<PFN_vkBindBufferMemory2>(vkGetDeviceProcAddr(d,"vkBindBufferMemory2"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.i32(fn(d,n,v.data()));c.reply();}
void handle_BindImageMemory2(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());uint32_t n=c.reader.u32();if(n>remoting::kMaxArrayElements||!count_fits(c.reader,n,28)){c.reply_status(Status::DecodeError);return;}Arena arena;std::vector<VkBindImageMemoryInfo>v(n);bool ok=d!=VK_NULL_HANDLE;for(auto&x:v){x={VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};x.image=c.tables.image(c.reader.handle());x.memory=c.tables.memory(c.reader.handle());x.memoryOffset=c.reader.u64();ok=ok&&remoting::read_pnext_chain(c.reader,arena,c.tables,remoting::PNextContext::BindImageMemory2,&x.pNext);}if(!ok||!c.reader.ok()){c.reply_status(Status::DecodeError);return;}auto fn=reinterpret_cast<PFN_vkBindImageMemory2>(vkGetDeviceProcAddr(d,"vkBindImageMemory2"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.i32(fn(d,n,v.data()));c.reply();}
void handle_GetBufferMemoryRequirements2(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkBufferMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};info.buffer=c.tables.buffer(c.reader.handle());Arena arena;void*chain=nullptr;if(!d||!remoting::read_pnext_output_topology(c.reader,arena,remoting::PNextContext::MemoryRequirements2,&chain)){c.reply_status(Status::DecodeError);return;}VkMemoryRequirements2 out{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,chain};auto fn=reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(vkGetDeviceProcAddr(d,"vkGetBufferMemoryRequirements2"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}fn(d,&info,&out);c.writer.u32(static_cast<uint32_t>(Status::Ok));write_memory_requirements(c.writer,out.memoryRequirements);remoting::write_pnext_output_values(c.writer,remoting::PNextContext::MemoryRequirements2,chain);c.reply();}
void handle_GetImageMemoryRequirements2(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};info.image=c.tables.image(c.reader.handle());Arena arena;const void*input=nullptr;void*output=nullptr;if(!d||!remoting::read_pnext_chain(c.reader,arena,c.tables,remoting::PNextContext::MemoryRequirements2,&input)||!remoting::read_pnext_output_topology(c.reader,arena,remoting::PNextContext::MemoryRequirements2,&output)){c.reply_status(Status::DecodeError);return;}info.pNext=input;VkMemoryRequirements2 out{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,output};auto fn=reinterpret_cast<PFN_vkGetImageMemoryRequirements2>(vkGetDeviceProcAddr(d,"vkGetImageMemoryRequirements2"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}fn(d,&info,&out);c.writer.u32(static_cast<uint32_t>(Status::Ok));write_memory_requirements(c.writer,out.memoryRequirements);remoting::write_pnext_output_values(c.writer,remoting::PNextContext::MemoryRequirements2,output);c.reply();}
void handle_GetImageSparseMemoryRequirements2(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkImageSparseMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2};info.image=c.tables.image(c.reader.handle());if(!c.reader.ok()||!d||!info.image){c.reply_status(Status::DecodeError);return;}auto fn=reinterpret_cast<PFN_vkGetImageSparseMemoryRequirements2>(vkGetDeviceProcAddr(d,"vkGetImageSparseMemoryRequirements2"));if(!fn){c.reply_status(Status::UnsupportedCommand);return;}uint32_t n=0;fn(d,&info,&n,nullptr);if(n>remoting::kMaxArrayElements){c.reply_status(Status::DecodeError);return;}std::vector<VkSparseImageMemoryRequirements2>v(n,VkSparseImageMemoryRequirements2{VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2});if(n)fn(d,&info,&n,v.data());c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.u32(n);for(auto&x:v){auto&q=x.memoryRequirements;c.writer.u32(q.formatProperties.aspectMask);c.writer.u32(q.formatProperties.imageGranularity.width);c.writer.u32(q.formatProperties.imageGranularity.height);c.writer.u32(q.formatProperties.imageGranularity.depth);c.writer.u32(q.formatProperties.flags);c.writer.u32(q.imageMipTailFirstLod);c.writer.u64(q.imageMipTailSize);c.writer.u64(q.imageMipTailOffset);c.writer.u64(q.imageMipTailStride);}c.reply();}

void handle_GetBufferDeviceAddress(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkBuffer b=c.tables.buffer(c.reader.handle());if(!c.reader.ok()||!d||!b){c.reply_status(Status::DecodeError);return;}c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.u64(0);c.reply();}
void handle_GetBufferOpaqueCaptureAddress(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkBuffer b=c.tables.buffer(c.reader.handle());if(!c.reader.ok()||!d||!b){c.reply_status(Status::DecodeError);return;}c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.u64(0);c.reply();}
void handle_GetDeviceMemoryOpaqueCaptureAddress(Session& c){VkDevice d=c.tables.devices.get(c.reader.handle());VkDeviceMemory m=c.tables.memory(c.reader.handle());if(!c.reader.ok()||!d||!m){c.reply_status(Status::DecodeError);return;}c.writer.u32(static_cast<uint32_t>(Status::Ok));c.writer.u64(0);c.reply();}

}  // namespace

REGISTER_HANDLER(vkAllocateMemory, handle_AllocateMemory);
REGISTER_HANDLER(vkFreeMemory, handle_FreeMemory);
REGISTER_HANDLER(FlushMappedMemory, handle_FlushMappedMemory);
REGISTER_HANDLER(DownloadMappedMemory, handle_DownloadMappedMemory);
REGISTER_HANDLER(vkBindBufferMemory, handle_BindBufferMemory);
REGISTER_HANDLER(vkBindImageMemory, handle_BindImageMemory);
REGISTER_HANDLER(vkGetBufferMemoryRequirements, handle_GetBufferMemoryRequirements);
REGISTER_HANDLER(vkGetImageMemoryRequirements, handle_GetImageMemoryRequirements);
REGISTER_HANDLER(vkGetDeviceMemoryCommitment, handle_GetDeviceMemoryCommitment);
REGISTER_HANDLER(vkGetImageSparseMemoryRequirements, handle_GetImageSparseMemoryRequirements);
REGISTER_HANDLER(vkBindBufferMemory2, handle_BindBufferMemory2);
REGISTER_HANDLER(vkBindImageMemory2, handle_BindImageMemory2);
REGISTER_HANDLER(vkGetBufferMemoryRequirements2, handle_GetBufferMemoryRequirements2);
REGISTER_HANDLER(vkGetImageMemoryRequirements2, handle_GetImageMemoryRequirements2);
REGISTER_HANDLER(vkGetImageSparseMemoryRequirements2, handle_GetImageSparseMemoryRequirements2);

REGISTER_HANDLER(vkGetBufferDeviceAddress, handle_GetBufferDeviceAddress);
REGISTER_HANDLER(vkGetBufferOpaqueCaptureAddress, handle_GetBufferOpaqueCaptureAddress);
REGISTER_HANDLER(vkGetDeviceMemoryOpaqueCaptureAddress, handle_GetDeviceMemoryOpaqueCaptureAddress);
