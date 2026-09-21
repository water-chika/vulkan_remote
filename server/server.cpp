// Server half of Vulkan-over-TCP: executes commands on the machine that owns
// the GPU and returns results to a client that has none.
//
// One Server::serve(fd) call handles one client end-to-end, and owns that
// client's ObjectTables: a second client gets its own table, its own ids.
//
// Recording commands (every vkCmd*, plus vkUpdateDescriptorSets, vkDestroy*,
// vkFreeMemory and vkResetFences) arrive with no reply expected: the client
// sent them with send_oneway and has already moved on. A failure in one of
// those cannot be reported when it happens, so this file counts them in
// m_oneway_errors and hands the count back in the next reply that does wait
// for one (vkQueueSubmit, vkDeviceWaitIdle) - see remote_objects.hpp on the
// client side for where that count is read and printed.

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "marshal.hpp"
#include "objects.hpp"
#include "remoting_commands.inl"
#include "wire.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

// Set to the current connection's oneway_errors counter for the duration of
// Server::serve(); the debug callback runs on this thread synchronously
// inside whatever Vulkan call triggered it, so this is safe without locking.
uint32_t* g_current_error_counter = nullptr;

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*) {
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) return VK_FALSE;
    fprintf(stderr, "server: validation: %s\n", data->pMessage);
    // A marshalling bug looks like a type-correct but malformed call from
    // here, which is exactly what this layer is for: count it so a client
    // sees the failure too instead of it being silent on this side only.
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && g_current_error_counter) {
        ++*g_current_error_counter;
    }
    return VK_FALSE;
}

class Server {
   public:
    bool init_vulkan(bool validate) {
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "vulkan-remoting-server";
        app_info.apiVersion = VK_API_VERSION_1_1;

        std::vector<const char*> layers;
        std::vector<const char*> extensions;
        if (validate) {
            uint32_t layer_count = 0;
            vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
            std::vector<VkLayerProperties> avail(layer_count);
            vkEnumerateInstanceLayerProperties(&layer_count, avail.data());
            bool found = false;
            for (const auto& l : avail) {
                if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) found = true;
            }
            if (found) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                fprintf(stderr, "server: VK_LAYER_KHRONOS_validation enabled\n");
            } else {
                fprintf(stderr,
                        "server: --validate requested but VK_LAYER_KHRONOS_validation is not "
                        "available; continuing without it\n");
            }
        }

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
        create_info.ppEnabledLayerNames = layers.data();
        create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();

        const VkResult result = vkCreateInstance(&create_info, nullptr, &m_instance);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "server: vkCreateInstance failed (%d)\n", result);
            return false;
        }

        if (!extensions.empty()) {
            auto create_fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create_fn) {
                VkDebugUtilsMessengerCreateInfoEXT dbg_info{};
                dbg_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
                dbg_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                dbg_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                dbg_info.pfnUserCallback = debug_callback;
                create_fn(m_instance, &dbg_info, nullptr, &m_messenger);
            }
        }

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
        m_physical_devices.resize(count);
        if (count > 0) {
            vkEnumeratePhysicalDevices(m_instance, &count, m_physical_devices.data());
        }

        fprintf(stderr, "server: %u physical device(s)\n", count);
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(m_physical_devices[i], &props);
            fprintf(stderr, "server:   [%u] %s\n", i, props.deviceName);
        }
        return true;
    }

    ~Server() {
        if (m_messenger != VK_NULL_HANDLE) {
            auto destroy_fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_fn) destroy_fn(m_instance, m_messenger, nullptr);
        }
        if (m_instance != VK_NULL_HANDLE) vkDestroyInstance(m_instance, nullptr);
    }

    // Handles cross the wire as indices, never as pointers. A VkPhysicalDevice
    // is a host pointer; sending its bits would be meaningless remotely and
    // would leak an address, so the client only ever sees 1-based indices.
    VkPhysicalDevice physical_device_from_id(uint64_t id) const {
        if (id == 0 || id > m_physical_devices.size()) return VK_NULL_HANDLE;
        return m_physical_devices[id - 1];
    }

    void serve(int fd);

   private:
    void reply(int fd, remoting::Opcode opcode, const remoting::Writer& writer) {
        remoting::send_message(fd, static_cast<uint32_t>(opcode), writer.data());
    }

    void reply_status(int fd, remoting::Opcode opcode, remoting::Status status) {
        remoting::Writer writer;
        writer.u32(static_cast<uint32_t>(status));
        reply(fd, opcode, writer);
    }

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> m_physical_devices;
};

// Shared by every case handler below; declared once per connection in
// Server::serve and threaded through by reference.
struct Ctx {
    int fd;
    remoting::Reader& reader;
    remoting::Writer& writer;
    remoting::ObjectTables& tables;
    uint32_t& oneway_errors;
};

using remoting::Arena;
using remoting::Status;

void mark_oneway_error(Ctx& c) { ++c.oneway_errors; }

// ---------------------------------------------------------------------------
// Device / queue
// ---------------------------------------------------------------------------

void handle_CreateDevice(Ctx& c, Server& server, remoting::Opcode opcode) {
    const uint64_t physdev_id = c.reader.handle();
    VkPhysicalDevice physdev = server.physical_device_from_id(physdev_id);
    Arena arena;
    VkDeviceCreateInfo info{};
    if (!c.reader.ok() || physdev == VK_NULL_HANDLE ||
        !remoting::read_DeviceCreateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    VkDevice device = VK_NULL_HANDLE;
    const VkResult result = vkCreateDevice(physdev, &info, nullptr, &device);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.devices.add(device) : 0);
}

void handle_DestroyDevice(Ctx& c) {
    const uint64_t id = c.reader.handle();
    VkDevice device = c.tables.devices.take(id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkDestroyDevice(device, nullptr);
}

void handle_GetDeviceQueue(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t family = c.reader.u32();
    const uint32_t index = c.reader.u32();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, index, &queue);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.handle(c.tables.queues.add(queue));
}

