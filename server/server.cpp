// Server half of Vulkan-over-TCP: executes commands on the machine that owns
// the GPU and returns results to a client that has none.
//
// Scope note: physical device queries only, for now. They are the right first
// slice because they exercise the whole path (framing, opcode dispatch, handle
// translation, reply encoding) against a real driver, while returning plain
// data that genuinely fits in a socket. Commands that hand back mappable
// memory cannot be remoted this way at all, and are rejected explicitly rather
// than half-implemented.

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
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "remoting_commands.inl"
#include "wire.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

class Server {
   public:
    bool init_vulkan() {
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "vulkan-remoting-server";
        app_info.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;

        const VkResult result = vkCreateInstance(&create_info, nullptr, &m_instance);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "server: vkCreateInstance failed (%d)\n", result);
            return false;
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
    std::vector<VkPhysicalDevice> m_physical_devices;
};

void Server::serve(int fd) {
    fprintf(stderr, "server: client connected\n");

    for (;;) {
        remoting::MessageHeader header{};
        std::vector<char> payload;
        if (!remoting::recv_message(fd, &header, &payload)) break;

        const remoting::Opcode opcode = static_cast<remoting::Opcode>(header.opcode);
        remoting::Reader reader(payload.data(), payload.size());
        remoting::Writer writer;

        switch (opcode) {
            case remoting::Opcode::Handshake: {
                std::string client_digest;
                reader.string(&client_digest);
                const bool match = reader.ok() && client_digest == remoting::kCommandSetDigest;
                if (!match) {
                    // Refusing early is much kinder than letting mismatched
                    // opcode numbering silently call the wrong command.
                    fprintf(stderr,
                            "server: rejecting client, command set digest %s != %s\n",
                            client_digest.c_str(), remoting::kCommandSetDigest);
                }
                writer.u32(match ? static_cast<uint32_t>(remoting::Status::Ok)
                                 : static_cast<uint32_t>(remoting::Status::DecodeError));
                writer.string(remoting::kCommandSetDigest);
                writer.u32(static_cast<uint32_t>(m_physical_devices.size()));
                reply(fd, opcode, writer);
                if (!match) {
                    return;
                }
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
                writer.u32(props.apiVersion);
                writer.u32(props.driverVersion);
                writer.u32(props.vendorID);
                writer.u32(props.deviceID);
                writer.u32(static_cast<uint32_t>(props.deviceType));
                writer.string(props.deviceName);
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
                if (count) {
                    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
                }

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

                // VkPhysicalDeviceFeatures is a flat block of VkBool32, so it
                // can go over as-is. Anything with pointers inside could not.
                writer.u32(static_cast<uint32_t>(remoting::Status::Ok));
                writer.bytes(&features, sizeof(features));
                reply(fd, opcode, writer);
                break;
            }

            default: {
                fprintf(stderr, "server: unsupported command %s (%u)\n", remoting::opcode_name(opcode),
                        header.opcode);
                reply_status(fd, opcode, remoting::Status::UnsupportedCommand);
                break;
            }
        }
    }

    fprintf(stderr, "server: client disconnected\n");
}

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

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--address" && i + 1 < argc) {
            address = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (arg == "--help") {
            printf("usage: %s [--address ADDR] [--port PORT]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    Server server;
    if (!server.init_vulkan()) return 1;

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
