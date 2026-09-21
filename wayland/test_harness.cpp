// Host-side verification harness: embeds WaylandProxy the way the real
// server process would, and stands in for what the Vulkan ICD side will
// eventually do — call surface_for_client_id() and draw directly onto the
// real object. That is deliberately NOT done by forwarding the app's own
// wl_shm requests (refused by proxy_client); it is done here exactly the way
// the design intends buffers to reach a surface, by using this process's own
// wl_display() connection.

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include <wayland-client.h>

#include "proxy_server.hpp"

namespace {

struct ShmState {
    wl_shm* shm = nullptr;
};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                      uint32_t version) {
    ShmState* state = static_cast<ShmState*>(data);
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener = {registry_global, registry_global_remove};

// Anonymous POSIX shm segment: random name, open O_EXCL, unlink immediately
// so the fd is the only handle — the standard idiom for a wl_shm-backed
// buffer (memfd_create would also work, but this avoids relying on a
// Linux-only syscall for what is otherwise portable POSIX shm).
void fill_random_name(char* buf) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A' + (r & 15) + (r & 16) * 2;
        r >>= 5;
    }
}

int open_anonymous_shm_fd(size_t size) {
    for (int retries = 100; retries > 0; --retries) {
        char name[] = "/wayland-proxy-test-XXXXXX";
        fill_random_name(name + sizeof(name) - 7);
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return -1;
        }
        shm_unlink(name);
        if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
    return -1;
}

// Solid ARGB8888 red, the way any wl_shm client draws: shm fd, mmap, fill.
wl_buffer* make_solid_buffer(wl_shm* shm, int width, int height, uint32_t argb) {
    const int stride = width * 4;
    const size_t size = static_cast<size_t>(stride) * height;

    const int fd = open_anonymous_shm_fd(size);
    if (fd < 0) {
        fprintf(stderr, "test_harness: shm fd for the test buffer failed: %s\n", strerror(errno));
        return nullptr;
    }
    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    uint32_t* pixels = static_cast<uint32_t*>(map);
    for (size_t i = 0; i < size / 4; ++i) pixels[i] = argb;
    munmap(map, size);

    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(size));
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);  // the pool/buffer keep the mapping alive via their own fd use
    return buffer;
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 24681;
    int seconds = 20;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(atoi(argv[++i]));
        if (arg == "--seconds" && i + 1 < argc) seconds = atoi(argv[++i]);
    }

    WaylandProxy proxy;
    if (!proxy.start(port)) return 1;

    // Bind wl_shm on our own event queue so it never interferes with the
    // objects the proxy is replaying on the default queue.
    wl_event_queue* queue = wl_display_create_queue(proxy.display());
    wl_registry* registry = wl_display_get_registry(proxy.display());
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);
    ShmState shm_state;
    wl_registry_add_listener(registry, &kRegistryListener, &shm_state);
    wl_display_roundtrip_queue(proxy.display(), queue);
    if (!shm_state.shm) {
        fprintf(stderr, "test_harness: real compositor did not advertise wl_shm\n");
        return 1;
    }

    bool attached = false;
    uint32_t found_candidate = 0;
    int ms_since_found = 0;
    for (int elapsed_ms = 0; elapsed_ms < seconds * 1000; elapsed_ms += 20) {
        if (!proxy.poll()) {
            fprintf(stderr, "test_harness: link ended\n");
            break;
        }
        if (!attached) {
            if (found_candidate == 0) {
                // Small ids are exactly what a simple client allocates first;
                // this harness has no other way to learn the app's own naming.
                for (uint32_t candidate = 1; candidate <= 64; ++candidate) {
                    if (proxy.surface_for_client_id(candidate)) {
                        found_candidate = candidate;
                        break;
                    }
                }
            } else {
                // Give the app's own xdg_surface configure/ack_configure
                // roundtrip time to land before we commit a buffer onto the
                // same surface — attaching too early hits "never configured".
                ms_since_found += 20;
                if (ms_since_found >= 500) {
                    wl_surface* surface = proxy.surface_for_client_id(found_candidate);
                    wl_buffer* buffer = surface ? make_solid_buffer(shm_state.shm, 200, 200, 0xffff0000)
                                                 : nullptr;
                    if (surface && buffer) {
                        wl_surface_attach(surface, buffer, 0, 0);
                        wl_surface_damage_buffer(surface, 0, 0, 200, 200);
                        wl_surface_commit(surface);
                        wl_display_flush(proxy.display());
                        fprintf(stderr,
                                "test_harness: attached a 200x200 solid red buffer to client surface %u\n",
                                found_candidate);
                        attached = true;
                    }
                }
            }
        }
        usleep(20 * 1000);
    }

    return attached ? 0 : 1;
}