void handle_QueueSubmit(Ctx& c) {
    const uint64_t queue_id = c.reader.handle();
    VkQueue queue = c.tables.queues.get(queue_id);
    const uint64_t fence_id = c.reader.handle();
    VkFence fence = c.tables.fence(fence_id);
    const uint32_t submit_count = c.reader.u32();

    std::vector<VkSubmitInfo> submits(submit_count);
    // Kept alive until vkQueueSubmit returns; one entry per submit so each
    // submit's arrays do not alias another's.
    std::vector<std::vector<VkSemaphore>> wait_semaphores(submit_count);
    std::vector<std::vector<VkPipelineStageFlags>> wait_stages(submit_count);
    std::vector<std::vector<VkCommandBuffer>> command_buffers(submit_count);
    std::vector<std::vector<VkSemaphore>> signal_semaphores(submit_count);

    bool ok = c.reader.ok() && queue != VK_NULL_HANDLE;
    for (uint32_t i = 0; ok && i < submit_count; ++i) {
        VkSubmitInfo& s = submits[i];
        s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        const uint32_t wait_count = c.reader.u32();
        wait_semaphores[i].resize(wait_count);
        wait_stages[i].resize(wait_count);
        for (uint32_t j = 0; j < wait_count && ok; ++j) {
            wait_semaphores[i][j] = c.tables.semaphore(c.reader.handle());
            wait_stages[i][j] = c.reader.u32();
        }
        s.waitSemaphoreCount = wait_count;
        s.pWaitSemaphores = wait_count ? wait_semaphores[i].data() : nullptr;
        s.pWaitDstStageMask = wait_count ? wait_stages[i].data() : nullptr;

        const uint32_t cb_count = c.reader.u32();
        command_buffers[i].resize(cb_count);
        for (uint32_t j = 0; j < cb_count && ok; ++j) {
            command_buffers[i][j] = c.tables.command_buffer(c.reader.handle());
        }
        s.commandBufferCount = cb_count;
        s.pCommandBuffers = cb_count ? command_buffers[i].data() : nullptr;

        const uint32_t signal_count = c.reader.u32();
        signal_semaphores[i].resize(signal_count);
        for (uint32_t j = 0; j < signal_count && ok; ++j) {
            signal_semaphores[i][j] = c.tables.semaphore(c.reader.handle());
        }
        s.signalSemaphoreCount = signal_count;
        s.pSignalSemaphores = signal_count ? signal_semaphores[i].data() : nullptr;

        ok = c.reader.ok();
    }

    if (!ok) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    const VkResult result = vkQueueSubmit(queue, submit_count, submits.data(), fence);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(c.oneway_errors);
    c.oneway_errors = 0;
}

void handle_QueueWaitIdle(Ctx& c) {
    const uint64_t queue_id = c.reader.handle();
    VkQueue queue = c.tables.queues.get(queue_id);
    if (!c.reader.ok() || queue == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkQueueWaitIdle(queue);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
}

void handle_DeviceWaitIdle(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkDeviceWaitIdle(device);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(c.oneway_errors);
    c.oneway_errors = 0;
}

// ---------------------------------------------------------------------------
// Sync
// ---------------------------------------------------------------------------

void handle_CreateFence(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkFenceCreateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_FenceCreateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkFence fence = VK_NULL_HANDLE;
    const VkResult result = vkCreateFence(device, &info, nullptr, &fence);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.fences.add(fence) : 0);
}

