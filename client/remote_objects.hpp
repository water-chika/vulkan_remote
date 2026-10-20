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
    bool emulated_coherent = false;
};

// Vulkan permits only one active mapping per allocation. The allocation_base
// owns the over-allocation used to return an aligned shadow pointer.
struct MappedRange {
    uint64_t memory_id = 0;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void* allocation_base = nullptr;
    void* shadow = nullptr;
    bool emulated_coherent = false;
    bool transfer_ready = true;
    std::vector<char> baseline;
};

struct RemoteInstance {
    RemoteInstance() = default;
    ~RemoteInstance();
    VK_LOADER_DATA loader_data;
    Connection connection;
    std::vector<struct RemotePhysicalDevice*> physical_devices;
    std::unordered_set<std::string> enabled_extensions;
    uint32_t requested_api_version = VK_API_VERSION_1_0;
    uint32_t server_loader_api_version = VK_API_VERSION_1_0;
    uint32_t negotiated_api_version = VK_API_VERSION_1_0;
    uint32_t capabilities = 0;
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
    uint32_t api_version = VK_API_VERSION_1_0;
    std::vector<VkMemoryPropertyFlags> memory_type_flags;
};

struct DescriptorTemplateMetadata {
    VkDescriptorUpdateTemplateType type = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
};

struct DescriptorResource {
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint64_t resource_id = 0;
};

inline uint64_t descriptor_slot_key(uint32_t binding, uint32_t element) {
    return (static_cast<uint64_t>(binding) << 32) | element;
}

struct ResourceBinding {
    uint64_t memory_id = 0;
    VkDeviceSize offset = 0;
};

struct PendingCoherentWrites {
    uint64_t queue_id = 0;
    uint64_t submission_serial = 0;
    std::unordered_set<uint64_t> memory_ids;
};

struct FramebufferMetadata {
    bool imageless = false;
    std::vector<uint64_t> image_views;
};

struct TimelineSignalWrites {
    uint64_t value = 0;
    uint64_t queue_id = 0;
    uint64_t submission_serial = 0;
    std::unordered_set<uint64_t> memory_ids;
};

struct PendingSemaphoreWait {
    uint64_t value = 0;
    uint64_t queue_id = 0;
    uint64_t submission_serial = 0;
    std::vector<uint64_t> downstream_semaphores;
};

struct SemaphoreProvenance {
    bool timeline = false;
    bool binary_signaled = false;
    uint64_t binary_queue_id = 0;
    uint64_t binary_submission_serial = 0;
    std::unordered_set<uint64_t> binary_writes;
    std::vector<TimelineSignalWrites> timeline_writes;
    std::vector<PendingSemaphoreWait> pending_waits;
};

struct RemoteDevice {
    VK_LOADER_DATA loader_data;
    RemoteInstance* instance = nullptr;
    uint64_t remote_id = 0;
    std::unordered_set<std::string> enabled_extensions;
    uint32_t api_version = VK_API_VERSION_1_0;
    std::vector<struct RemoteQueue*> queues;
    std::vector<struct RemoteCommandBuffer*> command_buffers;
    std::vector<MappedRange> mapped;
    std::mutex mapped_mutex;
    std::mutex metadata_mutex;
    std::unordered_map<uint64_t, MemoryAllocation> memories;
    std::unordered_map<uint64_t, ResourceBinding> buffer_bindings;
    std::unordered_map<uint64_t, ResourceBinding> image_bindings;
    std::unordered_map<uint64_t, std::vector<VkDescriptorSetLayoutBinding>> descriptor_layouts;
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, DescriptorResource>> descriptor_resources;
    std::unordered_map<uint64_t, uint64_t> image_view_images;
    std::unordered_map<uint64_t, uint64_t> buffer_view_buffers;
    std::unordered_map<uint64_t, FramebufferMetadata> framebuffers;
    std::unordered_map<uint64_t, SemaphoreProvenance> semaphores;
    uint64_t next_submission_serial = 1;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> queue_writes;
    std::unordered_map<uint64_t, PendingCoherentWrites> fence_writes;
    std::unordered_map<uint64_t, DescriptorTemplateMetadata> descriptor_templates;
    VkDeviceSize map_alignment = 1;
    std::vector<VkMemoryPropertyFlags> memory_type_flags;

    // Upload or download exactly the named ranges. Both operations are
    // synchronous and split transfers into bounded wire messages.
    VkResult flush_mapped(const VkMappedMemoryRange* ranges, uint32_t count);
    VkResult download_mapped(const VkMappedMemoryRange* ranges, uint32_t count);
    bool publish_coherent_mapped();
    bool acquire_coherent_mapped(const std::unordered_set<uint64_t>* memory_ids = nullptr);
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
    uint64_t command_pool_id = 0;
    std::unordered_map<uint32_t, uint64_t> bound_graphics_sets;
    std::unordered_map<uint32_t, uint64_t> bound_compute_sets;
    std::unordered_set<uint64_t> writable_descriptor_sets;
    bool descriptor_writes_host_visible = false;
    std::unordered_set<uint64_t> host_visible_writes;
    std::unordered_set<uint64_t> pending_writes;
};

struct DeviceEntry {
    const char* name;
    PFN_vkVoidFunction function;
};
const DeviceEntry* get_device_entries(size_t* count);

}  // namespace remoting
