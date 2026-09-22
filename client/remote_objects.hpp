#pragma once

// Client-side object model.
//
// Two kinds of Vulkan handle need opposite treatment here. Non-dispatchable
// handles (buffers, images, fences...) carry no local state, so the client
// simply uses the server's id as the handle and never allocates anything.
// Dispatchable handles (VkDevice, VkQueue, VkCommandBuffer) cannot do that:
// the loader requires their first word to be VK_LOADER_DATA, so each one has
// to be a real local object that happens to remember an id.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "remoting_commands.inl"
#include "wire.hpp"

namespace remoting {

template <typename H>
inline H handle_from_id(uint64_t id) {
    return reinterpret_cast<H>(static_cast<uintptr_t>(id));
}

template <typename H>
inline uint64_t id_from_handle(H handle) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
}

struct Connection {
    socket_t fd = kInvalidSocket;
    std::mutex mutex;

    // Recording commands are void and nobody waits on them, so they are sent
    // without a reply. That is the only reason this driver is not ruinously
    // slow at recording time: a command buffer of 40 calls costs one round
    // trip instead of 40. The price is that an error inside a fire-and-forget
    // command is not seen until the next call that does wait, which is why the
    // server counts them and reports the count back.
    bool send_oneway(Opcode opcode, const Writer& request);
    bool round_trip(Opcode opcode, const Writer& request, std::vector<char>* reply);
};

// One shadow allocation for a range the application has mapped.
//
// vkMapMemory promises a pointer the caller can dereference, and no socket can
// deliver one, so the client hands back ordinary memory and the contents are
// pushed to the server before anything can observe them there. Writes are the
// only direction that works: a program that maps memory to READ what the GPU
// wrote would see stale bytes. vkcube only ever writes through its mappings,
// and that limitation is recorded rather than hidden.
struct MappedRange {
    uint64_t memory_id = 0;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void* shadow = nullptr;
};

struct RemoteInstance {
    VK_LOADER_DATA loader_data;
    Connection connection;
    std::vector<struct RemotePhysicalDevice*> physical_devices;
};

struct RemotePhysicalDevice {
    VK_LOADER_DATA loader_data;
    RemoteInstance* instance = nullptr;
    uint64_t remote_id = 0;
};

struct RemoteDevice {
    VK_LOADER_DATA loader_data;
    RemoteInstance* instance = nullptr;
    uint64_t remote_id = 0;
    std::vector<struct RemoteQueue*> queues;
    std::vector<struct RemoteCommandBuffer*> command_buffers;
    std::vector<MappedRange> mapped;
    std::mutex mapped_mutex;
    // Allocation size requested at vkAllocateMemory, keyed by memory id. Needed
    // to resolve VK_WHOLE_SIZE at vkMapMemory time, since the wire format
    // never carries the allocation's own size back to the client otherwise.
    std::unordered_map<uint64_t, VkDeviceSize> memory_sizes;

    // Called before any submission, because the server must see whatever the
    // application wrote through its mappings since the last one.
    bool flush_mapped();

    // Called at vkMapMemory (for the newly mapped range) and at
    // vkInvalidateMappedMemoryRanges (for the ranges the caller named), so a
    // shadow buffer that has just been created, or one the caller is asking
    // to be refreshed, gets what the server-side memory actually holds.
    bool download_mapped(const VkMappedMemoryRange* ranges, uint32_t count);
};

struct RemoteQueue {
    VK_LOADER_DATA loader_data;
    RemoteDevice* device = nullptr;
    uint64_t remote_id = 0;
};

struct RemoteCommandBuffer {
    VK_LOADER_DATA loader_data;
    RemoteDevice* device = nullptr;
    uint64_t remote_id = 0;
};

// Device-level function table, defined in icd_device.cpp. vkGetDeviceProcAddr
// must serve these: the loader builds its device dispatch table from
// whatever that returns, so anything implemented but missing here is simply
// never called.
struct DeviceEntry {
    const char* name;
    PFN_vkVoidFunction function;
};
const DeviceEntry* get_device_entries(size_t* count);

}  // namespace remoting