void handle_DestroyFence(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t fence_id = c.reader.handle();
    VkFence fence = c.tables.fences.take(fence_id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkDestroyFence(device, fence, nullptr);
}

void handle_ResetFences(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t count = c.reader.u32();
    std::vector<VkFence> fences(count);
    for (uint32_t i = 0; i < count; ++i) fences[i] = c.tables.fence(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    if (vkResetFences(device, count, fences.data()) != VK_SUCCESS) mark_oneway_error(c);
}

void handle_GetFenceStatus(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t fence_id = c.reader.handle();
    VkFence fence = c.tables.fence(fence_id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkGetFenceStatus(device, fence);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
}

void handle_WaitForFences(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const bool wait_all = c.reader.u32() != 0;
    const uint32_t count = c.reader.u32();
    std::vector<VkFence> fences(count);
    for (uint32_t i = 0; i < count; ++i) fences[i] = c.tables.fence(c.reader.handle());
    const uint64_t timeout = c.reader.u64();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkWaitForFences(device, count, fences.data(), wait_all, timeout);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
}

void handle_CreateSemaphore(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkSemaphoreCreateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_SemaphoreCreateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult result = vkCreateSemaphore(device, &info, nullptr, &semaphore);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.semaphores.add(semaphore) : 0);
}

void handle_DestroySemaphore(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t id = c.reader.handle();
    VkSemaphore semaphore = c.tables.semaphores.take(id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkDestroySemaphore(device, semaphore, nullptr);
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

void handle_AllocateMemory(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkMemoryAllocateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_MemoryAllocateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = vkAllocateMemory(device, &info, nullptr, &memory);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.memories.add(memory) : 0);
}

void handle_FreeMemory(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint64_t id = c.reader.handle();
    VkDeviceMemory memory = c.tables.memories.take(id);
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkFreeMemory(device, memory, nullptr);
}

void handle_FlushMappedMemory(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t count = c.reader.u32();
    bool ok = c.reader.ok() && device != VK_NULL_HANDLE;
    for (uint32_t i = 0; ok && i < count; ++i) {
        const uint64_t memory_id = c.reader.handle();
        const uint64_t offset = c.reader.u64();
        const uint64_t size = c.reader.u64();
        std::vector<char> bytes;
        if (!c.reader.bytes(&bytes) || bytes.size() != size) {
            ok = false;
            break;
        }
        VkDeviceMemory memory = c.tables.memory(memory_id);
        if (memory == VK_NULL_HANDLE) {
            ok = false;
            break;
        }
        void* mapped = nullptr;
        // Mapped only for the duration of this copy; the shadow-buffer
        // scheme never keeps a server-side mapping open between messages.
        if (vkMapMemory(device, memory, offset, size, 0, &mapped) != VK_SUCCESS) {
            ok = false;
            break;
        }
        memcpy(mapped, bytes.data(), bytes.size());
        vkUnmapMemory(device, memory);
    }
    if (!ok) mark_oneway_error(c);
}

void handle_DownloadMappedMemory(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t count = c.reader.u32();

    struct Range {
        uint64_t memory_id;
        uint64_t offset;
        uint64_t size;
    };
    std::vector<Range> ranges(count);
    for (uint32_t i = 0; i < count; ++i) {
        ranges[i].memory_id = c.reader.handle();
        ranges[i].offset = c.reader.u64();
        ranges[i].size = c.reader.u64();
    }
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    for (const Range& range : ranges) {
        VkDeviceMemory memory = c.tables.memory(range.memory_id);
        std::vector<char> data(static_cast<size_t>(range.size));
        if (memory != VK_NULL_HANDLE) {
            void* mapped = nullptr;
            if (vkMapMemory(device, memory, range.offset, range.size, 0, &mapped) == VK_SUCCESS) {
                memcpy(data.data(), mapped, data.size());
                vkUnmapMemory(device, memory);
            }
        }
        c.writer.bytes(data.data(), data.size());
    }
}

void handle_BindBufferMemory(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkBuffer buffer = c.tables.buffer(c.reader.handle());
    VkDeviceMemory memory = c.tables.memory(c.reader.handle());
    const uint64_t offset = c.reader.u64();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkBindBufferMemory(device, buffer, memory, offset);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
}

void handle_BindImageMemory(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkImage image = c.tables.image(c.reader.handle());
    VkDeviceMemory memory = c.tables.memory(c.reader.handle());
    const uint64_t offset = c.reader.u64();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    const VkResult result = vkBindImageMemory(device, image, memory, offset);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
}

void write_memory_requirements(remoting::Writer& w, const VkMemoryRequirements& r) {
    w.u64(r.size);
    w.u64(r.alignment);
    w.u32(r.memoryTypeBits);
}

void handle_GetBufferMemoryRequirements(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkBuffer buffer = c.tables.buffer(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device, buffer, &reqs);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    write_memory_requirements(c.writer, reqs);
}

void handle_GetImageMemoryRequirements(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkImage image = c.tables.image(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(device, image, &reqs);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    write_memory_requirements(c.writer, reqs);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

#define SERVER_CREATE(HandleName, VkType, InfoType, ReadFn, VkCreateFn, table)              \
    void handle_Create##HandleName(Ctx& c) {                                                \
        const uint64_t device_id = c.reader.handle();                                       \
        VkDevice device = c.tables.devices.get(device_id);                                  \
        Arena arena;                                                                        \
        InfoType info{};                                                                    \
        if (!c.reader.ok() || device == VK_NULL_HANDLE ||                                   \
            !ReadFn(c.reader, arena, c.tables, &info)) {                                    \
            c.writer.u32(static_cast<uint32_t>(Status::DecodeError));                       \
            return;                                                                         \
        }                                                                                   \
        VkType obj = VK_NULL_HANDLE;                                                        \
        const VkResult result = VkCreateFn(device, &info, nullptr, &obj);                   \
        c.writer.u32(static_cast<uint32_t>(Status::Ok));                                    \
        c.writer.i32(result);                                                               \
        c.writer.handle(result == VK_SUCCESS ? c.tables.table.add(obj) : 0);                \
    }

#define SERVER_DESTROY(HandleName, VkType, VkDestroyFn, table)               \
    void handle_Destroy##HandleName(Ctx& c) {                                \
        const uint64_t device_id = c.reader.handle();                       \
        VkDevice device = c.tables.devices.get(device_id);                  \
        const uint64_t id = c.reader.handle();                              \
        VkType obj = c.tables.table.take(id);                               \
        if (!c.reader.ok() || device == VK_NULL_HANDLE) {                   \
            mark_oneway_error(c);                                           \
            return;                                                         \
        }                                                                   \
        VkDestroyFn(device, obj, nullptr);                                  \
    }

SERVER_CREATE(Buffer, VkBuffer, VkBufferCreateInfo, remoting::read_BufferCreateInfo,
              vkCreateBuffer, buffers)
SERVER_DESTROY(Buffer, VkBuffer, vkDestroyBuffer, buffers)

SERVER_CREATE(Image, VkImage, VkImageCreateInfo, remoting::read_ImageCreateInfo, vkCreateImage,
              images)
SERVER_DESTROY(Image, VkImage, vkDestroyImage, images)

SERVER_CREATE(ImageView, VkImageView, VkImageViewCreateInfo, remoting::read_ImageViewCreateInfo,
              vkCreateImageView, image_views)
SERVER_DESTROY(ImageView, VkImageView, vkDestroyImageView, image_views)

SERVER_CREATE(Sampler, VkSampler, VkSamplerCreateInfo, remoting::read_SamplerCreateInfo,
              vkCreateSampler, samplers)
SERVER_DESTROY(Sampler, VkSampler, vkDestroySampler, samplers)

SERVER_CREATE(ShaderModule, VkShaderModule, VkShaderModuleCreateInfo,
              remoting::read_ShaderModuleCreateInfo, vkCreateShaderModule, shader_modules)
SERVER_DESTROY(ShaderModule, VkShaderModule, vkDestroyShaderModule, shader_modules)

SERVER_CREATE(PipelineCache, VkPipelineCache, VkPipelineCacheCreateInfo,
              remoting::read_PipelineCacheCreateInfo, vkCreatePipelineCache, pipeline_caches)
SERVER_DESTROY(PipelineCache, VkPipelineCache, vkDestroyPipelineCache, pipeline_caches)

SERVER_CREATE(PipelineLayout, VkPipelineLayout, VkPipelineLayoutCreateInfo,
              remoting::read_PipelineLayoutCreateInfo, vkCreatePipelineLayout, pipeline_layouts)
SERVER_DESTROY(PipelineLayout, VkPipelineLayout, vkDestroyPipelineLayout, pipeline_layouts)

SERVER_DESTROY(Pipeline, VkPipeline, vkDestroyPipeline, pipelines)

SERVER_CREATE(RenderPass, VkRenderPass, VkRenderPassCreateInfo, remoting::read_RenderPassCreateInfo,
              vkCreateRenderPass, render_passes)
SERVER_DESTROY(RenderPass, VkRenderPass, vkDestroyRenderPass, render_passes)

SERVER_CREATE(Framebuffer, VkFramebuffer, VkFramebufferCreateInfo,
              remoting::read_FramebufferCreateInfo, vkCreateFramebuffer, framebuffers)
SERVER_DESTROY(Framebuffer, VkFramebuffer, vkDestroyFramebuffer, framebuffers)

SERVER_DESTROY(DescriptorSetLayout, VkDescriptorSetLayout, vkDestroyDescriptorSetLayout,
               descriptor_set_layouts)

SERVER_CREATE(DescriptorPool, VkDescriptorPool, VkDescriptorPoolCreateInfo,
              remoting::read_DescriptorPoolCreateInfo, vkCreateDescriptorPool, descriptor_pools)
SERVER_DESTROY(DescriptorPool, VkDescriptorPool, vkDestroyDescriptorPool, descriptor_pools)

SERVER_CREATE(CommandPool, VkCommandPool, VkCommandPoolCreateInfo,
              remoting::read_CommandPoolCreateInfo, vkCreateCommandPool, command_pools)
SERVER_DESTROY(CommandPool, VkCommandPool, vkDestroyCommandPool, command_pools)

#undef SERVER_CREATE
#undef SERVER_DESTROY

void handle_CreateDescriptorSetLayout(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkDescriptorSetLayoutCreateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_DescriptorSetLayoutCreateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    const VkResult result = vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.handle(result == VK_SUCCESS ? c.tables.descriptor_set_layouts.add(layout) : 0);
}

void handle_GetImageSubresourceLayout(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkImage image = c.tables.image(c.reader.handle());
    VkImageSubresource sub{};
    sub.aspectMask = c.reader.u32();
    sub.mipLevel = c.reader.u32();
    sub.arrayLayer = c.reader.u32();
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(device, image, &sub, &layout);
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.u64(layout.offset);
    c.writer.u64(layout.size);
    c.writer.u64(layout.rowPitch);
    c.writer.u64(layout.arrayPitch);
    c.writer.u64(layout.depthPitch);
}

// ---------------------------------------------------------------------------
// Pipeline (graphics pipelines: many create infos, many results)
// ---------------------------------------------------------------------------

void handle_CreateGraphicsPipelines(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkPipelineCache cache = c.tables.pipeline_cache(c.reader.handle());
    const uint32_t count = c.reader.u32();

    Arena arena;
    std::vector<VkGraphicsPipelineCreateInfo> infos(count);
    bool ok = c.reader.ok() && device != VK_NULL_HANDLE;
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_GraphicsPipelineCreateInfo(c.reader, arena, c.tables, &infos[i]);
    }
    if (!ok) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    std::vector<VkPipeline> pipelines(count, VK_NULL_HANDLE);
    const VkResult result =
        count ? vkCreateGraphicsPipelines(device, cache, count, infos.data(), nullptr,
                                           pipelines.data())
              : VK_SUCCESS;

    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        c.writer.handle(pipelines[i] != VK_NULL_HANDLE ? c.tables.pipelines.add(pipelines[i]) : 0);
    }
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

void handle_AllocateDescriptorSets(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkDescriptorSetAllocateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_DescriptorSetAllocateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    std::vector<VkDescriptorSet> sets(info.descriptorSetCount);
    const VkResult result = vkAllocateDescriptorSets(device, &info, sets.data());
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(info.descriptorSetCount);
    for (uint32_t i = 0; i < info.descriptorSetCount; ++i) {
        c.writer.handle(result == VK_SUCCESS ? c.tables.descriptor_sets.add(sets[i]) : 0);
    }
}

void handle_UpdateDescriptorSets(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    const uint32_t write_count = c.reader.u32();
    Arena arena;
    std::vector<VkWriteDescriptorSet> writes(write_count);
    bool ok = c.reader.ok() && device != VK_NULL_HANDLE;
    for (uint32_t i = 0; ok && i < write_count; ++i) {
        ok = remoting::read_WriteDescriptorSet(c.reader, arena, c.tables, &writes[i]);
    }
    const uint32_t copy_count = ok ? c.reader.u32() : 0;
    std::vector<VkCopyDescriptorSet> copies(copy_count);
    for (uint32_t i = 0; ok && i < copy_count; ++i) {
        ok = remoting::read_CopyDescriptorSet(c.reader, arena, c.tables, &copies[i]);
    }
    if (!ok || !c.reader.ok()) {
        mark_oneway_error(c);
        return;
    }
    vkUpdateDescriptorSets(device, write_count, writes.data(), copy_count, copies.data());
}

// ---------------------------------------------------------------------------
// Command pools / buffers
// ---------------------------------------------------------------------------

void handle_AllocateCommandBuffers(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    Arena arena;
    VkCommandBufferAllocateInfo info{};
    if (!c.reader.ok() || device == VK_NULL_HANDLE ||
        !remoting::read_CommandBufferAllocateInfo(c.reader, arena, c.tables, &info)) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }
    std::vector<VkCommandBuffer> cbs(info.commandBufferCount);
    const VkResult result = vkAllocateCommandBuffers(device, &info, cbs.data());
    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(result);
    c.writer.u32(info.commandBufferCount);
    for (uint32_t i = 0; i < info.commandBufferCount; ++i) {
        c.writer.handle(result == VK_SUCCESS ? c.tables.command_buffers.add(cbs[i]) : 0);
    }
}

void handle_FreeCommandBuffers(Ctx& c) {
    const uint64_t device_id = c.reader.handle();
    VkDevice device = c.tables.devices.get(device_id);
    VkCommandPool pool = c.tables.command_pool(c.reader.handle());
    const uint32_t count = c.reader.u32();
    std::vector<VkCommandBuffer> cbs(count);
    for (uint32_t i = 0; i < count; ++i) cbs[i] = c.tables.command_buffers.take(c.reader.handle());
    if (!c.reader.ok() || device == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkFreeCommandBuffers(device, pool, count, cbs.data());
}

void handle_BeginCommandBuffer(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    Arena arena;
    VkCommandBufferBeginInfo info{};
    if (!c.reader.ok() || cb == VK_NULL_HANDLE ||
        !remoting::read_CommandBufferBeginInfo(c.reader, arena, c.tables, &info)) {
        mark_oneway_error(c);
        return;
    }
    if (vkBeginCommandBuffer(cb, &info) != VK_SUCCESS) mark_oneway_error(c);
}

void handle_EndCommandBuffer(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) mark_oneway_error(c);
}

void handle_ResetCommandBuffer(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t flags = c.reader.u32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    if (vkResetCommandBuffer(cb, flags) != VK_SUCCESS) mark_oneway_error(c);
}

// ---------------------------------------------------------------------------
// Recording (all fire-and-forget)
// ---------------------------------------------------------------------------

void handle_CmdBeginRenderPass(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    Arena arena;
    VkRenderPassBeginInfo info{};
    if (!c.reader.ok() || cb == VK_NULL_HANDLE ||
        !remoting::read_RenderPassBeginInfo(c.reader, arena, c.tables, &info)) {
        mark_oneway_error(c);
        return;
    }
    const int32_t contents = c.reader.i32();
    if (!c.reader.ok()) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBeginRenderPass(cb, &info, static_cast<VkSubpassContents>(contents));
}

void handle_CmdEndRenderPass(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdEndRenderPass(cb);
}

void handle_CmdBindPipeline(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const int32_t bind_point = c.reader.i32();
    VkPipeline pipeline = c.tables.pipeline(c.reader.handle());
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBindPipeline(cb, static_cast<VkPipelineBindPoint>(bind_point), pipeline);
}

void handle_CmdBindDescriptorSets(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const int32_t bind_point = c.reader.i32();
    VkPipelineLayout layout = c.tables.pipeline_layout(c.reader.handle());
    const uint32_t first_set = c.reader.u32();
    const uint32_t count = c.reader.u32();
    std::vector<VkDescriptorSet> sets(count);
    for (uint32_t i = 0; i < count; ++i) sets[i] = c.tables.descriptor_set(c.reader.handle());
    const uint32_t dyn_count = c.reader.u32();
    std::vector<uint32_t> offsets(dyn_count);
    for (uint32_t i = 0; i < dyn_count; ++i) offsets[i] = c.reader.u32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBindDescriptorSets(cb, static_cast<VkPipelineBindPoint>(bind_point), layout, first_set,
                            count, sets.data(), dyn_count, offsets.data());
}

void handle_CmdBindVertexBuffers(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t first = c.reader.u32();
    const uint32_t count = c.reader.u32();
    std::vector<VkBuffer> buffers(count);
    std::vector<VkDeviceSize> offsets(count);
    for (uint32_t i = 0; i < count; ++i) {
        buffers[i] = c.tables.buffer(c.reader.handle());
        offsets[i] = c.reader.u64();
    }
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBindVertexBuffers(cb, first, count, buffers.data(), offsets.data());
}

void handle_CmdBindIndexBuffer(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkBuffer buffer = c.tables.buffer(c.reader.handle());
    const uint64_t offset = c.reader.u64();
    const int32_t index_type = c.reader.i32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdBindIndexBuffer(cb, buffer, offset, static_cast<VkIndexType>(index_type));
}

void handle_CmdSetViewport(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t first = c.reader.u32();
    const uint32_t count = c.reader.u32();
    Arena arena;
    std::vector<VkViewport> viewports(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_Viewport(c.reader, arena, c.tables, &viewports[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdSetViewport(cb, first, count, viewports.data());
}

void handle_CmdSetScissor(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t first = c.reader.u32();
    const uint32_t count = c.reader.u32();
    Arena arena;
    std::vector<VkRect2D> scissors(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_Rect2D(c.reader, arena, c.tables, &scissors[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdSetScissor(cb, first, count, scissors.data());
}

void handle_CmdDraw(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t vertex_count = c.reader.u32();
    const uint32_t instance_count = c.reader.u32();
    const uint32_t first_vertex = c.reader.u32();
    const uint32_t first_instance = c.reader.u32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdDraw(cb, vertex_count, instance_count, first_vertex, first_instance);
}

void handle_CmdDrawIndexed(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t index_count = c.reader.u32();
    const uint32_t instance_count = c.reader.u32();
    const uint32_t first_index = c.reader.u32();
    const int32_t vertex_offset = c.reader.i32();
    const uint32_t first_instance = c.reader.u32();
    if (!c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdDrawIndexed(cb, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void handle_CmdPipelineBarrier(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    const uint32_t src_stage = c.reader.u32();
    const uint32_t dst_stage = c.reader.u32();
    const uint32_t dep_flags = c.reader.u32();

    Arena arena;
    const uint32_t mem_count = c.reader.u32();
    std::vector<VkMemoryBarrier> mem_barriers(mem_count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < mem_count; ++i) {
        ok = remoting::read_MemoryBarrier(c.reader, arena, c.tables, &mem_barriers[i]);
    }
    const uint32_t buf_count = ok ? c.reader.u32() : 0;
    std::vector<VkBufferMemoryBarrier> buf_barriers(buf_count);
    for (uint32_t i = 0; ok && i < buf_count; ++i) {
        ok = remoting::read_BufferMemoryBarrier(c.reader, arena, c.tables, &buf_barriers[i]);
    }
    const uint32_t img_count = ok ? c.reader.u32() : 0;
    std::vector<VkImageMemoryBarrier> img_barriers(img_count);
    for (uint32_t i = 0; ok && i < img_count; ++i) {
        ok = remoting::read_ImageMemoryBarrier(c.reader, arena, c.tables, &img_barriers[i]);
    }

    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, dep_flags, mem_count, mem_barriers.data(),
                         buf_count, buf_barriers.data(), img_count, img_barriers.data());
}

void handle_CmdCopyBuffer(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkBuffer src = c.tables.buffer(c.reader.handle());
    VkBuffer dst = c.tables.buffer(c.reader.handle());
    const uint32_t count = c.reader.u32();
    Arena arena;
    std::vector<VkBufferCopy> regions(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_BufferCopy(c.reader, arena, c.tables, &regions[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdCopyBuffer(cb, src, dst, count, regions.data());
}

void handle_CmdCopyBufferToImage(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkBuffer src = c.tables.buffer(c.reader.handle());
    VkImage dst = c.tables.image(c.reader.handle());
    const int32_t layout = c.reader.i32();
    const uint32_t count = c.reader.u32();
    Arena arena;
    std::vector<VkBufferImageCopy> regions(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_BufferImageCopy(c.reader, arena, c.tables, &regions[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdCopyBufferToImage(cb, src, dst, static_cast<VkImageLayout>(layout), count,
                           regions.data());
}

void handle_CmdCopyImageToBuffer(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkImage src = c.tables.image(c.reader.handle());
    const int32_t layout = c.reader.i32();
    VkBuffer dst = c.tables.buffer(c.reader.handle());
    const uint32_t count = c.reader.u32();
    Arena arena;
    std::vector<VkBufferImageCopy> regions(count);
    bool ok = c.reader.ok();
    for (uint32_t i = 0; ok && i < count; ++i) {
        ok = remoting::read_BufferImageCopy(c.reader, arena, c.tables, &regions[i]);
    }
    if (!ok || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdCopyImageToBuffer(cb, src, static_cast<VkImageLayout>(layout), dst, count,
                           regions.data());
}

void handle_CmdClearColorImage(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkImage image = c.tables.image(c.reader.handle());
    const int32_t layout = c.reader.i32();
    std::vector<char> color_bytes;
    c.reader.bytes(&color_bytes);
    const uint32_t count = c.reader.u32();
    std::vector<VkImageSubresourceRange> ranges(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<char> raw;
        if (!c.reader.bytes(&raw) || raw.size() != sizeof(VkImageSubresourceRange)) continue;
        memcpy(&ranges[i], raw.data(), sizeof(VkImageSubresourceRange));
    }
    if (!c.reader.ok() || cb == VK_NULL_HANDLE || color_bytes.size() != sizeof(VkClearColorValue)) {
        mark_oneway_error(c);
        return;
    }
    VkClearColorValue color{};
    memcpy(&color, color_bytes.data(), sizeof(color));
    vkCmdClearColorImage(cb, image, static_cast<VkImageLayout>(layout), &color, count,
                        ranges.data());
}

void handle_CmdPushConstants(Ctx& c) {
    VkCommandBuffer cb = c.tables.command_buffer(c.reader.handle());
    VkPipelineLayout layout = c.tables.pipeline_layout(c.reader.handle());
    const uint32_t stage_flags = c.reader.u32();
    const uint32_t offset = c.reader.u32();
    std::vector<char> values;
    if (!c.reader.bytes(&values) || !c.reader.ok() || cb == VK_NULL_HANDLE) {
        mark_oneway_error(c);
        return;
    }
    vkCmdPushConstants(cb, layout, stage_flags, offset, static_cast<uint32_t>(values.size()),
                       values.data());
}

// ---------------------------------------------------------------------------
// Device extensions
// ---------------------------------------------------------------------------

void handle_EnumerateDeviceExtensionProperties(Ctx& c, Server& server) {
    const uint64_t physdev_id = c.reader.handle();
    VkPhysicalDevice physdev = server.physical_device_from_id(physdev_id);
    if (!c.reader.ok() || physdev == VK_NULL_HANDLE) {
        c.writer.u32(static_cast<uint32_t>(Status::DecodeError));
        return;
    }

    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physdev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    if (count) vkEnumerateDeviceExtensionProperties(physdev, nullptr, &count, exts.data());

    // Only advertise what this project actually remotes. WSI is separate
    // work landing in wayland/; report VK_KHR_swapchain only if the real
    // device supports it, so that other agent's code has something truthful
    // to build on, and report nothing else.
    const VkExtensionProperties* swapchain = nullptr;
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, "VK_KHR_swapchain") == 0) {
            swapchain = &e;
            break;
        }
    }

    c.writer.u32(static_cast<uint32_t>(Status::Ok));
    c.writer.i32(VK_SUCCESS);
    c.writer.u32(swapchain ? 1 : 0);
    if (swapchain) remoting::write_ExtensionProperties(c.writer, *swapchain);
}

}  // namespace

void Server::serve(int fd) {
    fprintf(stderr, "server: client connected\n");
    remoting::ObjectTables tables;
    uint32_t oneway_errors = 0;
    g_current_error_counter = &oneway_errors;

    for (;;) {
        remoting::MessageHeader header{};
        std::vector<char> payload;
        if (!remoting::recv_message(fd, &header, &payload)) break;

        const remoting::Opcode opcode = static_cast<remoting::Opcode>(header.opcode);
        remoting::Reader reader(payload.data(), payload.size());
        remoting::Writer writer;
        Ctx c{fd, reader, writer, tables, oneway_errors};

        // Oneway opcodes never call reply(); the client already moved on
        // without waiting, and sending one back would desynchronise the next
        // round trip's recv().
        switch (opcode) {
            case remoting::Opcode::Handshake: {
                std::string client_digest;
                reader.string(&client_digest);
                const bool match = reader.ok() && client_digest == remoting::kCommandSetDigest;
                if (!match) {
                    fprintf(stderr,
                            "server: rejecting client, command set digest %s != %s\n",
                            client_digest.c_str(), remoting::kCommandSetDigest);
                }
                writer.u32(match ? static_cast<uint32_t>(remoting::Status::Ok)
                                 : static_cast<uint32_t>(remoting::Status::DecodeError));
                writer.string(remoting::kCommandSetDigest);
                writer.u32(static_cast<uint32_t>(m_physical_devices.size()));
                reply(fd, opcode, writer);
                if (!match) return;
                break;
            }

            case remoting::Opcode::vkEnumeratePhysicalDevices: {
                writer.u32(static_cast<uint32_t>(remoting::Status::Ok));
                writer.i32(VK_SUCCESS);
                writer.u32(static_cast<uint32_t>(m_physical_devices.size()));
                for (size_t i = 0; i < m_physical_devices.size(); ++i) {
                    writer.handle(static_cast<uint64_t>(i + 1));
                }
                reply(fd, opcode, writer);
                break;
            }

            case remoting::Opcode::vkGetPhysicalDeviceProperties: {
                const uint64_t id = reader.handle();
                VkPhysicalDevice device = physical_device_from_id(id);
                if (!reader.ok() || device == VK_NULL_HANDLE) {
                    reply_status(fd, opcode, remoting::Status::DecodeError);
                    break;
                }
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(device, &props);
                writer.u32(static_cast<uint32_t>(remoting::Status::Ok));
                // Sent as one flat struct (limits included) rather than field
                // by field: the client clamping apiVersion to 1.0 was masking
                // a real bug where limits stayed zeroed, which made every
                // later create call fail validation against a driver that
                // looked like it had no framebuffer, no viewports, nothing.
                remoting::write_PhysicalDeviceProperties(writer, props);
                reply(fd, opcode, writer);
                break;
            }

            case remoting::Opcode::vkGetPhysicalDeviceMemoryProperties: {
                const uint64_t id = reader.handle();
                VkPhysicalDevice device = physical_device_from_id(id);
                if (!reader.ok() || device == VK_NULL_HANDLE) {
                    reply_status(fd, opcode, remoting::Status::DecodeError);
                    break;
                }
                VkPhysicalDeviceMemoryProperties props{};
                vkGetPhysicalDeviceMemoryProperties(device, &props);
                writer.u32(static_cast<uint32_t>(remoting::Status::Ok));
                writer.u32(props.memoryTypeCount);
                for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
                    writer.u32(props.memoryTypes[i].propertyFlags);
                    writer.u32(props.memoryTypes[i].heapIndex);
                }
                writer.u32(props.memoryHeapCount);
                for (uint32_t i = 0; i < props.memoryHeapCount; ++i) {
                    writer.u64(props.memoryHeaps[i].size);
                    writer.u32(props.memoryHeaps[i].flags);
                }
                reply(fd, opcode, writer);
                break;
            }

            case remoting::Opcode::vkGetPhysicalDeviceQueueFamilyProperties: {
                const uint64_t id = reader.handle();
                VkPhysicalDevice device = physical_device_from_id(id);
                if (!reader.ok() || device == VK_NULL_HANDLE) {
                    reply_status(fd, opcode, remoting::Status::DecodeError);
                    break;
                }
                uint32_t count = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
                std::vector<VkQueueFamilyProperties> families(count);
                if (count) vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
                writer.u32(static_cast<uint32_t>(remoting::Status::Ok));
                writer.u32(count);
                for (uint32_t i = 0; i < count; ++i) {
                    writer.u32(families[i].queueFlags);
                    writer.u32(families[i].queueCount);
                    writer.u32(families[i].timestampValidBits);
                    writer.u32(families[i].minImageTransferGranularity.width);
                    writer.u32(families[i].minImageTransferGranularity.height);
                    writer.u32(families[i].minImageTransferGranularity.depth);
                }
                reply(fd, opcode, writer);
                break;
            }

            case remoting::Opcode::vkGetPhysicalDeviceFeatures: {
                const uint64_t id = reader.handle();
                VkPhysicalDevice device = physical_device_from_id(id);
                if (!reader.ok() || device == VK_NULL_HANDLE) {
                    reply_status(fd, opcode, remoting::Status::DecodeError);
                    break;
                }
                VkPhysicalDeviceFeatures features{};
                vkGetPhysicalDeviceFeatures(device, &features);
                writer.u32(static_cast<uint32_t>(remoting::Status::Ok));
                writer.bytes(&features, sizeof(features));
                reply(fd, opcode, writer);
                break;
            }

            case remoting::Opcode::vkGetPhysicalDeviceFormatProperties: {
                const uint64_t id = reader.handle();
                VkPhysicalDevice device = physical_device_from_id(id);
                const int32_t format = reader.i32();
                if (!reader.ok() || device == VK_NULL_HANDLE) {
                    reply_status(fd, opcode, remoting::Status::DecodeError);
                    break;
                }
                VkFormatProperties props{};
                vkGetPhysicalDeviceFormatProperties(device, static_cast<VkFormat>(format), &props);
                writer.u32(static_cast<uint32_t>(remoting::Status::Ok));
                writer.bytes(&props, sizeof(props));
                reply(fd, opcode, writer);
                break;
            }

            case remoting::Opcode::vkEnumerateDeviceExtensionProperties:
                handle_EnumerateDeviceExtensionProperties(c, *this);
                reply(fd, opcode, writer);
                break;

            case remoting::Opcode::vkCreateDevice:
                handle_CreateDevice(c, *this, opcode);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyDevice:
                handle_DestroyDevice(c);
                break;
            case remoting::Opcode::vkGetDeviceQueue:
                handle_GetDeviceQueue(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkQueueSubmit:
                handle_QueueSubmit(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkQueueWaitIdle:
                handle_QueueWaitIdle(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDeviceWaitIdle:
                handle_DeviceWaitIdle(c);
                reply(fd, opcode, writer);
                break;

            case remoting::Opcode::vkCreateFence:
                handle_CreateFence(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyFence:
                handle_DestroyFence(c);
                break;
            case remoting::Opcode::vkResetFences:
                handle_ResetFences(c);
                break;
            case remoting::Opcode::vkGetFenceStatus:
                handle_GetFenceStatus(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkWaitForFences:
                handle_WaitForFences(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkCreateSemaphore:
                handle_CreateSemaphore(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroySemaphore:
                handle_DestroySemaphore(c);
                break;

            case remoting::Opcode::vkAllocateMemory:
                handle_AllocateMemory(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkFreeMemory:
                handle_FreeMemory(c);
                break;
            case remoting::Opcode::FlushMappedMemory:
                handle_FlushMappedMemory(c);
                break;
            case remoting::Opcode::DownloadMappedMemory:
                handle_DownloadMappedMemory(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkBindBufferMemory:
                handle_BindBufferMemory(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkBindImageMemory:
                handle_BindImageMemory(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkGetBufferMemoryRequirements:
                handle_GetBufferMemoryRequirements(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkGetImageMemoryRequirements:
                handle_GetImageMemoryRequirements(c);
                reply(fd, opcode, writer);
                break;

            case remoting::Opcode::vkCreateBuffer:
                handle_CreateBuffer(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyBuffer:
                handle_DestroyBuffer(c);
                break;
            case remoting::Opcode::vkCreateImage:
                handle_CreateImage(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyImage:
                handle_DestroyImage(c);
                break;
            case remoting::Opcode::vkCreateImageView:
                handle_CreateImageView(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyImageView:
                handle_DestroyImageView(c);
                break;
            case remoting::Opcode::vkCreateSampler:
                handle_CreateSampler(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroySampler:
                handle_DestroySampler(c);
                break;
            case remoting::Opcode::vkGetImageSubresourceLayout:
                handle_GetImageSubresourceLayout(c);
                reply(fd, opcode, writer);
                break;

            case remoting::Opcode::vkCreateShaderModule:
                handle_CreateShaderModule(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyShaderModule:
                handle_DestroyShaderModule(c);
                break;
            case remoting::Opcode::vkCreatePipelineCache:
                handle_CreatePipelineCache(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyPipelineCache:
                handle_DestroyPipelineCache(c);
                break;
            case remoting::Opcode::vkCreatePipelineLayout:
                handle_CreatePipelineLayout(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyPipelineLayout:
                handle_DestroyPipelineLayout(c);
                break;
            case remoting::Opcode::vkCreateGraphicsPipelines:
                handle_CreateGraphicsPipelines(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyPipeline:
                handle_DestroyPipeline(c);
                break;
            case remoting::Opcode::vkCreateRenderPass:
                handle_CreateRenderPass(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyRenderPass:
                handle_DestroyRenderPass(c);
                break;
            case remoting::Opcode::vkCreateFramebuffer:
                handle_CreateFramebuffer(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyFramebuffer:
                handle_DestroyFramebuffer(c);
                break;

            case remoting::Opcode::vkCreateDescriptorSetLayout:
                handle_CreateDescriptorSetLayout(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyDescriptorSetLayout:
                handle_DestroyDescriptorSetLayout(c);
                break;
            case remoting::Opcode::vkCreateDescriptorPool:
                handle_CreateDescriptorPool(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyDescriptorPool:
                handle_DestroyDescriptorPool(c);
                break;
            case remoting::Opcode::vkAllocateDescriptorSets:
                handle_AllocateDescriptorSets(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkUpdateDescriptorSets:
                handle_UpdateDescriptorSets(c);
                break;

            case remoting::Opcode::vkCreateCommandPool:
                handle_CreateCommandPool(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkDestroyCommandPool:
                handle_DestroyCommandPool(c);
                break;
            case remoting::Opcode::vkAllocateCommandBuffers:
                handle_AllocateCommandBuffers(c);
                reply(fd, opcode, writer);
                break;
            case remoting::Opcode::vkFreeCommandBuffers:
                handle_FreeCommandBuffers(c);
                break;
            case remoting::Opcode::vkBeginCommandBuffer:
                handle_BeginCommandBuffer(c);
                break;
            case remoting::Opcode::vkEndCommandBuffer:
                handle_EndCommandBuffer(c);
                break;
            case remoting::Opcode::vkResetCommandBuffer:
                handle_ResetCommandBuffer(c);
                break;

            case remoting::Opcode::vkCmdBeginRenderPass:
                handle_CmdBeginRenderPass(c);
                break;
            case remoting::Opcode::vkCmdEndRenderPass:
                handle_CmdEndRenderPass(c);
                break;
            case remoting::Opcode::vkCmdBindPipeline:
                handle_CmdBindPipeline(c);
                break;
            case remoting::Opcode::vkCmdBindDescriptorSets:
                handle_CmdBindDescriptorSets(c);
                break;
            case remoting::Opcode::vkCmdBindVertexBuffers:
                handle_CmdBindVertexBuffers(c);
                break;
            case remoting::Opcode::vkCmdBindIndexBuffer:
                handle_CmdBindIndexBuffer(c);
                break;
            case remoting::Opcode::vkCmdSetViewport:
                handle_CmdSetViewport(c);
                break;
            case remoting::Opcode::vkCmdSetScissor:
                handle_CmdSetScissor(c);
                break;
            case remoting::Opcode::vkCmdDraw:
                handle_CmdDraw(c);
                break;
            case remoting::Opcode::vkCmdDrawIndexed:
                handle_CmdDrawIndexed(c);
                break;
            case remoting::Opcode::vkCmdPipelineBarrier:
                handle_CmdPipelineBarrier(c);
                break;
            case remoting::Opcode::vkCmdCopyBuffer:
                handle_CmdCopyBuffer(c);
                break;
            case remoting::Opcode::vkCmdCopyBufferToImage:
                handle_CmdCopyBufferToImage(c);
                break;
            case remoting::Opcode::vkCmdCopyImageToBuffer:
                handle_CmdCopyImageToBuffer(c);
                break;
            case remoting::Opcode::vkCmdClearColorImage:
                handle_CmdClearColorImage(c);
                break;
            case remoting::Opcode::vkCmdPushConstants:
                handle_CmdPushConstants(c);
                break;

            default: {
                fprintf(stderr, "server: unsupported command %s (%u)\n", remoting::opcode_name(opcode),
                        header.opcode);
                reply_status(fd, opcode, remoting::Status::UnsupportedCommand);
                break;
            }
        }
    }

    g_current_error_counter = nullptr;
    fprintf(stderr, "server: client disconnected\n");
}

namespace {

int listen_on(const std::string& address, uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "server: socket failed: %s\n", strerror(errno));
        return -1;
    }

    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        fprintf(stderr, "server: bad address '%s'\n", address.c_str());
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "server: bind failed: %s\n", strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) < 0) {
        fprintf(stderr, "server: listen failed: %s\n", strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    std::string address = "0.0.0.0";
    uint16_t port = 24680;
    bool validate = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--address" && i + 1 < argc) {
            address = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (arg == "--validate") {
            validate = true;
        } else if (arg == "--help") {
            printf("usage: %s [--address ADDR] [--port PORT] [--validate]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    Server server;
    if (!server.init_vulkan(validate)) return 1;

    const int listen_fd = listen_on(address, port);
    if (listen_fd < 0) return 1;

    fprintf(stderr, "server: listening on %s:%u (command set %s)\n", address.c_str(),
            static_cast<unsigned>(port), remoting::kCommandSetDigest);

    while (!g_stop.load()) {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        server.serve(fd);
        ::close(fd);
    }

    ::close(listen_fd);
    fprintf(stderr, "server: stopped\n");
    return 0;
}
