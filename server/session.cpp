#if defined(_WIN32)
// windows.h's default (non-lean) mode drags in the legacy winsock.h, which
// conflicts with wire.hpp's winsock2.h if windows.h is reached first in this
// translation unit; defining this - and requesting Vulkan's Win32 surface
// types - before session.hpp's own #include <vulkan/vulkan.h> keeps that
// from happening and makes VK_KHR_WIN32_SURFACE_EXTENSION_NAME available
// below (see client/wsi.cpp lines 20-31 for the same reasoning).
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "session.hpp"

#include <errno.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "own_window.hpp"
#if !defined(_WIN32)
#include "proxy_server.hpp"
#endif

namespace {

// Set to the current connection's oneway_errors counter for the duration of
// Server::serve(); the debug callback runs on this thread synchronously
// inside whatever Vulkan call triggered it. thread_local because serve() now
// runs concurrently for multiple clients (see the accept loop in main.cpp): a
// plain global here would let one client's errors get counted against
// another's, or a UAF once the first thread's oneway_errors goes out of scope.
thread_local uint32_t* g_current_error_counter = nullptr;

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

std::unordered_map<uint32_t, remoting::Handler>& handler_table() {
    static std::unordered_map<uint32_t, remoting::Handler> table;
    return table;
}

}  // namespace

std::atomic<bool> g_stop{false};

namespace remoting {

void register_handler(Opcode opcode, Handler fn) {
    handler_table()[static_cast<uint32_t>(opcode)] = fn;
}

Handler find_handler(Opcode opcode) {
    auto it = handler_table().find(static_cast<uint32_t>(opcode));
    return it == handler_table().end() ? nullptr : it->second;
}

}  // namespace remoting

