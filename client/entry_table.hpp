#pragma once

// Declares the per-file accessor functions that entry_table.cpp concatenates
// into the instance-level and device-level lookup tables.
//
// Each provider file (icd.cpp, physical_device.cpp, device.cpp, memory.cpp,
// commands.cpp, wsi.cpp) keeps its own vkCreate*/vkCmd*/... implementations in
// an anonymous namespace, exactly as this project always has, so that adding
// one never risks a name clashing with another file's helper of the same
// name. That means entry_table.cpp - which is the only place allowed to do
// name lookup - cannot refer to those functions by name across a translation
// unit boundary; it can only see them through a plain function pointer. Each
// provider therefore exposes one small accessor, defined at namespace scope
// (not anonymous) in its own file, that hands back a pointer to its own
// static table of {name, function pointer} pairs built from its anonymous
// namespace. remoting::DeviceEntry (see remote_objects.hpp) is reused here
// for both instance-level and device-level tables since the two are the same
// {name, function} shape; only entry_table.cpp treats them as two logically
// separate tables.

#include <cstddef>

#include "remote_objects.hpp"

namespace remoting {

// Instance-level (used from icd.cpp's own core and from
// vk_icdGetInstanceProcAddr/vk_icdGetPhysicalDeviceProcAddr).
const DeviceEntry* get_icd_core_entries(size_t* count);
const DeviceEntry* get_physical_device_entries(size_t* count);
const DeviceEntry* get_wsi_instance_entries(size_t* count);

// Device-level (used from vkGetDeviceProcAddr, reachable via
// get_device_entries() in remote_objects.hpp).
const DeviceEntry* get_device_core_entries(size_t* count);
const DeviceEntry* get_memory_entries(size_t* count);
const DeviceEntry* get_command_entries(size_t* count);
const DeviceEntry* get_wsi_device_entries(size_t* count);

// The single place that does string comparison against either table; see
// entry_table.cpp.
PFN_vkVoidFunction lookup(const char* name);

}  // namespace remoting
