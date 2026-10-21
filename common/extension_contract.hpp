#pragma once

#include <cstddef>
#include <cstring>
#include <vulkan/vulkan.h>

namespace remoting {

struct ExtensionContractEntry { const char* name; uint32_t spec_version; };

inline constexpr ExtensionContractEntry kProperties2InstanceExtension = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION,
};

inline constexpr ExtensionContractEntry kDescriptorIndexingDeviceExtension = {
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_SPEC_VERSION,
};

inline constexpr ExtensionContractEntry kBufferDeviceAddressDeviceExtension = {
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_SPEC_VERSION,
};

inline constexpr ExtensionContractEntry kSynchronization2DeviceExtension = {
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_SPEC_VERSION,
};

inline constexpr ExtensionContractEntry kRequiredDeviceExtensions[] = {
    {VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MULTIVIEW_SPEC_VERSION},
    {VK_KHR_MAINTENANCE_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_2_SPEC_VERSION},
    {VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, VK_KHR_IMAGE_FORMAT_LIST_SPEC_VERSION},
    {VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_SPEC_VERSION},
    {VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, VK_KHR_FRAGMENT_SHADING_RATE_SPEC_VERSION},
    {VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME, VK_KHR_IMAGELESS_FRAMEBUFFER_SPEC_VERSION},
};

inline bool is_required_device_extension(const char* name) {
    if (!name) return false;
    for (const auto& extension : kRequiredDeviceExtensions)
        if (std::strcmp(name, extension.name) == 0) return true;
    return false;
}

