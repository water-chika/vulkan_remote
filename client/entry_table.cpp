// The only file that does string comparison / lookup for the client-side
// ICD's name -> function tables.
//
// Every other client file only ever exposes a static table of its own
// {name, function pointer} pairs (see entry_table.hpp); this file concatenates
// them, once, into the two tables the loader actually walks:
// GetInstanceProcAddr's (covering instance and physical-device level
// functions) and GetDeviceProcAddr's (covering everything reachable once a
// VkDevice exists). Concatenation order does not affect behaviour - lookup is
// a linear scan by name, and no two providers define the same name - so the
// grouping below simply follows the file split rather than the original
// single-table order.

#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

#include "entry_table.hpp"
#include "remote_objects.hpp"

namespace remoting {
namespace {

void append(std::vector<DeviceEntry>* out, const DeviceEntry* entries, size_t count) {
    out->insert(out->end(), entries, entries + count);
}

}  // namespace

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice, const char* pName) {
    if (pName == nullptr) return nullptr;
    size_t count = 0;
    const DeviceEntry* entries = get_device_entries(&count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, pName) == 0) return entries[i].function;
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char* pName);

namespace {

#define ENTRY(name) \
    { "vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name) }

const std::vector<DeviceEntry>& instance_entries() {
    static const std::vector<DeviceEntry> table = [] {
        std::vector<DeviceEntry> v;
        size_t count = 0;
        const DeviceEntry* entries = get_icd_core_entries(&count);
        append(&v, entries, count);
        entries = get_physical_device_entries(&count);
        append(&v, entries, count);
        entries = get_wsi_instance_entries(&count);
        append(&v, entries, count);
        v.push_back(ENTRY(GetDeviceProcAddr));
        v.push_back(ENTRY(GetInstanceProcAddr));
        return v;
    }();
    return table;
}

#undef ENTRY

}  // namespace

PFN_vkVoidFunction lookup(const char* name) {
    if (name == nullptr) return nullptr;
    for (const DeviceEntry& entry : instance_entries()) {
        if (strcmp(entry.name, name) == 0) return entry.function;
    }
    // Device-level functions are reachable through vkGetInstanceProcAddr too:
    // some callers resolve them that way before they have a VkDevice, and the
    // loader tolerates a driver answering either way.
    size_t count = 0;
    const DeviceEntry* entries = get_device_entries(&count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) return entries[i].function;
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char* pName) {
    return lookup(pName);
}

const DeviceEntry* get_device_entries(size_t* count) {
    static const std::vector<DeviceEntry> table = [] {
        std::vector<DeviceEntry> v;
        size_t n = 0;
        const DeviceEntry* entries = get_device_core_entries(&n);
        append(&v, entries, n);
        entries = get_memory_entries(&n);
        append(&v, entries, n);
        entries = get_command_entries(&n);
        append(&v, entries, n);
        entries = get_wsi_device_entries(&n);
        append(&v, entries, n);
        return v;
    }();
    *count = table.size();
    return table.data();
}

}  // namespace remoting