bool Server::init_vulkan(bool validate, bool want_wayland) {
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

    // Both surface paths need a windowing-system surface extension now: the
    // WaylandProxy one (--wayland, Linux only) and the server-owned
    // OwnWindow one behind vkCreateWin32SurfaceKHR, which never touches
    // WaylandProxy and so cannot be tied to that flag. Enabling either
    // unconditionally would be wrong the other way round, though: a headless
    // server has no such extension, and asking for a missing one fails
    // vkCreateInstance outright, which would take the offscreen path down
    // with it. So ask only for what is there.
    {
        uint32_t ext_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> avail(ext_count);
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, avail.data());
        bool has_surface = false;
        bool has_platform_surface = false;
        for (const auto& e : avail) {
            if (strcmp(e.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0) has_surface = true;
#if defined(_WIN32)
            if (strcmp(e.extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME) == 0) {
                has_platform_surface = true;
            }
#else
            if (strcmp(e.extensionName, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) == 0) {
                has_platform_surface = true;
            }
#endif
        }
        if (has_surface && has_platform_surface) {
            extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(_WIN32)
            extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#else
            extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#endif
        } else {
#if defined(_WIN32)
            fprintf(stderr,
                    "server: VK_KHR_win32_surface is not available; presentation is disabled "
                    "and only the offscreen path will work\n");
#else
            fprintf(stderr,
                    "server: VK_KHR_wayland_surface is not available; presentation is disabled "
                    "and only the offscreen path will work\n");
#endif
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

Server::~Server() {
#if !defined(_WIN32)
    stop_wayland();
#endif
    if (m_messenger != VK_NULL_HANDLE) {
        auto destroy_fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_fn) destroy_fn(m_instance, m_messenger, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE) vkDestroyInstance(m_instance, nullptr);
}

#if !defined(_WIN32)
bool Server::start_wayland(uint16_t port) {
    m_wayland = std::make_unique<WaylandProxy>();
    if (!m_wayland->start(port)) {
        m_wayland.reset();
        return false;
    }
    m_wayland_thread = std::thread([this] {
        while (!g_stop.load()) {
            if (!m_wayland->poll()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    return true;
}

void Server::stop_wayland() {
    if (m_wayland_thread.joinable()) {
        g_stop.store(true);
        m_wayland_thread.join();
    }
}
#endif

OwnWindow* Server::create_own_window() {
    auto window = std::make_unique<OwnWindow>();
    if (!window->create()) return nullptr;
    std::lock_guard<std::mutex> lock(m_own_windows_mutex);
    m_own_windows.push_back(std::move(window));
    return m_own_windows.back().get();
}

void Server::associate_surface(VkSurfaceKHR surface, OwnWindow* window) {
    if (surface == VK_NULL_HANDLE || window == nullptr) return;
    std::lock_guard<std::mutex> lock(m_own_windows_mutex);
    m_surface_windows[surface] = window;
}

void Server::discard_own_window(OwnWindow* window) {
    if (window == nullptr) return;
    std::lock_guard<std::mutex> lock(m_own_windows_mutex);
    auto it = std::find_if(m_own_windows.begin(), m_own_windows.end(),
                           [window](const auto& candidate) { return candidate.get() == window; });
    if (it != m_own_windows.end()) m_own_windows.erase(it);
}

void Server::release_surface_window(VkSurfaceKHR surface) {
    if (surface == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(m_own_windows_mutex);
    auto mapped = m_surface_windows.find(surface);
    if (mapped == m_surface_windows.end()) return;
    OwnWindow* window = mapped->second;
    m_surface_windows.erase(mapped);
    auto owned = std::find_if(m_own_windows.begin(), m_own_windows.end(),
                              [window](const auto& candidate) { return candidate.get() == window; });
    if (owned != m_own_windows.end()) m_own_windows.erase(owned);
}

OwnWindow* Server::window_for_surface(VkSurfaceKHR surface) {
    if (surface == VK_NULL_HANDLE) return nullptr;
    std::lock_guard<std::mutex> lock(m_own_windows_mutex);
    auto it = m_surface_windows.find(surface);
    return it == m_surface_windows.end() ? nullptr : it->second;
}

bool Server::poll_window_closed(VkSurfaceKHR surface) {
    std::lock_guard<std::mutex> lock(m_own_windows_mutex);
    auto it = m_surface_windows.find(surface);
    if (it == m_surface_windows.end()) return false;
    it->second->pump();
    return it->second->closed();
}

bool Server::poll_window_resized(VkSurfaceKHR surface) {
    std::lock_guard<std::mutex> lock(m_own_windows_mutex);
    auto it = m_surface_windows.find(surface);
    if (it == m_surface_windows.end()) return false;
    it->second->pump();
    return it->second->take_resized();
}

std::vector<OwnWindow::InputEvent> Server::poll_window_events(VkSurfaceKHR surface,
                                                               uint32_t max_events,
                                                               bool* overflowed) {
    std::lock_guard<std::mutex> lock(m_own_windows_mutex);
    auto it = m_surface_windows.find(surface);
    if (it == m_surface_windows.end()) return {};
    return it->second->take_input_events(max_events, overflowed);
}

VkPhysicalDevice Server::physical_device_from_id(uint64_t id) const {
    if (id == 0 || id > m_physical_devices.size()) return VK_NULL_HANDLE;
    return m_physical_devices[id - 1];
}

namespace {

// Destroy what one connection created, in an order Vulkan allows: a
// swapchain must go before the surface it presents to (the spec says so
// outright, VUID-vkDestroySurfaceKHR-surface-01266), and the device must be
// idle before either, or the objects are still in use by work in flight.
//
// Before this, a disconnect destroyed nothing at all: the tables simply went
// out of scope, and their handles with them. Every swapchain, surface and
// wl_buffer of every connection leaked for the server's whole life, which is
// what kept a compositor window alive after the client that owned it was
// gone, and what produced 'queue destroyed while proxies still attached'
// with fourteen objects still on it.
void destroy_session_objects(remoting::ObjectTables& tables, Server& server) {
    const VkInstance instance = server.instance();
    for (VkSwapchainKHR swapchain : tables.swapchains.all()) {
        if (swapchain == VK_NULL_HANDLE) continue;
        const auto it = tables.swapchain_devices.find(swapchain);
        if (it == tables.swapchain_devices.end() || it->second == VK_NULL_HANDLE) continue;
        vkDeviceWaitIdle(it->second);
        vkDestroySwapchainKHR(it->second, swapchain, nullptr);
    }
    tables.swapchain_devices.clear();
    tables.swapchain_surfaces.clear();

    for (VkSurfaceKHR surface : tables.surfaces.all()) {
        if (surface == VK_NULL_HANDLE) continue;
        vkDestroySurfaceKHR(instance, surface, nullptr);
        server.release_surface_window(surface);
    }
}

// A stalled server is indistinguishable from an idle one from the outside:
// both are a process that is alive and reading nothing. That ambiguity cost a
// whole Windows round trip, where all the evidence available (a frozen 5920
// byte receive queue, no read syscalls on any of 58 threads) established only
// that the server had stopped servicing the connection, and nothing at all
// about where it had stopped.
//
// So the server says so itself. While a handler runs, its opcode and start
// time are published here; a watchdog names any handler that has not returned
// within the deadline. If it is blocked, the log names the exact command, and
// a second line every deadline afterwards distinguishes "slow" from "never
// coming back". If instead the server is genuinely idle, nothing is printed,
// which is the other half of the answer.
class StallWatchdog {
public:
    explicit StallWatchdog(double deadline_seconds)
        : deadline_(deadline_seconds), thread_([this] { run(); }) {}

    ~StallWatchdog() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    void enter(remoting::Opcode opcode) {
        start_ = Clock::now();
        opcode_.store(static_cast<uint32_t>(opcode), std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
    }

    void leave() { running_.store(false, std::memory_order_release); }

private:
    using Clock = std::chrono::steady_clock;

    void run() {
        // Sampling, not instrumenting the fast path: a handler is only ever
        // looked at from here, so an ordinary sub-millisecond command costs
        // nothing and is never seen. The poll follows the deadline so that a
        // short deadline is actually observable - with a fixed interval, any
        // deadline below it could never be caught in the act.
        // Parentheses avoid expansion of Windows' legacy min macro.
        const double interval = (std::min)(0.5, deadline_ / 2.0);
        std::unique_lock<std::mutex> lock(mutex_);
        unsigned reported = 0;
        while (!stop_) {
            cv_.wait_for(lock, std::chrono::duration<double>(interval),
                         [this] { return stop_; });
            if (stop_) break;
            if (!running_.load(std::memory_order_acquire)) {
                reported = 0;
                continue;
            }
            const double elapsed =
                std::chrono::duration<double>(Clock::now() - start_).count();
            const unsigned overdue = static_cast<unsigned>(elapsed / deadline_);
            if (overdue > reported) {
                reported = overdue;
                const auto opcode =
                    static_cast<remoting::Opcode>(opcode_.load(std::memory_order_relaxed));
                fprintf(stderr,
                        "server: STALL: %s has not returned after %.2fs; the server is not "
                        "reading the connection while this runs\n",
                        remoting::opcode_name(opcode), elapsed);
                fflush(stderr);
            }
        }
    }

    const double deadline_;
    std::atomic<uint32_t> opcode_{0};
    std::atomic<bool> running_{false};
    Clock::time_point start_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

double stall_deadline_seconds() {
    if (const char* env = getenv("VK_REMOTING_STALL_SECONDS")) {
        const double value = atof(env);
        if (value > 0.0) return value;
    }
    return 5.0;
}

}  // namespace

void Server::serve(remoting::socket_t fd) {
    fprintf(stderr, "server: client connected\n");
    remoting::ObjectTables tables;
    uint32_t oneway_errors = 0;
    g_current_error_counter = &oneway_errors;
    StallWatchdog watchdog(stall_deadline_seconds());

    for (;;) {
        remoting::MessageHeader header{};
        std::vector<char> payload;
        if (!remoting::recv_message(fd, &header, &payload)) break;

        const remoting::Opcode opcode = static_cast<remoting::Opcode>(header.opcode);
        remoting::Reader reader(payload.data(), payload.size());
        remoting::Writer writer;
        remoting::Session session{fd, *this, reader, writer, tables, oneway_errors, opcode};

        remoting::Handler handler = remoting::find_handler(opcode);
        if (handler == nullptr) {
            fprintf(stderr, "server: unsupported command %s (%u)\n", remoting::opcode_name(opcode),
                    header.opcode);
            session.reply_status(remoting::Status::UnsupportedCommand);
            continue;
        }

        watchdog.enter(opcode);
        handler(session);
        watchdog.leave();
        // Only the Handshake handler ever sets this, on a command-set digest
        // mismatch; the original code returned from Server::serve() right
        // there, skipping the "client disconnected" log and the error-counter
        // cleanup below, so this preserves that exactly.
        if (session.close_connection) {
            destroy_session_objects(tables, *this);
            return;
        }
    }

    destroy_session_objects(tables, *this);
    g_current_error_counter = nullptr;
    fprintf(stderr, "server: client disconnected\n");
}
