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
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>

#include "objects.hpp"
#include "remoting_commands.inl"
#include "wire.hpp"

class WaylandProxy;

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

    // Starts the embedded Wayland proxy and its pump thread. See the
    // top-of-file comment in server.cpp's original form (now in
    // handlers_wsi.cpp) for why this runs on its own thread rather than
    // being folded into serve()'s blocking recv loop.
    bool start_wayland(uint16_t port);
    void stop_wayland();

    WaylandProxy* wayland() const { return m_wayland.get(); }
    VkInstance instance() const { return m_instance; }
    size_t physical_device_count() const { return m_physical_devices.size(); }

    // Handles cross the wire as indices, never as pointers. A VkPhysicalDevice
    // is a host pointer; sending its bits would be meaningless remotely and
    // would leak an address, so the client only ever sees 1-based indices.
    VkPhysicalDevice physical_device_from_id(uint64_t id) const;

    // Runs on its own thread per connection (see main.cpp's accept loop);
    // owns that connection's ObjectTables and dispatches every message on it
    // through the registration table below until the peer disconnects.
    void serve(int fd);

   private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> m_physical_devices;
    std::unique_ptr<WaylandProxy> m_wayland;
    std::thread m_wayland_thread;
};

namespace remoting {

// Everything one handler call needs. Reused across the handlers_*.cpp files
// exactly as server.cpp's original Ctx struct was reused across its case
// handler functions.
struct Session {
    int fd;
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
