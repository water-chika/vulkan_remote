// Connection helpers shared by every remoted entry point.

#include "remote_objects.hpp"

#include <unistd.h>

namespace remoting {

bool Connection::send_oneway(Opcode opcode, const Writer& request) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd < 0) return false;
    return send_message(fd, static_cast<uint32_t>(opcode), request.data());
}

bool Connection::round_trip(Opcode opcode, const Writer& request, std::vector<char>* reply) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd < 0) return false;
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
        request.handle(id_from_handle(ranges[i].memory));
        request.u64(ranges[i].offset);
        request.u64(ranges[i].size);
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
