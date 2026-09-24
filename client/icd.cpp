// Client-side Vulkan ICD: a driver for a machine that has no GPU.
//
// Registers with the real Vulkan loader, so unmodified Vulkan programs drive
// it without knowing a socket is involved. This file only implements the
// loader's negotiation entry points and instance creation/destruction;
// physical-device queries live in physical_device.cpp, the whole device
// layer (vkCreateDevice onward) in device.cpp/memory.cpp/commands.cpp, WSI in
// wsi.cpp, and the name -> function tables those are all reached through in
// entry_table.cpp.

#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <cstdlib>
#include <cstring>

// windows.h's default (non-lean) mode drags in the legacy winsock.h, which
// conflicts with wire.hpp's winsock2.h if windows.h is reached first in this
// translation unit (as it is here, via vulkan.h's VK_USE_PLATFORM_WIN32_KHR
// path below); defining this before that include keeps them from conflicting
// regardless of which of the two ends up included first.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "entry_table.hpp"
#include "marshal.hpp"
#include "remote_objects.hpp"
#include "remoting_commands.inl"
#include "wire.hpp"

namespace {

using remoting::Opcode;
using remoting::Reader;
using remoting::RemoteInstance;
using remoting::RemotePhysicalDevice;
using remoting::Writer;

RemoteInstance* to_instance(VkInstance instance) {
    return reinterpret_cast<RemoteInstance*>(instance);
}

bool round_trip(RemoteInstance* instance, Opcode opcode, const Writer& request,
                std::vector<char>* reply) {
    return instance->connection.round_trip(opcode, request, reply);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char* /*layer*/,
                                                                    uint32_t* count,
                                                                    VkExtensionProperties* props) {
#if defined(_WIN32)
    // A Windows client has no Wayland connection of its own to name a window
    // on (see wsi.cpp); the server turns the hinstance/hwnd this platform's
    // surface carries into its own server-owned window instead (see
    // server/handlers_wsi.cpp's handle_CreateWin32SurfaceKHR), so Win32
    // surface is what this driver offers here rather than Wayland surface.
    static const char* const kNames[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    static const uint32_t kVersions[] = {
        VK_KHR_SURFACE_SPEC_VERSION,
        VK_KHR_WIN32_SURFACE_SPEC_VERSION,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION,
    };
#else
    // The only WSI platform this driver ever offers is Wayland, because that
    // is the only one wayland/proxy_server.hpp's WaylandProxy can name a
    // window on.
    static const char* const kNames[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    static const uint32_t kVersions[] = {
        VK_KHR_SURFACE_SPEC_VERSION,
        VK_KHR_WAYLAND_SURFACE_SPEC_VERSION,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION,
    };
#endif
    constexpr uint32_t kCount = sizeof(kNames) / sizeof(kNames[0]);

    if (props == nullptr) {
        *count = kCount;
        return VK_SUCCESS;
    }
    const uint32_t to_write = *count < kCount ? *count : kCount;
    for (uint32_t i = 0; i < to_write; ++i) {
        memset(&props[i], 0, sizeof(props[i]));
        strncpy(props[i].extensionName, kNames[i], VK_MAX_EXTENSION_NAME_SIZE - 1);
        props[i].specVersion = kVersions[i];
    }
    *count = to_write;
    return to_write < kCount ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo*,
                                              const VkAllocationCallbacks*,
                                              VkInstance* pInstance) {
    const char* host = getenv("VK_REMOTING_HOST");
    if (host == nullptr || *host == '\0') host = "127.0.0.1";

    uint16_t port = 24680;
    if (const char* port_text = getenv("VK_REMOTING_PORT")) {
        const long parsed = strtol(port_text, nullptr, 10);
        if (parsed > 0 && parsed <= 65535) port = static_cast<uint16_t>(parsed);
    }

    auto* instance = new RemoteInstance();
    // Required of every dispatchable handle an ICD creates.
    set_loader_magic_value(instance);

    instance->connection.fd = remoting::connect_to(host, port);
    if (instance->connection.fd == remoting::kInvalidSocket) {
        fprintf(stderr, "vulkan-remoting: no server at %s:%u\n", host, static_cast<unsigned>(port));
        delete instance;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    {
        Writer request;
        request.string(remoting::kWireSchemaRevision);
        request.string(remoting::kRegistrySha256);
        request.string(remoting::kWireAbi);
        request.string(remoting::kCommandSetDigest);
        std::vector<char> reply;
        if (!round_trip(instance, Opcode::Handshake, request, &reply)) {
            fprintf(stderr,
                    "vulkan-remoting: handshake refused; server wire schema, registry, ABI, or "
                    "command set differs (ours: schema=%s registry=%s ABI=%s commands=%s)\n",
                    remoting::kWireSchemaRevision, remoting::kRegistrySha256,
                    remoting::kWireAbi, remoting::kCommandSetDigest);
            remoting::close_socket(instance->connection.fd);
            delete instance;
            return VK_ERROR_INCOMPATIBLE_DRIVER;
        }
    }

    *pInstance = reinterpret_cast<VkInstance>(instance);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance handle, const VkAllocationCallbacks*) {
    if (handle == VK_NULL_HANDLE) return;
    RemoteInstance* instance = to_instance(handle);
    if (instance->connection.fd != remoting::kInvalidSocket) {
        remoting::close_socket(instance->connection.fd);
    }
    for (auto* device : instance->physical_devices) delete device;
    delete instance;
}

}  // namespace

namespace remoting {

const DeviceEntry* get_icd_core_entries(size_t* count) {
    static const DeviceEntry kEntries[] = {
#define D(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name)}
        D(CreateInstance),
        D(DestroyInstance),
        D(EnumerateInstanceExtensionProperties),
#undef D
    };
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

}  // namespace remoting

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion) {
    // Version 5 is the highest this driver implements; the loader lowers its
    // own expectation to whatever is written back here.
    if (*pVersion > 5) *pVersion = 5;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance, const char* pName) {
    return remoting::lookup(pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance, const char* pName) {
    return remoting::lookup(pName);
}

}  // extern "C"
