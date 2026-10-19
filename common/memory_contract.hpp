#pragma once

#include <vulkan/vulkan.h>

namespace remoting {

inline constexpr uint32_t kCapabilityWholeCopyHostCoherent = 1u << 0;
inline constexpr uint32_t kKnownCapabilities = kCapabilityWholeCopyHostCoherent;

inline VkMemoryPropertyFlags public_memory_property_flags(VkMemoryPropertyFlags flags,
                                                          uint32_t capabilities = 0) {
    VkMemoryPropertyFlags hidden =
        VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
        VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
    if (!(capabilities & kCapabilityWholeCopyHostCoherent))
        hidden |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    return flags & ~hidden;
}

}  // namespace remoting
