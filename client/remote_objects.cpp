// Connection helpers and mapped-memory transfer implementation.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#endif

#include "remote_objects.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace remoting {

RemoteInstance::~RemoteInstance() = default;

namespace {
constexpr VkDeviceSize kMaxMappedChunk = 64ull * 1024ull * 1024ull - 64ull;

std::chrono::milliseconds heartbeat_duration(const char* name, long fallback) {
    const char* text = std::getenv(name);
    if (!text || !*text) return std::chrono::milliseconds(fallback);
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > 3600000)
        return std::chrono::milliseconds(fallback);
    return std::chrono::milliseconds(value);
}

bool resolve_range(std::vector<MappedRange>& mapped, const VkMappedMemoryRange& requested,
                   MappedRange** mapping, VkDeviceSize* offset, VkDeviceSize* size) {
    const uint64_t memory_id = id_from_handle(requested.memory);
    for (MappedRange& candidate : mapped) {
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

void Connection::enable_heartbeat() {
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        heartbeat_enabled = true;
    }
    watchdog_cv.notify_all();
}

bool Connection::start_receiver() {
    std::lock_guard<std::mutex> lock(pending_mutex);
    if (failed || fd == kInvalidSocket) return false;
    if (receiver_started) return true;
    receiver_started = true;
    receiver = std::thread([this] { receive_loop(); });
    watchdog = std::thread([this] { watchdog_loop(); });
    return true;
}

void Connection::fail_all() {
    std::vector<std::shared_ptr<Pending>> waiters;
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        if (failed.exchange(true)) return;
        stopping = true;
        application_pending = 0;
        heartbeat_id = 0;
        for (auto& item : pending) waiters.push_back(item.second);
        pending.clear();
        watchdog_cv.notify_all();
    }
    for (const auto& waiter : waiters) {
        std::lock_guard<std::mutex> lock(waiter->mutex);
        waiter->done = true;
        waiter->ok = false;
        waiter->cv.notify_all();
    }
}

void Connection::watchdog_loop() {
    const auto interval = heartbeat_duration("VK_REMOTING_HEARTBEAT_INTERVAL_MS", 5000);
    const auto timeout = heartbeat_duration("VK_REMOTING_HEARTBEAT_TIMEOUT_MS", 15000);
    if (interval.count() == 0 || timeout.count() == 0) return;
    std::unique_lock<std::mutex> lock(pending_mutex);
    for (;;) {
        watchdog_cv.wait(lock, [&] {
            return stopping || failed || (heartbeat_enabled && application_pending != 0);
        });
        if (stopping || failed) return;
        if (watchdog_cv.wait_for(lock, interval, [&] {
                return stopping || failed || application_pending == 0;
            })) continue;
        if (heartbeat_id != 0 || application_pending == 0) continue;
        uint64_t id = 0;
        do { id = next_request_id++; } while (id == 0 || pending.count(id));
        auto waiter = std::make_shared<Pending>();
        waiter->opcode = Opcode::Ping;
        pending.emplace(id, waiter);
        heartbeat_id = id;
        lock.unlock();
        Writer request;
        bool sent = false;
        {
            std::lock_guard<std::mutex> send_lock(send_mutex);
            sent = !failed && fd != kInvalidSocket &&
                send_message(fd, static_cast<uint32_t>(Opcode::Ping), request.data(), id);
        }
        if (!sent) {
            fail_all();
            lock.lock();
            continue;
        }
        std::unique_lock<std::mutex> waiter_lock(waiter->mutex);
        const bool completed = waiter->cv.wait_for(
            waiter_lock, timeout, [&] { return waiter->done; });
        waiter_lock.unlock();
        lock.lock();
        if (!completed && heartbeat_id == id && application_pending != 0 &&
            !stopping && !failed) {
            lock.unlock();
            fprintf(stderr, "vulkan-remoting: server heartbeat timed out\n");
            fail_all();
            socket_t socket = kInvalidSocket;
            {
                std::lock_guard<std::mutex> send_lock(send_mutex);
                socket = fd;
            }
            if (socket != kInvalidSocket) {
#if defined(_WIN32)
                ::shutdown(socket, SD_BOTH);
#else
                ::shutdown(socket, SHUT_RDWR);
#endif
            }
            return;
        }
        if (!completed && heartbeat_id == id) {
            // No application request remains, so a missing Pong is harmless.
            // Keep the ID pending so a late Pong remains correlated, and avoid
            // sending another heartbeat until it arrives. If new work appears,
            // the unanswered Ping is immediately treated as a liveness failure.
            watchdog_cv.wait(lock, [&] {
                return stopping || failed || heartbeat_id == 0 || application_pending != 0;
            });
            if (application_pending != 0 && heartbeat_id != 0 && !stopping && !failed) {
                lock.unlock();
                fprintf(stderr, "vulkan-remoting: prior server heartbeat remained unanswered\n");
                fail_all();
                return;
            }
            }
    }
}

