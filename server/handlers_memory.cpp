// Server-side memory handlers: allocate/free device memory, the shadow-copy
// upload/download pair that stands in for a real vkMapMemory across the
// wire (FlushMappedMemory writes client bytes in, DownloadMappedMemory reads
// server bytes out - see remote_objects.hpp on the client side for the
// callers), bind-to-resource, and the two GetXMemoryRequirements queries.

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

void handle_AllocateMemory(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkMemoryAllocateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_MemoryAllocateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = vkAllocateMemory(device, &info, nullptr, &memory);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.memories.add(memory) : 0);
    c.reply();
}

void handle_FreeMemory(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t id = c.reader.handle();
    VkDeviceMemory memory = c.tables.memories.take(id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkFreeMemory(device, memory, nullptr);
}

void handle_FlushMappedMemory(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t count = c.reader.u32();
    bool ok = c.reader.ok() && device != VK_NULL_HANDLE;
    for (uint32_t i = 0; ok && i < count; ++i) {
        const uint64_t memory_id = c.reader.handle();
        const uint64_t offset = c.reader.u64();
        const uint64_t size = c.reader.u64();
        std::vector<char> bytes;
        if (!c.reader.bytes(&bytes) || bytes.size() != size) {
            ok = false;
            break;
        }
        VkDeviceMemory memory = c.tables.memory(memory_id);
        if (memory == VK_NULL_HANDLE) {
            ok = false;
            break;
        }
        void* mapped = nullptr;
        // Mapped only for the duration of this copy; the shadow-buffer
        // scheme never keeps a server-side mapping open between messages.
        if (vkMapMemory(device, memory, offset, size, 0, &mapped) != VK_SUCCESS) {
            ok = false;
            break;
        }
        memcpy(mapped, bytes.data(), bytes.size());
        vkUnmapMemory(device, memory);
    }
    if (!ok) mark_oneway_error(c);
}

void handle_DownloadMappedMemory(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t count = c.reader.u32();

    // recv_message already refuses a payload over 64MB (see wire.cpp), so no
    // genuine request needs more ranges than that could possibly encode; a
    // huge count here is either a desynchronised reader or a bogus size (see
    // below) and must be rejected before it sizes an allocation, not after.
    constexpr uint32_t kMaxRanges = 64u * 1024u * 1024u / 24u;
    if (count > kMaxRanges) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    struct Range {
        uint64_t memory_id;
        uint64_t offset;
        uint64_t size;
    };
    std::vector<Range> ranges(count);
    for (uint32_t i = 0; i < count; ++i) {
        ranges[i].memory_id = c.reader.handle();
        ranges[i].offset = c.reader.u64();
        ranges[i].size = c.reader.u64();
    }
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    // A range's size travels the wire as a bare uint64 (see
    // RemoteDevice::download_mapped), so VK_WHOLE_SIZE (client bug fixed
    // there) or any other oversized value would otherwise reach here intact
    // and turn into a multi-exabyte std::vector construction, which throws
    // std::length_error and kills the server. This is the same 64MB ceiling
    // recv_message already applies to a whole message.
    constexpr uint64_t kMaxRangeSize = 64ull * 1024u * 1024u;
    uint64_t total = 0;
    for (const Range& range : ranges) {
        // Bounding each range alone still lets many of them agree: a range
        // costs 24 bytes to ask for and 64MB to answer, so the sum is what a
        // peer can actually amplify. Subtracting keeps the test overflow-free.
        if (range.size > kMaxRangeSize - total) {
            c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
            return;
        }
        total += range.size;
    }

    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    for (const Range& range : ranges) {
        VkDeviceMemory memory = c.tables.memory(range.memory_id);
        std::vector<char> data(static_cast<size_t>(range.size));
        if (memory != VK_NULL_HANDLE) {
            void* mapped = nullptr;
            if (vkMapMemory(device, memory, range.offset, range.size, 0, &mapped) == VK_SUCCESS) {
                memcpy(data.data(), mapped, data.size());
                vkUnmapMemory(device, memory);
            }
        }
        c.writer.bytes(data.data(), data.size());
    }
    c.reply();
}

void handle_BindBufferMemory(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkBuffer buffer = c.tables.buffer(c.reader.handle());
    VkDeviceMemory memory = c.tables.memory(c.reader.handle());
    const uint64_t offset = c.reader.u64();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkBindBufferMemory(device, buffer, memory, offset);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.reply();
}

void handle_BindImageMemory(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkImage image = c.tables.image(c.reader.handle());
    VkDeviceMemory memory = c.tables.memory(c.reader.handle());
    const uint64_t offset = c.reader.u64();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkBindImageMemory(device, image, memory, offset);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.reply();
}

void write_memory_requirements(remoting::Writer& w, const VkMemoryRequirements& r) {
    w.u64(r.size);
    w.u64(r.alignment);
    w.u32(r.memoryTypeBits);
}

void handle_GetBufferMemoryRequirements(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkBuffer buffer = c.tables.buffer(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device, buffer, &reqs);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    write_memory_requirements(c.writer, reqs);
    c.reply();
}

void handle_GetImageMemoryRequirements(Session& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkImage image = c.tables.image(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(device, image, &reqs);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    write_memory_requirements(c.writer, reqs);
    c.reply();
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
