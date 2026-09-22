// Connection helpers shared by every remoted entry point.
//
// windows.h's default (non-lean) mode drags in the legacy winsock.h, which
// conflicts with wire.hpp's winsock2.h if windows.h is reached first in this
// translation unit; defining this before any include - including
// remote_objects.hpp itself, which pulls in wire.hpp - keeps that from
// happening regardless of what pulls windows.h in.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#endif

#include "remote_objects.hpp"

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace remoting {

bool Connection::send_oneway(Opcode opcode, const Writer& request) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd == kInvalidSocket) return false;
    return send_message(fd, static_cast<uint32_t>(opcode), request.data());
}

bool Connection::round_trip(Opcode opcode, const Writer& request, std::vector<char>* reply) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd == kInvalidSocket) return false;
    if (!send_message(fd, static_cast<uint32_t>(opcode), request.data())) return false;

    MessageHeader header{};
    if (!recv_message(fd, &header, reply)) return false;

    // A reply for a different opcode means the two ends have lost step, and
    // every later call would be answered with the wrong data. Fail loudly.
    if (header.opcode != static_cast<uint32_t>(opcode)) return false;
    if (reply->size() < sizeof(uint32_t)) return false;

    uint32_t status = 0;
    std::memcpy(&status, reply->data(), sizeof(status));
    return status == static_cast<uint32_t>(Status::Ok);
}

bool RemoteDevice::flush_mapped() {
    std::lock_guard<std::mutex> lock(mapped_mutex);
    if (mapped.empty()) return true;

    Writer request;
    request.handle(remote_id);
    request.u32(static_cast<uint32_t>(mapped.size()));
    for (const MappedRange& range : mapped) {
        request.handle(range.memory_id);
        request.u64(range.offset);
        request.u64(range.size);
        request.bytes(range.shadow, static_cast<size_t>(range.size));
    }
    return instance->connection.send_oneway(Opcode::FlushMappedMemory, request);
}

bool RemoteDevice::download_mapped(const VkMappedMemoryRange* ranges, uint32_t count) {
    std::lock_guard<std::mutex> lock(mapped_mutex);
    if (count == 0) return true;

    Writer request;
    request.handle(remote_id);
    request.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t memory_id = id_from_handle(ranges[i].memory);
        VkDeviceSize size = ranges[i].size;
        // vkInvalidateMappedMemoryRanges allows VK_WHOLE_SIZE here just as
        // vkMapMemory does; unlike MapMemory (see above) this range's size
        // never gets resolved against the actual mapping before now, so
        // UINT64_MAX would otherwise go straight over the wire and the
        // server would try to size a std::vector with it.
        if (size == VK_WHOLE_SIZE) {
            for (const MappedRange& mapped_range : mapped) {
                if (mapped_range.memory_id == memory_id) {
                    size = mapped_range.size - (ranges[i].offset - mapped_range.offset);
                    break;
                }
            }
        }
        request.handle(memory_id);
        request.u64(ranges[i].offset);
        request.u64(size);
    }

    std::vector<char> reply;
    if (!instance->connection.round_trip(Opcode::DownloadMappedMemory, request, &reply)) {
        return false;
    }

    Reader reader(reply.data(), reply.size());
    reader.u32();  // status, already checked Ok by round_trip
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<char> bytes;
        if (!reader.bytes(&bytes)) return false;

        const uint64_t memory_id = id_from_handle(ranges[i].memory);
        for (MappedRange& range : mapped) {
            if (range.memory_id != memory_id) continue;
            const VkDeviceSize local_offset = ranges[i].offset - range.offset;
            if (local_offset + bytes.size() <= range.size) {
                std::memcpy(static_cast<char*>(range.shadow) + local_offset, bytes.data(),
                            bytes.size());
            }
            break;
        }
    }
    return true;
}

}  // namespace remoting