void Connection::receive_loop() {
    for (;;) {
        MessageHeader header{};
        std::vector<char> payload;
        if (!recv_message(fd, &header, &payload)) break;
        std::shared_ptr<Pending> waiter;
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            auto it = pending.find(header.request_id);
            if (header.request_id == 0 || it == pending.end() ||
                header.opcode != static_cast<uint32_t>(it->second->opcode)) {
                break;
            }
            waiter = it->second;
            if (header.request_id == heartbeat_id) {
                heartbeat_id = 0;
                    }
            else if (application_pending != 0)
                --application_pending;
            pending.erase(it);
            watchdog_cv.notify_all();
        }
        bool ok = payload.size() >= sizeof(uint32_t);
        if (ok) {
            uint32_t status = 0;
            std::memcpy(&status, payload.data(), sizeof(status));
            ok = status == static_cast<uint32_t>(Status::Ok);
        }
        {
            std::lock_guard<std::mutex> lock(waiter->mutex);
            waiter->reply = std::move(payload);
            waiter->done = true;
            waiter->ok = ok;
        }
        waiter->cv.notify_all();
    }
    fail_all();
}

void Connection::shutdown() {
    fail_all();
    socket_t socket = kInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        socket = fd;
    }
    if (socket != kInvalidSocket) {
#if defined(_WIN32)
        ::shutdown(socket, SD_BOTH);
#else
        ::shutdown(socket, SHUT_RDWR);
#endif
    }
    watchdog_cv.notify_all();
    if (watchdog.joinable() && watchdog.get_id() != std::this_thread::get_id()) watchdog.join();
    if (receiver.joinable() && receiver.get_id() != std::this_thread::get_id()) receiver.join();
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        if (fd == socket) fd = kInvalidSocket;
    }
    if (socket != kInvalidSocket) close_socket(socket);
}

bool Connection::send_oneway(Opcode opcode, const Writer& request) {
    std::lock_guard<std::mutex> lock(send_mutex);
    if (failed || fd == kInvalidSocket) return false;
    if (!send_message(fd, static_cast<uint32_t>(opcode), request.data(), 0)) {
        fail_all();
        return false;
    }
    return true;
}

bool Connection::round_trip(Opcode opcode, const Writer& request, std::vector<char>* reply) {
    auto waiter = std::make_shared<Pending>();
    waiter->opcode = opcode;
    uint64_t request_id = 0;
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        if (failed || pending.size() >= 1024) return false;
        do { request_id = next_request_id++; } while (request_id == 0 || pending.count(request_id));
        pending.emplace(request_id, waiter);
        ++application_pending;
        watchdog_cv.notify_all();
    }
    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        sent = !failed && fd != kInvalidSocket &&
            send_message(fd, static_cast<uint32_t>(opcode), request.data(), request_id);
    }
    if (!sent) {
        fail_all();
        return false;
    }
    std::unique_lock<std::mutex> lock(waiter->mutex);
    waiter->cv.wait(lock, [&] { return waiter->done; });
    *reply = std::move(waiter->reply);
    return waiter->ok;
}

bool Connection::try_round_trip(Opcode opcode, const Writer& request, std::vector<char>* reply) {
    auto waiter = std::make_shared<Pending>();
    waiter->opcode = opcode;
    uint64_t request_id = 0;
    {
        std::unique_lock<std::mutex> pending_lock(pending_mutex, std::try_to_lock);
        if (!pending_lock.owns_lock() || failed || pending.size() >= 1024) return false;
        do { request_id = next_request_id++; } while (request_id == 0 || pending.count(request_id));
        pending.emplace(request_id, waiter);
        ++application_pending;
        watchdog_cv.notify_all();
    }

    bool attempted = false;
    bool sent = false;
    {
        std::unique_lock<std::mutex> send_lock(send_mutex, std::try_to_lock);
        if (send_lock.owns_lock() && !failed && fd != kInvalidSocket) {
            attempted = true;
            sent = send_message(
                fd, static_cast<uint32_t>(opcode), request.data(), request_id);
        }
    }
    if (!sent) {
        if (attempted) {
            fail_all();
        } else {
            std::lock_guard<std::mutex> pending_lock(pending_mutex);
            pending.erase(request_id);
            if (application_pending != 0) --application_pending;
            watchdog_cv.notify_all();
        }
        return false;
    }

    std::unique_lock<std::mutex> lock(waiter->mutex);
    waiter->cv.wait(lock, [&] { return waiter->done; });
    *reply = std::move(waiter->reply);
    return waiter->ok;
}

