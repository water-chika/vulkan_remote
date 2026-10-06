#pragma once

// Client-side object model. Dispatchable handles are local loader-compatible
// wrappers; non-dispatchable handles are server object ids.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
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

    bool send_oneway(Opcode opcode, const Writer& request);
    bool round_trip(Opcode opcode, const Writer& request, std::vector<char>* reply);
    bool try_round_trip(Opcode opcode, const Writer& request, std::vector<char>* reply);
};

struct MemoryAllocation {
    VkDeviceSize size = 0;
    uint32_t memory_type_index = 0;
    VkMemoryPropertyFlags property_flags = 0;
    VkDeviceSize non_coherent_atom_size = 1;
    bool coherent = false;
};

// Vulkan permits only one active mapping per allocation. The allocation_base
// owns the over-allocation used to return an aligned shadow pointer.
struct MappedRange {
    uint64_t memory_id = 0;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void* allocation_base = nullptr;
    void* shadow = nullptr;
    bool coherent = false;
};

struct RemoteInstance {
    RemoteInstance() = default;
    ~RemoteInstance();
    VK_LOADER_DATA loader_data;
    Connection connection;
    std::vector<struct RemotePhysicalDevice*> physical_devices;
    std::unordered_set<std::string> enabled_extensions;
#if defined(_WIN32)
    std::mutex surface_input_mutex;
    std::unordered_map<uint64_t, uintptr_t> surface_windows;
    std::unordered_map<uint64_t, uint32_t> surface_mouse_buttons;
    std::unordered_map<uint64_t, uint32_t> surface_mouse_positions;
    std::unordered_map<uint64_t, std::vector<uint32_t>> surface_pressed_keys;
#endif
};

struct RemotePhysicalDevice {
    VK_LOADER_DATA loader_data;
    RemoteInstance* instance = nullptr;
    uint64_t remote_id = 0;
    VkDeviceSize map_alignment = 1;
    std::vector<VkMemoryPropertyFlags> memory_type_flags;
};

struct RemoteDevice {
    VK_LOADER_DATA loader_data;
    RemoteInstance* instance = nullptr;
    uint64_t remote_id = 0;
    std::unordered_set<std::string> enabled_extensions;
    std::vector<struct RemoteQueue*> queues;
    std::vector<struct RemoteCommandBuffer*> command_buffers;
    std::vector<MappedRange> mapped;
    std::mutex mapped_mutex;
    std::unordered_map<uint64_t, MemoryAllocation> memories;
    VkDeviceSize map_alignment = 1;
    std::vector<VkMemoryPropertyFlags> memory_type_flags;

    // Upload all coherent mappings at implicit visibility points.
    bool flush_coherent_mapped();
    // Upload or download exactly the named ranges. Both operations are
    // synchronous and split transfers into bounded wire messages.
    VkResult flush_mapped(const VkMappedMemoryRange* ranges, uint32_t count);
    VkResult download_mapped(const VkMappedMemoryRange* ranges, uint32_t count);
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

struct DeviceEntry {
    const char* name;
    PFN_vkVoidFunction function;
};
const DeviceEntry* get_device_entries(size_t* count);

}  // namespace remoting
