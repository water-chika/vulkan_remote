// Connection helpers and mapped-memory transfer implementation.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#endif

#include "remote_objects.hpp"

#include <algorithm>
#include <limits>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace remoting {

RemoteInstance::~RemoteInstance() = default;

bool Connection::send_oneway(Opcode opcode, const Writer& request) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd == kInvalidSocket) return false;
    return send_message(fd, static_cast<uint32_t>(opcode), request.data());
}

namespace {
constexpr VkDeviceSize kMaxMappedChunk = 64ull * 1024ull * 1024ull - 64ull;

bool round_trip_locked(socket_t fd, Opcode opcode, const Writer& request,
                       std::vector<char>* reply) {
    if (fd == kInvalidSocket) return false;
    if (!send_message(fd, static_cast<uint32_t>(opcode), request.data())) return false;
    MessageHeader header{};
    if (!recv_message(fd, &header, reply)) return false;
    if (header.opcode != static_cast<uint32_t>(opcode)) return false;
    if (reply->size() < sizeof(uint32_t)) return false;
    uint32_t status = 0;
    std::memcpy(&status, reply->data(), sizeof(status));
    return status == static_cast<uint32_t>(Status::Ok);
}

bool resolve_range(const std::vector<MappedRange>& mapped, const VkMappedMemoryRange& requested,
                   MappedRange const** mapping, VkDeviceSize* offset, VkDeviceSize* size) {
    const uint64_t memory_id = id_from_handle(requested.memory);
    for (const MappedRange& candidate : mapped) {
        if (candidate.memory_id != memory_id) continue;
        if (requested.offset < candidate.offset) return false;
        const VkDeviceSize local = requested.offset - candidate.offset;
        if (local > candidate.size) return false;
        const VkDeviceSize available = candidate.size - local;
        const VkDeviceSize resolved = requested.size == VK_WHOLE_SIZE ? available : requested.size;
        if (resolved > available) return false;
        *mapping = &candidate;
        *offset = requested.offset;
        *size = resolved;
        return true;
    }
    return false;
}
}  // namespace

bool Connection::round_trip(Opcode opcode, const Writer& request, std::vector<char>* reply) {
    std::lock_guard<std::mutex> lock(mutex);
    return round_trip_locked(fd, opcode, request, reply);
}

bool Connection::try_round_trip(Opcode opcode, const Writer& request, std::vector<char>* reply) {
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    return round_trip_locked(fd, opcode, request, reply);
}

VkResult RemoteDevice::flush_mapped(const VkMappedMemoryRange* ranges, uint32_t count) {
    std::lock_guard<std::mutex> lock(mapped_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        const MappedRange* mapping = nullptr;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        if (!resolve_range(mapped, ranges[i], &mapping, &offset, &size)) return VK_ERROR_DEVICE_LOST;
        const VkDeviceSize local = offset - mapping->offset;
        for (VkDeviceSize done = 0; done < size;) {
            VkDeviceSize chunk = std::min(kMaxMappedChunk, size - done);
            if (!mapping->coherent && chunk < size - done) {
                const auto allocation = memories.find(mapping->memory_id);
                if (allocation == memories.end()) return VK_ERROR_DEVICE_LOST;
                const VkDeviceSize atom = allocation->second.non_coherent_atom_size;
                chunk -= chunk % atom;
                if (chunk == 0) return VK_ERROR_DEVICE_LOST;
            }
            Writer request;
            request.handle(remote_id);
            request.handle(mapping->memory_id);
            request.u64(offset + done);
            request.u64(chunk);
            request.bytes(static_cast<const char*>(mapping->shadow) + local + done,
                          static_cast<size_t>(chunk));
            std::vector<char> reply;
            if (!instance->connection.round_trip(Opcode::FlushMappedMemory, request, &reply)) {
                return VK_ERROR_DEVICE_LOST;
            }
            Reader reader(reply.data(), reply.size());
            if (reader.u32() != static_cast<uint32_t>(Status::Ok)) return VK_ERROR_DEVICE_LOST;
            const VkResult result = static_cast<VkResult>(reader.i32());
            if (!reader.ok()) return VK_ERROR_DEVICE_LOST;
            if (result != VK_SUCCESS) return result;
            done += chunk;
        }
    }
    return VK_SUCCESS;
}

bool RemoteDevice::flush_coherent_mapped() {
    std::vector<VkMappedMemoryRange> ranges;
    {
        std::lock_guard<std::mutex> lock(mapped_mutex);
        for (const MappedRange& mapping : mapped) {
            if (!mapping.coherent) continue;
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = handle_from_id<VkDeviceMemory>(mapping.memory_id);
            range.offset = mapping.offset;
            range.size = mapping.size;
            ranges.push_back(range);
        }
    }
    return flush_mapped(ranges.data(), static_cast<uint32_t>(ranges.size())) == VK_SUCCESS;
}

VkResult RemoteDevice::download_mapped(const VkMappedMemoryRange* ranges, uint32_t count) {
    std::lock_guard<std::mutex> lock(mapped_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        const MappedRange* mapping = nullptr;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        if (!resolve_range(mapped, ranges[i], &mapping, &offset, &size)) return VK_ERROR_DEVICE_LOST;
        const VkDeviceSize local = offset - mapping->offset;
        for (VkDeviceSize done = 0; done < size;) {
            VkDeviceSize chunk = std::min(kMaxMappedChunk, size - done);
            if (!mapping->coherent && chunk < size - done) {
                const auto allocation = memories.find(mapping->memory_id);
                if (allocation == memories.end()) return VK_ERROR_DEVICE_LOST;
                const VkDeviceSize atom = allocation->second.non_coherent_atom_size;
                chunk -= chunk % atom;
                if (chunk == 0) return VK_ERROR_DEVICE_LOST;
            }
            Writer request;
            request.handle(remote_id);
            request.handle(mapping->memory_id);
            request.u64(offset + done);
            request.u64(chunk);
            std::vector<char> reply;
            if (!instance->connection.round_trip(Opcode::DownloadMappedMemory, request, &reply)) {
                return VK_ERROR_DEVICE_LOST;
            }
            Reader reader(reply.data(), reply.size());
            if (reader.u32() != static_cast<uint32_t>(Status::Ok)) return VK_ERROR_DEVICE_LOST;
            const VkResult result = static_cast<VkResult>(reader.i32());
            if (!reader.ok()) return VK_ERROR_DEVICE_LOST;
            if (result != VK_SUCCESS) return result;
            std::vector<char> bytes;
            if (!reader.bytes(&bytes, static_cast<size_t>(chunk)) || bytes.size() != chunk ||
                !reader.ok()) return VK_ERROR_DEVICE_LOST;
            std::memcpy(static_cast<char*>(mapping->shadow) + local + done,
                        bytes.data(), bytes.size());
            done += chunk;
        }
    }
    return VK_SUCCESS;
}

}  // namespace remoting