VkResult RemoteDevice::flush_mapped(const VkMappedMemoryRange* ranges, uint32_t count) {
    std::lock_guard<std::mutex> lock(mapped_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        MappedRange* mapping = nullptr;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        if (!resolve_range(mapped, ranges[i], &mapping, &offset, &size)) return VK_ERROR_DEVICE_LOST;
        const VkDeviceSize local = offset - mapping->offset;
        for (VkDeviceSize done = 0; done < size;) {
            VkDeviceSize chunk = (std::min)(kMaxMappedChunk, size - done);
            if (chunk < size - done) {
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

bool RemoteDevice::publish_coherent_mapped() {
    std::lock_guard<std::mutex> lock(mapped_mutex);
    for (MappedRange& mapping : mapped) {
        if (!mapping.emulated_coherent || !mapping.transfer_ready) continue;
        const char* shadow = static_cast<const char*>(mapping.shadow);
        size_t begin = 0;
        while (begin < mapping.size) {
            while (begin < mapping.size && shadow[begin] == mapping.baseline[begin]) ++begin;
            if (begin == mapping.size) break;
            size_t end = begin + 1;
            while (end < mapping.size) {
                const size_t gap_begin = end;
                while (end < mapping.size && shadow[end] == mapping.baseline[end] &&
                       end - gap_begin < 4096)
                    ++end;
                if (end == mapping.size || end - gap_begin == 4096) {
                    end = gap_begin;
                    break;
                }
                while (end < mapping.size && shadow[end] != mapping.baseline[end]) ++end;
            }
            for (size_t done = begin; done < end;) {
                const size_t chunk = (std::min)(static_cast<size_t>(kMaxMappedChunk), end - done);
                Writer request;
                request.handle(remote_id);
                request.handle(mapping.memory_id);
                request.u64(mapping.offset + done);
                request.u64(chunk);
                request.bytes(shadow + done, chunk);
                std::vector<char> reply;
                if (!instance->connection.round_trip(Opcode::FlushMappedMemory, request, &reply))
                    return false;
                Reader reader(reply.data(), reply.size());
                if (reader.u32() != static_cast<uint32_t>(Status::Ok) ||
                    static_cast<VkResult>(reader.i32()) != VK_SUCCESS || !reader.ok())
                    return false;
                std::memcpy(mapping.baseline.data() + done, shadow + done, chunk);
                done += chunk;
            }
            begin = end;
        }
    }
    return true;
}

bool RemoteDevice::acquire_coherent_mapped(const std::unordered_set<uint64_t>* memory_ids) {
    std::lock_guard<std::mutex> lock(mapped_mutex);
    for (MappedRange& mapping : mapped) {
        if (!mapping.emulated_coherent || !mapping.transfer_ready ||
            (memory_ids && !memory_ids->count(mapping.memory_id))) continue;
        char* shadow = static_cast<char*>(mapping.shadow);
        for (VkDeviceSize done = 0; done < mapping.size;) {
            const VkDeviceSize chunk = (std::min)(kMaxMappedChunk, mapping.size - done);
            Writer request;
            request.handle(remote_id);
            request.handle(mapping.memory_id);
            request.u64(mapping.offset + done);
            request.u64(chunk);
            std::vector<char> reply;
            if (!instance->connection.round_trip(Opcode::DownloadMappedMemory, request, &reply))
                return false;
            Reader reader(reply.data(), reply.size());
            if (reader.u32() != static_cast<uint32_t>(Status::Ok) ||
                static_cast<VkResult>(reader.i32()) != VK_SUCCESS)
                return false;
            std::vector<char> bytes;
            if (!reader.bytes(&bytes, static_cast<size_t>(chunk)) || bytes.size() != chunk ||
                !reader.ok())
                return false;
            const size_t base = static_cast<size_t>(done);
            for (size_t i = 0; i < bytes.size(); ++i) {
                // Preserve host stores newer than the prior baseline while
                // replacing bytes the host has not touched with device output.
                if (shadow[base + i] == mapping.baseline[base + i])
                    shadow[base + i] = bytes[i];
                mapping.baseline[base + i] = bytes[i];
            }
            done += chunk;
        }
    }
    return true;
}

VkResult RemoteDevice::download_mapped(const VkMappedMemoryRange* ranges, uint32_t count) {
    std::lock_guard<std::mutex> lock(mapped_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        MappedRange* mapping = nullptr;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        if (!resolve_range(mapped, ranges[i], &mapping, &offset, &size)) return VK_ERROR_DEVICE_LOST;
        const VkDeviceSize local = offset - mapping->offset;
        for (VkDeviceSize done = 0; done < size;) {
            VkDeviceSize chunk = (std::min)(kMaxMappedChunk, size - done);
            if (chunk < size - done) {
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
            if (mapping->emulated_coherent && mapping->baseline.size() == mapping->size)
                std::memcpy(mapping->baseline.data() + local + done,
                            bytes.data(), bytes.size());
            done += chunk;
        }
    }
    return VK_SUCCESS;
}

}  // namespace remoting
