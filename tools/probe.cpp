// Client half: issues remoted Vulkan queries and times them.
//
// The timing is the point as much as the result. Every call here is a blocking
// round trip, which is how a real Vulkan client behaves for anything returning
// data. Measuring it now, on a trivial command that carries almost no payload,
// isolates pure latency from bandwidth: whatever this costs, a frame's worth of
// such calls costs that many times over.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <vector>

#include "remoting_commands.inl"
#include "wire.hpp"

namespace {

// Defined here rather than pulling in vulkan.h: the client has no driver and
// needs no Vulkan headers, which is the whole point of remoting.
constexpr uint32_t version_major(uint32_t v) { return (v >> 22) & 0x7fu; }
constexpr uint32_t version_minor(uint32_t v) { return (v >> 12) & 0x3ffu; }
constexpr uint32_t version_patch(uint32_t v) { return v & 0xfffu; }

struct Call {
    remoting::MessageHeader header{};
    std::vector<char> payload;
    double microseconds = 0.0;
    bool ok = false;
};

Call call(int fd, remoting::Opcode opcode, const remoting::Writer& request) {
    Call result;
    const auto start = std::chrono::steady_clock::now();

    if (!remoting::send_message(fd, static_cast<uint32_t>(opcode), request.data())) {
        fprintf(stderr, "client: send failed for %s\n", remoting::opcode_name(opcode));
        return result;
    }
    if (!remoting::recv_message(fd, &result.header, &result.payload)) {
        fprintf(stderr, "client: no reply for %s\n", remoting::opcode_name(opcode));
        return result;
    }

    const auto end = std::chrono::steady_clock::now();
    result.microseconds =
        std::chrono::duration<double, std::micro>(end - start).count();
    result.ok = true;
    return result;
}

const char* device_type_name(uint32_t type) {
    switch (type) {
        case 1: return "integrated GPU";
        case 2: return "discrete GPU";
        case 3: return "virtual GPU";
        case 4: return "CPU";
        default: return "other";
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 24680;
    int repeats = 100;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (arg == "--repeats" && i + 1 < argc) {
            repeats = atoi(argv[++i]);
        } else if (arg == "--help") {
            printf("usage: %s [--host HOST] [--port PORT] [--repeats N]\n", argv[0]);
            return 0;
        }
    }

    const int fd = remoting::connect_to(host, port);
    if (fd < 0) return 1;
    printf("connected to %s:%u\n", host.c_str(), static_cast<unsigned>(port));

    {
        remoting::Writer request;
        request.string(remoting::kCommandSetDigest);
        const Call reply = call(fd, remoting::Opcode::Handshake, request);
        if (!reply.ok) return 1;

        remoting::Reader reader(reply.payload.data(), reply.payload.size());
        const uint32_t status = reader.u32();
        std::string server_digest;
        reader.string(&server_digest);
        const uint32_t device_count = reader.u32();

        if (status != static_cast<uint32_t>(remoting::Status::Ok)) {
            fprintf(stderr,
                    "client: server rejected handshake, its command set is %s, ours is %s\n",
                    server_digest.c_str(), remoting::kCommandSetDigest);
            return 1;
        }
        printf("handshake ok, command set %s, %u device(s), %.0f us\n", server_digest.c_str(),
               device_count, reply.microseconds);
    }

    std::vector<uint64_t> devices;
    {
        remoting::Writer request;
        const Call reply = call(fd, remoting::Opcode::vkEnumeratePhysicalDevices, request);
        if (!reply.ok) return 1;

        remoting::Reader reader(reply.payload.data(), reply.payload.size());
        reader.u32();  // status
        const int32_t vk_result = reader.i32();
        const uint32_t count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) devices.push_back(reader.handle());

        printf("vkEnumeratePhysicalDevices -> VkResult %d, %u device(s), %.0f us\n", vk_result,
               count, reply.microseconds);
    }

    for (uint64_t id : devices) {
        remoting::Writer request;
        request.handle(id);
        const Call reply = call(fd, remoting::Opcode::vkGetPhysicalDeviceProperties, request);
        if (!reply.ok) return 1;

        remoting::Reader reader(reply.payload.data(), reply.payload.size());
        const uint32_t status = reader.u32();
        if (status != static_cast<uint32_t>(remoting::Status::Ok)) {
            fprintf(stderr, "client: properties query failed for handle %llu\n",
                    static_cast<unsigned long long>(id));
            continue;
        }
        const uint32_t api_version = reader.u32();
        reader.u32();  // driverVersion
        const uint32_t vendor_id = reader.u32();
        reader.u32();  // deviceID
        const uint32_t device_type = reader.u32();
        std::string name;
        reader.string(&name);

        printf("  device %llu: %s (%s, vendor 0x%04x, Vulkan %u.%u.%u) %.0f us\n",
               static_cast<unsigned long long>(id), name.c_str(), device_type_name(device_type),
               vendor_id, version_major(api_version), version_minor(api_version),
               version_patch(api_version), reply.microseconds);
    }

    if (repeats > 0 && !devices.empty()) {
        // Same trivial command over and over: this is the round-trip floor,
        // the cost a real workload pays per synchronous call no matter how
        // little data it moves.
        double total = 0.0;
        double worst = 0.0;
        for (int i = 0; i < repeats; ++i) {
            remoting::Writer request;
            request.handle(devices[0]);
            const Call reply = call(fd, remoting::Opcode::vkGetPhysicalDeviceProperties, request);
            if (!reply.ok) return 1;
            total += reply.microseconds;
            if (reply.microseconds > worst) worst = reply.microseconds;
        }
        const double mean = total / repeats;
        printf("\nround trip over %d calls: mean %.1f us, worst %.1f us\n", repeats, mean, worst);
        printf("at that latency a 16.7 ms frame affords about %.0f synchronous calls\n",
               16700.0 / mean);
    }

    ::close(fd);
    return 0;
}