template <typename Contains>
inline bool device_extension_dependencies_satisfied(const char* name,
                                                     const Contains& enabled,
                                                     bool properties2_enabled) {
    if (name == nullptr) return false;
    if (std::strcmp(name, VK_KHR_MULTIVIEW_EXTENSION_NAME) == 0 ||
        std::strcmp(name, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0)
        return properties2_enabled;
    if (std::strcmp(name, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) == 0)
        return enabled(VK_KHR_MULTIVIEW_EXTENSION_NAME) &&
               enabled(VK_KHR_MAINTENANCE_2_EXTENSION_NAME);
    if (std::strcmp(name, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0)
        return properties2_enabled && enabled(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    if (std::strcmp(name, VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME) == 0)
        return properties2_enabled && enabled(VK_KHR_MAINTENANCE_2_EXTENSION_NAME) &&
               enabled(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
    return true;
}

inline const char* device_extension_for_command(const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkCreateRenderPass2KHR") == 0 ||
        std::strcmp(name, "vkCmdBeginRenderPass2KHR") == 0 ||
        std::strcmp(name, "vkCmdNextSubpass2KHR") == 0 ||
        std::strcmp(name, "vkCmdEndRenderPass2KHR") == 0)
        return VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
    if (std::strcmp(name, "vkCmdSetFragmentShadingRateKHR") == 0)
        return VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
    if (std::strcmp(name, "vkGetBufferDeviceAddressKHR") == 0 ||
        std::strcmp(name, "vkGetBufferOpaqueCaptureAddressKHR") == 0 ||
        std::strcmp(name, "vkGetDeviceMemoryOpaqueCaptureAddressKHR") == 0)
        return VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
    if (std::strcmp(name, "vkQueueSubmit2KHR") == 0 ||
        std::strcmp(name, "vkCmdPipelineBarrier2KHR") == 0 ||
        std::strcmp(name, "vkCmdSetEvent2KHR") == 0 ||
        std::strcmp(name, "vkCmdResetEvent2KHR") == 0 ||
        std::strcmp(name, "vkCmdWaitEvents2KHR") == 0 ||
        std::strcmp(name, "vkCmdWriteTimestamp2KHR") == 0)
        return VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
    return nullptr;
}

inline VkResult copy_fragment_shading_rates(
    const VkPhysicalDeviceFragmentShadingRateKHR* source, uint32_t available,
    uint32_t* count, VkPhysicalDeviceFragmentShadingRateKHR* destination) {
    if (count == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (destination == nullptr) {
        *count = available;
        return VK_SUCCESS;
    }
    const uint32_t to_write = *count < available ? *count : available;
    for (uint32_t i = 0; i < to_write; ++i) {
        destination[i].sampleCounts = source[i].sampleCounts;
        destination[i].fragmentSize = source[i].fragmentSize;
    }
    *count = to_write;
    return to_write < available ? VK_INCOMPLETE : VK_SUCCESS;
}

inline void apply_vulkan12_feature_policy(VkPhysicalDeviceVulkan12Features* f) {
    if (!f) return;
    // Server-native addresses remain opaque shader data; capture/replay tokens
    // and multi-device addresses cannot cross this remoting contract.
    f->bufferDeviceAddressCaptureReplay = VK_FALSE;
    f->bufferDeviceAddressMultiDevice = VK_FALSE;
    // Fixed-size descriptor arrays use the ordinary layout/update codecs. Keep
    // their shader indexing features, but hide every feature that changes
    // descriptor cardinality or permits mutation while descriptors are in use.
    f->descriptorBindingUniformBufferUpdateAfterBind = VK_FALSE;
    f->descriptorBindingSampledImageUpdateAfterBind = VK_FALSE;
    f->descriptorBindingStorageImageUpdateAfterBind = VK_FALSE;
    f->descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
    f->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_FALSE;
    f->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_FALSE;
    f->descriptorBindingUpdateUnusedWhilePending = VK_FALSE;
    f->descriptorBindingPartiallyBound = VK_FALSE;
    f->descriptorBindingVariableDescriptorCount = VK_FALSE;
    f->runtimeDescriptorArray = VK_FALSE;
}

inline void apply_descriptor_indexing_feature_policy(VkPhysicalDeviceDescriptorIndexingFeatures* f) {
    if (!f) return;
    f->descriptorBindingUniformBufferUpdateAfterBind = VK_FALSE;
    f->descriptorBindingSampledImageUpdateAfterBind = VK_FALSE;
    f->descriptorBindingStorageImageUpdateAfterBind = VK_FALSE;
    f->descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
    f->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_FALSE;
    f->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_FALSE;
    f->descriptorBindingUpdateUnusedWhilePending = VK_FALSE;
    f->descriptorBindingPartiallyBound = VK_FALSE;
    f->descriptorBindingVariableDescriptorCount = VK_FALSE;
    f->runtimeDescriptorArray = VK_FALSE;
}

inline void apply_buffer_address_feature_policy(VkPhysicalDeviceBufferDeviceAddressFeatures* f) {
    if (!f) return;
    f->bufferDeviceAddressCaptureReplay = VK_FALSE;
    f->bufferDeviceAddressMultiDevice = VK_FALSE;
}

inline bool vulkan12_requested_features_supported(const VkPhysicalDeviceVulkan12Features& f) {
    VkPhysicalDeviceVulkan12Features allowed = f;
    apply_vulkan12_feature_policy(&allowed);
    const auto* requested = reinterpret_cast<const VkBool32*>(&f.samplerMirrorClampToEdge);
    const auto* filtered = reinterpret_cast<const VkBool32*>(&allowed.samplerMirrorClampToEdge);
    constexpr size_t count = (sizeof(VkPhysicalDeviceVulkan12Features) -
                              offsetof(VkPhysicalDeviceVulkan12Features,
                                       samplerMirrorClampToEdge)) / sizeof(VkBool32);
    for (size_t i = 0; i < count; ++i)
        if (requested[i] && !filtered[i]) return false;
    return true;
}

inline bool is_properties2_command(const char* name) {
    return name && (std::strcmp(name, "vkGetPhysicalDeviceFeatures2") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceProperties2") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties2") == 0 ||
        std::strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR") == 0);
}

} // namespace remoting
