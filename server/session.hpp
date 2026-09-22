#pragma once

#define VK_USE_PLATFORM_WAYLAND_KHR

// Per-connection session state and the opcode -> handler dispatch table.
//
// One Session is constructed per received message inside Server::serve() (see
// session.cpp) and threaded by reference into whichever handlers_*.cpp
// function is registered for that message's opcode. It bundles exactly what a
// handler needs: the socket to reply on, a reference to the Server (for
// physical-device/Wayland/instance access), this message's Reader/Writer, the
// connection's ObjectTables, and its oneway-error counter (see server.cpp's
// original header comment on thread_local error counting, now on
// g_current_error_counter in session.cpp).
//
// Handlers register themselves at static-init time via REGISTER_HANDLER,
// which is why adding a new command only ever means adding a new
// handlers_*.cpp function plus one REGISTER_HANDLER line in the same file -
// never touching this header, session.cpp, or another handlers_*.cpp. Every
// handlers_*.cpp source file is listed directly on the vulkan_remoting_server
// executable target in CMakeLists.txt (not folded into a separate static
// library), so the linker cannot drop its translation unit - and therefore
// cannot drop its REGISTER_HANDLER side effect - the way it could if these
// objects were pulled from an archive nothing else referenced.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>

#include "objects.hpp"
#include "own_window.hpp"
#include "remoting_commands.inl"
#include "wire.hpp"

#if !defined(_WIN32)
class WaylandProxy;
#endif

// Set by SIGINT/SIGTERM (see main.cpp's on_signal); checked by main.cpp's
// accept loop and by Server::start_wayland's pump thread so both stop
// promptly on shutdown.
extern std::atomic<bool> g_stop;

// Everything the server knows once Vulkan is up: the instance, its physical
// devices, and (optionally) the embedded Wayland proxy. One process-wide
// instance, shared by every connection's Session - see objects.hpp for the
// per-connection state instead.
class Server {
   public:
    bool init_vulkan(bool validate, bool want_wayland);
    ~Server();

#if !defined(_WIN32)
    // Starts the embedded Wayland proxy and its pump thread. See the
    // top-of-file comment in server.cpp's original form (now in
    // handlers_wsi.cpp) for why this runs on its own thread rather than
    // being folded into serve()'s blocking recv loop. Linux-only: a Windows
    // server has no compositor connection of its own to proxy (see
    // CMakeLists.txt's `if(NOT WIN32)` block around the wayland/ pieces),
    // so main.cpp never lets --wayland reach here on that platform.
    bool start_wayland(uint16_t port);
    void stop_wayland();

    WaylandProxy* wayland() const { return m_wayland.get(); }
#endif
    VkInstance instance() const { return m_instance; }
    size_t physical_device_count() const { return m_physical_devices.size(); }

    // Creates a new server-owned Wayland window (see server/own_window.hpp)
    // for handle_CreateWin32SurfaceKHR to present through. One per surface
    // request rather than a single shared window, kept alive for the life of
    // the Server because the VkSurfaceKHR it backs must outlive the call
    // that created it. Returns nullptr if there is no compositor to talk
    // to - this must work whether or not the server was started with
    // --wayland, so it never touches m_wayland.
    OwnWindow* create_own_window();

    // Handles cross the wire as indices, never as pointers. A VkPhysicalDevice
    // is a host pointer; sending its bits would be meaningless remotely and
    // would leak an address, so the client only ever sees 1-based indices.
    VkPhysicalDevice physical_device_from_id(uint64_t id) const;

    // Runs on its own thread per connection (see main.cpp's accept loop);
    // owns that connection's ObjectTables and dispatches every message on it
    // through the registration table below until the peer disconnects.
    void serve(remoting::socket_t fd);

   private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> m_physical_devices;
#if !defined(_WIN32)
    std::unique_ptr<WaylandProxy> m_wayland;
    std::thread m_wayland_thread;
#endif

    // Guards m_own_windows: handle_CreateWin32SurfaceKHR runs on whichever
    // thread is serving that connection (see main.cpp's accept loop), and
    // two connections can request one concurrently.
    std::mutex m_own_windows_mutex;
    std::vector<std::unique_ptr<OwnWindow>> m_own_windows;
};

namespace remoting {

// Everything one handler call needs. Reused across the handlers_*.cpp files
// exactly as server.cpp's original Ctx struct was reused across its case
// handler functions.
struct Session {
    socket_t fd;
    Server& server;
    Reader& reader;
    Writer& writer;
    ObjectTables& tables;
    uint32_t& oneway_errors;
    Opcode opcode;
    // Set by the Handshake handler on a digest mismatch: the original code
    // returned from Server::serve() immediately in that case, skipping the
    // "client disconnected" log and the error-counter cleanup at the bottom
    // of the loop, so serve() checks this and does the same.
    bool close_connection = false;

    void reply() { send_message(fd, static_cast<uint32_t>(opcode), writer.data()); }
    void reply_status(Status status) {
        writer.u32(static_cast<uint32_t>(status));
        reply();
    }
};

inline void mark_oneway_error(Session& s) { ++s.oneway_errors; }

// True when a peer-supplied element count could actually be backed by the
// bytes still unread. Handlers must consult this *before* sizing a container
// from the count: the per-element loops below check reader.ok(), but that
// check runs after the allocation has already been attempted, so it cannot
// stop a count of 0xFFFFFFFF from throwing length_error or getting the
// process OOM-killed. min_wire_bytes is the smallest number of bytes one
// element can possibly consume on the wire, so this stays conservative - it
// rejects only counts that are impossible, never merely large ones.
inline bool count_fits(const Reader& reader, uint32_t count, size_t min_wire_bytes) {
    return count <= reader.remaining() / (min_wire_bytes == 0 ? 1 : min_wire_bytes);
}

using Handler = void (*)(Session&);

// Inserts into a function-local static map, so registration never depends on
// static-initialisation order across translation units - only on each
// handlers_*.cpp's own REGISTER_HANDLER running at some point before the
// first dispatch, which static initialisation of file-scope objects
// guarantees.
void register_handler(Opcode opcode, Handler fn);
Handler find_handler(Opcode opcode);

struct HandlerRegistrar {
    HandlerRegistrar(Opcode opcode, Handler fn) { register_handler(opcode, fn); }
};

}  // namespace remoting

#define REGISTER_HANDLER(OPCODE, fn)                                                \
    static const remoting::HandlerRegistrar registrar_##fn(remoting::Opcode::OPCODE, fn)
