#pragma once

// Wayland wire format: parsing and re-serialising messages using tables
// generated from the protocol XML (see generate_protocol.py). Both proxy
// halves need this because neither one gets to hand-write a listener per
// interface — the whole point is a generic pump that works for whatever
// interface the app happens to use.
//
// Object ids are per-connection, not per-interface, so decoding a message
// requires knowing which interface its object_id currently names. That is
// tracked in ObjectTable as messages flow past, seeded with id 1 = wl_display
// the same way every Wayland connection is.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wire {

enum class ArgType : uint8_t { Int, Uint, Fixed, String, Object, NewId, Array, Fd };

// interface is the statically-known target for Object/NewId args, or nullptr
// when the XML does not say (wl_registry.bind's new_id, or an untyped
// object arg — none exist in the protocols we generate from, but the parser
// must not assume that stays true forever).
struct ArgSpec {
    ArgType type;
    const char* interface;
};

struct MessageSpec {
    const char* name;
    uint16_t opcode;
    uint16_t arg_count;
    const ArgSpec* args;
    bool is_destructor;
};

struct InterfaceSpec {
    const char* name;
    uint32_t version;
    uint16_t request_count;
    const MessageSpec* requests;
    uint16_t event_count;
    const MessageSpec* events;
};

const InterfaceSpec* find_interface(const std::string& name);
const MessageSpec* find_message(const InterfaceSpec* iface, bool is_event, uint16_t opcode);

// One decoded argument. Which fields are meaningful depends on `type`:
//   Int/Fixed   -> i32 (Fixed is the raw 24.8 value, unconverted)
//   Uint/Object -> u32 (Object is the referenced object id)
//   String      -> str
//   Array       -> array
//   NewId       -> u32 is the new object id; `str`/new_id_version give the
//                  target interface/version, filled in either from the
//                  static ArgSpec or, for wl_registry.bind, decoded off the
//                  wire (string name, uint version) that precedes the id.
//   Fd          -> fd, filled in by the caller after decoding since fds never
//                  travel in the byte stream itself.
struct DecodedArg {
    ArgType type;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    std::string str;
    uint32_t new_id_version = 0;
    std::vector<uint8_t> array;
    int fd = -1;
};

struct DecodedMessage {
    uint32_t object_id = 0;
    uint16_t opcode = 0;
    uint16_t size = 0;
    std::vector<DecodedArg> args;
};

// Number of Fd-typed arguments a message declares. The caller needs this
// before it can know how many fds to pull off SCM_RIGHTS or off the wire
// framing for a given message.
size_t count_fd_args(const MessageSpec* msg);

// Decodes exactly one message body (the bytes after the 8-byte header) using
// `msg` as the shape. Returns false on any malformed input — short buffer,
// string/array length that would overrun, garbage new_id interface name —
// rather than guessing, because the peer is untrusted.
bool decode_message(const MessageSpec* msg, const uint8_t* payload, size_t payload_len, DecodedMessage* out);

// Re-serialises `msg_args` (object_id/opcode plus the decoded arguments) into
// a full wire message, header included. Fd-typed args contribute no bytes.
bool encode_message(uint32_t object_id, uint16_t opcode, const std::vector<DecodedArg>& args,
                     std::vector<uint8_t>* out);

struct ObjectInfo {
    std::string interface;
    uint32_t version = 0;
};

class ObjectTable {
   public:
    ObjectTable() { table_[1] = {"wl_display", 1}; }

    const ObjectInfo* find(uint32_t id) const {
        auto it = table_.find(id);
        return it == table_.end() ? nullptr : &it->second;
    }
    void insert(uint32_t id, std::string interface, uint32_t version) {
        table_[id] = {std::move(interface), version};
    }
    void erase(uint32_t id) { table_.erase(id); }

   private:
    std::unordered_map<uint32_t, ObjectInfo> table_;
};

// Applies the effect a decoded message has on object lifetime: any NewId arg
// creates an entry, and wl_display.delete_id (the one event every Wayland
// connection gets for free) removes one. Called for both directions since
// new_id-carrying messages can be requests (client creates) or events
// (server creates, e.g. wl_data_device.data_offer).
void track_object_lifetime(ObjectTable* table, const std::string& interface, bool is_event,
                            const MessageSpec* msg, const DecodedMessage& decoded);

// One record on the TCP link between proxy_client and proxy_server: the raw
// Wayland wire message (header+payload, fds excluded, byte-identical to what
// travelled on the local unix socket) plus, in argument order, the byte
// content of each Fd-typed argument. Fds cannot cross TCP, so their contents
// are shipped instead and turned back into an fd (memfd, or a pipe written
// once) on the far end.
struct LinkFrame {
    std::vector<uint8_t> wire_bytes;
    std::vector<std::vector<uint8_t>> fd_blobs;
};

void write_link_frame(const LinkFrame& frame, std::vector<uint8_t>* out);

enum class FrameResult { NeedMoreData, Ok, Malformed };

// Tries to remove exactly one frame from the front of `buffer` (which holds
// whatever has arrived on the socket so far, possibly a partial frame).
// NeedMoreData leaves `buffer` untouched; Ok erases the consumed prefix and
// fills `out`; Malformed means a size field was absurd, which the untrusted
// peer must never be trusted on — the caller should drop the connection.
FrameResult take_link_frame(std::vector<uint8_t>* buffer, LinkFrame* out);

// Pulls one raw Wayland wire message (header+payload, exactly as it appears
// on a real unix socket) off the front of a streaming buffer. Used on the
// app-facing socket, which has no framing of our own to lean on.
FrameResult take_wire_message(std::vector<uint8_t>* buffer, std::vector<uint8_t>* whole_message);

}  // namespace wire
