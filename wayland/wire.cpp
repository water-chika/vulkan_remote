#include "wire.hpp"

#include <cstring>

#include "protocol_tables.inl"

namespace wire {

const InterfaceSpec* find_interface(const std::string& name) {
    for (size_t i = 0; i < kInterfaceTableCount; ++i) {
        if (name == kInterfaceTable[i].name) return &kInterfaceTable[i];
    }
    return nullptr;
}

const MessageSpec* find_message(const InterfaceSpec* iface, bool is_event, uint16_t opcode) {
    if (!iface) return nullptr;
    const MessageSpec* msgs = is_event ? iface->events : iface->requests;
    const uint16_t count = is_event ? iface->event_count : iface->request_count;
    if (opcode >= count) return nullptr;
    return &msgs[opcode];
}

size_t count_fd_args(const MessageSpec* msg) {
    size_t n = 0;
    for (uint16_t i = 0; i < msg->arg_count; ++i) {
        if (msg->args[i].type == ArgType::Fd) ++n;
    }
    return n;
}

namespace {

// Wire integers are host-endian on both peers in practice (Wayland itself
// only ever runs on a local unix socket and assumes native byte order); the
// TCP hop in between is between two x86_64 Linux boxes, so no byte-swapping
// is done here, matching common/wire.hpp's own choice.
bool read_u32(const uint8_t* data, size_t len, size_t* pos, uint32_t* out) {
    if (*pos + 4 > len) return false;
    std::memcpy(out, data + *pos, 4);
    *pos += 4;
    return true;
}

// Strings/arrays are length-prefixed and padded to a 4-byte boundary; a
// malicious or truncated length must never turn into an out-of-bounds read.
bool read_blob(const uint8_t* data, size_t len, size_t* pos, std::vector<uint8_t>* out) {
    uint32_t blob_len = 0;
    if (!read_u32(data, len, pos, &blob_len)) return false;
    if (blob_len > len || *pos + blob_len > len) return false;
    out->assign(data + *pos, data + *pos + blob_len);
    const size_t padded = (blob_len + 3) & ~size_t(3);
    if (*pos + padded > len) return false;
    *pos += padded;
    return true;
}

}  // namespace

bool decode_message(const MessageSpec* msg, const uint8_t* payload, size_t payload_len, DecodedMessage* out) {
    out->args.clear();
    out->args.reserve(msg->arg_count);
    size_t pos = 0;

    for (uint16_t i = 0; i < msg->arg_count; ++i) {
        const ArgSpec& spec = msg->args[i];
        DecodedArg arg;
        arg.type = spec.type;

        switch (spec.type) {
            case ArgType::Int:
            case ArgType::Fixed: {
                uint32_t raw = 0;
                if (!read_u32(payload, payload_len, &pos, &raw)) return false;
                arg.i32 = static_cast<int32_t>(raw);
                break;
            }
            case ArgType::Uint:
            case ArgType::Object: {
                if (!read_u32(payload, payload_len, &pos, &arg.u32)) return false;
                break;
            }
            case ArgType::String: {
                std::vector<uint8_t> blob;
                if (!read_blob(payload, payload_len, &pos, &blob)) return false;
                // blob includes the trailing NUL; drop it for a clean std::string.
                if (blob.empty() || blob.back() != 0) return false;
                arg.str.assign(blob.begin(), blob.end() - 1);
                break;
            }
            case ArgType::Array: {
                if (!read_blob(payload, payload_len, &pos, &arg.array)) return false;
                break;
            }
            case ArgType::NewId: {
                if (spec.interface == nullptr) {
                    // wl_registry.bind: interface name, version, then the id,
                    // collapsed here into the single declared argument.
                    std::vector<uint8_t> blob;
                    if (!read_blob(payload, payload_len, &pos, &blob)) return false;
                    if (blob.empty() || blob.back() != 0) return false;
                    arg.str.assign(blob.begin(), blob.end() - 1);
                    if (!read_u32(payload, payload_len, &pos, &arg.new_id_version)) return false;
                } else {
                    arg.str = spec.interface;
                }
                if (!read_u32(payload, payload_len, &pos, &arg.u32)) return false;
                break;
            }
            case ArgType::Fd: {
                // Consumes no wire bytes; the caller fills arg.fd separately.
                break;
            }
        }
        out->args.push_back(std::move(arg));
    }

    // Trailing garbage after all declared args is also a malformed message —
    // it would mean our understanding of the shape disagrees with the sender's.
    return pos == payload_len;
}

bool encode_message(uint32_t object_id, uint16_t opcode, const std::vector<DecodedArg>& args,
                     std::vector<uint8_t>* out) {
    std::vector<uint8_t> body;
    auto put_u32 = [&](uint32_t v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        body.insert(body.end(), p, p + 4);
    };
    auto put_blob = [&](const uint8_t* data, size_t len) {
        put_u32(static_cast<uint32_t>(len));
        body.insert(body.end(), data, data + len);
        const size_t pad = ((len + 3) & ~size_t(3)) - len;
        for (size_t i = 0; i < pad; ++i) body.push_back(0);
    };

    for (const DecodedArg& arg : args) {
        switch (arg.type) {
            case ArgType::Int:
            case ArgType::Fixed:
                put_u32(static_cast<uint32_t>(arg.i32));
                break;
            case ArgType::Uint:
            case ArgType::Object:
                put_u32(arg.u32);
                break;
            case ArgType::String: {
                std::vector<uint8_t> nul_terminated(arg.str.begin(), arg.str.end());
                nul_terminated.push_back(0);
                put_blob(nul_terminated.data(), nul_terminated.size());
                break;
            }
            case ArgType::Array:
                put_blob(arg.array.data(), arg.array.size());
                break;
            case ArgType::NewId:
                if (!arg.str.empty() && arg.new_id_version != 0) {
                    // Only wl_registry.bind ever needs the inline
                    // interface/version pair; statically-typed new_id args
                    // never set new_id_version.
                    std::vector<uint8_t> nul_terminated(arg.str.begin(), arg.str.end());
                    nul_terminated.push_back(0);
                    put_blob(nul_terminated.data(), nul_terminated.size());
                    put_u32(arg.new_id_version);
                }
                put_u32(arg.u32);
                break;
            case ArgType::Fd:
                break;  // out of band
        }
    }

    if (body.size() > 0xffffu) return false;  // size field is 16 bits
    const uint32_t header2 = (static_cast<uint32_t>(body.size() + 8) << 16) | opcode;

    out->clear();
    out->reserve(body.size() + 8);
    auto append_u32 = [&](uint32_t v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        out->insert(out->end(), p, p + 4);
    };
    append_u32(object_id);
    append_u32(header2);
    out->insert(out->end(), body.begin(), body.end());
    return true;
}

void track_object_lifetime(ObjectTable* table, const std::string& interface, bool is_event,
                            const MessageSpec* msg, const DecodedMessage& decoded) {
    if (is_event && interface == "wl_display" && std::string(msg->name) == "delete_id" &&
        !decoded.args.empty()) {
        table->erase(decoded.args[0].u32);
        return;
    }
    for (const DecodedArg& arg : decoded.args) {
        if (arg.type != ArgType::NewId) continue;
        const uint32_t version = arg.new_id_version != 0 ? arg.new_id_version : 1;
        table->insert(arg.u32, arg.str, version);
    }
}

namespace {
// A cap generous enough for a keymap (typically tens of KB) or a handful of
// queued events, but small enough that a hostile peer cannot force an
// unbounded allocation from a single length field.
constexpr uint32_t kMaxFrameBytes = 16u * 1024 * 1024;

void put_u32(std::vector<uint8_t>* out, uint32_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out->insert(out->end(), p, p + 4);
}

// Returns false if fewer than `count` bytes remain from `pos`.
bool get_u32(const uint8_t* data, size_t size, size_t* pos, uint32_t* out) {
    if (*pos + 4 > size) return false;
    std::memcpy(out, data + *pos, 4);
    *pos += 4;
    return true;
}
}  // namespace

void write_link_frame(const LinkFrame& frame, std::vector<uint8_t>* out) {
    put_u32(out, static_cast<uint32_t>(frame.wire_bytes.size()));
    out->insert(out->end(), frame.wire_bytes.begin(), frame.wire_bytes.end());
    put_u32(out, static_cast<uint32_t>(frame.fd_blobs.size()));
    for (const auto& blob : frame.fd_blobs) {
        put_u32(out, static_cast<uint32_t>(blob.size()));
        out->insert(out->end(), blob.begin(), blob.end());
    }
}

FrameResult take_link_frame(std::vector<uint8_t>* buffer, LinkFrame* out) {
    const uint8_t* data = buffer->data();
    const size_t size = buffer->size();
    size_t pos = 0;

    uint32_t wire_len = 0;
    if (!get_u32(data, size, &pos, &wire_len)) return FrameResult::NeedMoreData;
    if (wire_len > kMaxFrameBytes) return FrameResult::Malformed;
    if (pos + wire_len > size) return FrameResult::NeedMoreData;
    std::vector<uint8_t> wire_bytes(data + pos, data + pos + wire_len);
    pos += wire_len;

    uint32_t fd_count = 0;
    if (!get_u32(data, size, &pos, &fd_count)) return FrameResult::NeedMoreData;
    if (fd_count > 16) return FrameResult::Malformed;  // Wayland messages take few fds

    std::vector<std::vector<uint8_t>> blobs;
    blobs.reserve(fd_count);
    for (uint32_t i = 0; i < fd_count; ++i) {
        uint32_t blob_len = 0;
        if (!get_u32(data, size, &pos, &blob_len)) return FrameResult::NeedMoreData;
        if (blob_len > kMaxFrameBytes) return FrameResult::Malformed;
        if (pos + blob_len > size) return FrameResult::NeedMoreData;
        blobs.emplace_back(data + pos, data + pos + blob_len);
        pos += blob_len;
    }

    out->wire_bytes = std::move(wire_bytes);
    out->fd_blobs = std::move(blobs);
    buffer->erase(buffer->begin(), buffer->begin() + pos);
    return FrameResult::Ok;
}

FrameResult take_wire_message(std::vector<uint8_t>* buffer, std::vector<uint8_t>* whole_message) {
    if (buffer->size() < 8) return FrameResult::NeedMoreData;
    uint32_t header2 = 0;
    std::memcpy(&header2, buffer->data() + 4, 4);
    const uint32_t size = header2 >> 16;
    if (size < 8 || size > kMaxFrameBytes) return FrameResult::Malformed;
    if (buffer->size() < size) return FrameResult::NeedMoreData;
    whole_message->assign(buffer->begin(), buffer->begin() + size);
    buffer->erase(buffer->begin(), buffer->begin() + size);
    return FrameResult::Ok;
}

}  // namespace wire
