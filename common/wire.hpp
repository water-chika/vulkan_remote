#pragma once

// Framing for Vulkan-over-TCP.
//
// Every message is a fixed header followed by a payload. Vulkan calls are
// request/response because most of them return something the caller uses
// immediately, so the client blocks on a reply. That is precisely why this
// cannot be fast over a network: a frame's worth of calls pays the round trip
// each time. Keeping the framing this plain makes that cost visible instead of
// hiding it behind batching that would only help the calls nobody waits on.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
// windows.h's default (non-lean) mode drags in the legacy winsock.h, which
// conflicts with winsock2.h if both end up in the same translation unit; the
// .cpp files that reach this header on Windows define WIN32_LEAN_AND_MEAN
// before their very first include (see wire.cpp/icd.cpp/wsi.cpp/
// remote_objects.cpp) so that whichever of windows.h/winsock2.h the compiler
// sees first, the other does not conflict with it.
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace remoting {

#if defined(_WIN32)
// SOCKET is an unsigned, pointer-sized handle on Windows (64 bits on x64),
// not a small file descriptor; storing it in `int` as the POSIX side does
// would silently truncate it, so every socket value lives in this type
// instead from here on.
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

enum class Status : uint32_t {
    Ok = 0,
    UnsupportedCommand = 1,
    DecodeError = 2,
};

struct MessageHeader {
    uint32_t opcode;
    uint32_t payload_size;
};

// Serialises values little-endian. Vulkan handles are written as uint64 so the
// wire format does not change between a 32-bit and 64-bit client.
class Writer {
   public:
    void u32(uint32_t v) { raw(&v, sizeof(v)); }
    void u64(uint64_t v) { raw(&v, sizeof(v)); }
    void i32(int32_t v) { raw(&v, sizeof(v)); }
    void f32(float v) { raw(&v, sizeof(v)); }

    void handle(uint64_t v) { u64(v); }

    void bytes(const void* data, size_t size) {
        u32(static_cast<uint32_t>(size));
        if (size) raw(data, size);
    }

    void string(const char* s) {
        const size_t len = s ? std::strlen(s) : 0;
        bytes(s, len);
    }

    const std::vector<char>& data() const { return m_data; }
    void clear() { m_data.clear(); }

   private:
    void raw(const void* data, size_t size) {
        const char* p = static_cast<const char*>(data);
        m_data.insert(m_data.end(), p, p + size);
    }

    std::vector<char> m_data;
};

// Bounds-checked so a malformed or truncated message reports an error rather
// than reading past the buffer. A remote peer is untrusted input.
class Reader {
   public:
    Reader(const char* data, size_t size) : m_data(data), m_size(size), m_pos(0) {}

    bool ok() const { return !m_failed; }

    // Bytes not yet consumed. A handler about to size a container from a
    // peer-supplied count needs this: no count is honest if the elements it
    // promises cannot fit in what is left of the message. Checking costs
    // nothing and must happen *before* the allocation, because a vector sized
    // from 0xFFFFFFFF throws or gets the process OOM-killed long before any
    // per-element bounds check in the read loop can run.
    size_t remaining() const { return m_failed ? 0 : m_size - m_pos; }

    uint32_t u32() { return read<uint32_t>(); }
    uint64_t u64() { return read<uint64_t>(); }
    int32_t i32() { return read<int32_t>(); }
    float f32() { return read<float>(); }

    uint64_t handle() { return u64(); }

    bool bytes(std::vector<char>* out) {
        const uint32_t size = u32();
        if (m_failed || m_pos + size > m_size) {
            m_failed = true;
            return false;
        }
        out->assign(m_data + m_pos, m_data + m_pos + size);
        m_pos += size;
        return true;
    }

    bool string(std::string* out) {
        std::vector<char> buf;
        if (!bytes(&buf)) return false;
        out->assign(buf.begin(), buf.end());
        return true;
    }

   private:
    template <typename T>
    T read() {
        T v{};
        if (m_failed || m_pos + sizeof(T) > m_size) {
            m_failed = true;
            return v;
        }
        std::memcpy(&v, m_data + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return v;
    }

    const char* m_data;
    size_t m_size;
    size_t m_pos;
    bool m_failed = false;
};

// Blocking whole-message send and receive. Return false on any short read or
// peer disconnect; callers treat that as a dead connection.
bool send_message(socket_t fd, uint32_t opcode, const std::vector<char>& payload);
bool recv_message(socket_t fd, MessageHeader* header, std::vector<char>* payload);

// Connects to host:port with TCP_NODELAY set, returning kInvalidSocket on
// failure.
socket_t connect_to(const std::string& host, uint16_t port);

// ::close on POSIX, ::closesocket on Windows (they are not interchangeable:
// Windows keeps ::close for CRT file descriptors, not SOCKETs).
void close_socket(socket_t fd);

// Runs WSAStartup exactly once per process (a no-op on POSIX). Winsock
// requires this before any socket call at all, including the very first
// ::socket()/::bind()/::listen() a listening server makes, not just the
// ::connect() path connect_to() below already covers - so any translation
// unit that opens a socket directly (see server/main.cpp's listen_on) must
// call this first instead of assuming connect_to() already ran on this
// process.
void ensure_sockets_initialised();

}  // namespace remoting
