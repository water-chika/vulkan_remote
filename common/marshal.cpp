#include "marshal.hpp"
#include "extension_contract.hpp"

#include <cstring>
#include <limits>

namespace remoting {
namespace {

// A non-dispatchable handle's bit pattern IS the id while it is still on the
// client, so extracting it is just a reinterpret of the pointer-sized value.
template <typename H>
uint64_t id_of(H h) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
}

template <typename H, typename Resolve>
void patch_handle(H* field, Resolve resolve) {
    const uint64_t id = id_of(*field);
    *field = id ? resolve(id) : static_cast<H>(0);
}

template <typename T>
bool read_raw(Reader& r, T* out) {
    std::vector<char> buf;
    if (!r.bytes(&buf) || buf.size() != sizeof(T)) return false;
    std::memcpy(out, buf.data(), sizeof(T));
    return true;
}

template <typename T>
void write_pod_array(Writer& w, const T* arr, uint32_t count) {
    w.bytes(count ? arr : nullptr, static_cast<size_t>(count) * sizeof(T));
}

template <typename T>
bool read_pod_array(Reader& r, Arena& arena, uint32_t count, T** out) {
    if (count > kMaxArrayElements) return false;
    // write_pod_array always emits a length-prefixed blob, even for count 0,
    // so this must always consume one to stay in sync with the wire.
    std::vector<char> buf;
    if (!r.bytes(&buf)) return false;
    if (buf.size() != static_cast<size_t>(count) * sizeof(T)) return false;
    if (count == 0) {
        *out = nullptr;
        return true;
    }
    T* p = arena.array<T>(count);
    std::memcpy(p, buf.data(), buf.size());
    *out = p;
    return true;
}

// For arrays whose presence is independent of count (e.g. pViewports, which
// may be null when the viewport count is set dynamically).
template <typename T>
void write_opt_pod_array(Writer& w, const T* arr, uint32_t count) {
    w.u32(arr ? 1 : 0);
    if (arr) write_pod_array(w, arr, count);
}

template <typename T>
bool read_opt_pod_array(Reader& r, Arena& arena, uint32_t count, T** out) {
    const uint32_t present = r.u32();
    if (!r.ok() || count > kMaxArrayElements) return false;
    if (!present) {
        *out = nullptr;
        return true;
    }
    return read_pod_array(r, arena, count, out);
}

template <typename T>
void write_opt_pod(Writer& w, const T* p) {
    w.u32(p ? 1 : 0);
    if (p) w.bytes(p, sizeof(T));
}

template <typename T>
bool read_opt_pod(Reader& r, Arena& arena, T** out) {
    const uint32_t present = r.u32();
    if (!r.ok()) return false;
    if (!present) {
        *out = nullptr;
        return true;
    }
    std::vector<char> buf;
    if (!r.bytes(&buf) || buf.size() != sizeof(T)) return false;
    T* p = arena.array<T>(1);
    std::memcpy(p, buf.data(), sizeof(T));
    *out = p;
    return true;
}

template <typename H>
void write_handle_array(Writer& w, const H* arr, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) w.handle(id_of(arr[i]));
}

template <typename H, typename Resolve>
bool read_handle_array(Reader& r, Arena& arena, uint32_t count, Resolve resolve, H** out) {
    if (count == 0) {
        *out = nullptr;
        return true;
    }
    if (count > kMaxArrayElements) return false;
    H* p = arena.array<H>(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t id = r.handle();
        if (!r.ok()) return false;
        p[i] = id ? resolve(id) : static_cast<H>(0);
    }
    *out = p;
    return true;
}

void write_string_array(Writer& w, const char* const* arr, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) w.string(arr[i]);
}

bool read_string_array(Reader& r, Arena& arena, uint32_t count, const char*** out) {
    if (count == 0) {
        *out = nullptr;
        return true;
    }
    if (count > kMaxArrayElements) return false;
    const char** p = arena.array<const char*>(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string s;
        if (!r.string(&s)) return false;
        char* buf = static_cast<char*>(arena.allocate(s.size() + 1));
        std::memcpy(buf, s.c_str(), s.size() + 1);
        p[i] = buf;
    }
    *out = p;
    return true;
}

bool read_bytes_to_arena(Reader& r, Arena& arena, size_t expected_size, void** out) {
    std::vector<char> buf;
    if (!r.bytes(&buf)) return false;
    if (buf.size() != expected_size) return false;
    if (expected_size == 0) {
        *out = nullptr;
        return true;
    }
    void* p = arena.allocate(expected_size);
    std::memcpy(p, buf.data(), expected_size);
    *out = p;
    return true;
}

// --- VkSpecializationInfo, nested inside VkPipelineShaderStageCreateInfo ---

void write_SpecializationInfo(Writer& w, const VkSpecializationInfo& s) {
    VkSpecializationInfo tmp = s;
    tmp.pMapEntries = nullptr;
    tmp.pData = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_pod_array(w, s.pMapEntries, s.mapEntryCount);
    w.bytes(s.pData, s.dataSize);
}

bool read_SpecializationInfo(Reader& r, Arena& arena, VkSpecializationInfo* out) {
    if (!read_raw(r, out)) return false;
    if (!read_pod_array(r, arena, out->mapEntryCount,
                         const_cast<VkSpecializationMapEntry**>(&out->pMapEntries)))
        return false;
    void* data = nullptr;
    if (!read_bytes_to_arena(r, arena, out->dataSize, &data)) return false;
    out->pData = data;
    return true;
}

// --- VkPipelineShaderStageCreateInfo, nested inside a pipeline's pStages ---

void write_PipelineShaderStageCreateInfo(Writer& w, const VkPipelineShaderStageCreateInfo& s) {
    VkPipelineShaderStageCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pName = nullptr;
    tmp.pSpecializationInfo = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    w.string(s.pName);
    w.u32(s.pSpecializationInfo ? 1 : 0);
    if (s.pSpecializationInfo) write_SpecializationInfo(w, *s.pSpecializationInfo);
}

bool read_PipelineShaderStageCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                         VkPipelineShaderStageCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    std::string name;
    if (!r.string(&name)) return false;
    char* name_buf = static_cast<char*>(arena.allocate(name.size() + 1));
    std::memcpy(name_buf, name.c_str(), name.size() + 1);
    out->pName = name_buf;

    const uint32_t present = r.u32();
    if (!r.ok()) return false;
    if (present) {
        VkSpecializationInfo* si = arena.array<VkSpecializationInfo>(1);
        if (!read_SpecializationInfo(r, arena, si)) return false;
        out->pSpecializationInfo = si;
    } else {
        out->pSpecializationInfo = nullptr;
    }

    const uint64_t module_id = id_of(out->module);
    patch_handle(&out->module, [&](uint64_t id) { return hr.shader_module(id); });
    return !module_id || out->module != VK_NULL_HANDLE;
}

// --- VkSubpassDescription, nested inside VkRenderPassCreateInfo::pSubpasses ---

void write_SubpassDescription(Writer& w, const VkSubpassDescription& s) {
    VkSubpassDescription tmp = s;
    tmp.pInputAttachments = nullptr;
    tmp.pColorAttachments = nullptr;
    tmp.pResolveAttachments = nullptr;
    tmp.pDepthStencilAttachment = nullptr;
    tmp.pPreserveAttachments = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_pod_array(w, s.pInputAttachments, s.inputAttachmentCount);
    write_pod_array(w, s.pColorAttachments, s.colorAttachmentCount);
    write_opt_pod_array(w, s.pResolveAttachments, s.colorAttachmentCount);
    write_opt_pod(w, s.pDepthStencilAttachment);
    write_pod_array(w, s.pPreserveAttachments, s.preserveAttachmentCount);
}

bool read_SubpassDescription(Reader& r, Arena& arena, VkSubpassDescription* out) {
    if (!read_raw(r, out)) return false;
    if (!read_pod_array(r, arena, out->inputAttachmentCount,
                         const_cast<VkAttachmentReference**>(&out->pInputAttachments)))
        return false;
    if (!read_pod_array(r, arena, out->colorAttachmentCount,
                         const_cast<VkAttachmentReference**>(&out->pColorAttachments)))
        return false;
    if (!read_opt_pod_array(r, arena, out->colorAttachmentCount,
                             const_cast<VkAttachmentReference**>(&out->pResolveAttachments)))
        return false;
    if (!read_opt_pod(r, arena, const_cast<VkAttachmentReference**>(&out->pDepthStencilAttachment)))
        return false;
    if (!read_pod_array(r, arena, out->preserveAttachmentCount,
                         const_cast<uint32_t**>(&out->pPreserveAttachments)))
        return false;
    return true;
}

}  // namespace

namespace {

bool pnext_allowed(PNextContext context, VkStructureType type) {
    switch (context) {
        case PNextContext::QueryFeaturesOutput:
            return type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
        case PNextContext::ImageFormatPropertiesOutput:
            return type == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES;
        case PNextContext::QueryPropertiesOutput:
            return type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
        case PNextContext::DeviceCreate:
            return type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES ||
                   type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
        case PNextContext::RenderPass:
            return type == VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO ||
                   type == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
        case PNextContext::RenderPass2:
            return type == VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
        case PNextContext::GraphicsPipeline:
            return type == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
        case PNextContext::TessellationState:
            return type == VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO;
        case PNextContext::ImageCreate:
            return type == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        case PNextContext::ImageViewCreate:
            return type == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO ||
                   type == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        case PNextContext::SamplerCreate:
            return type == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        case PNextContext::MemoryRequirements2:
            return type == VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO ||
                   type == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
        case PNextContext::BindBufferMemory2:
            return false;
        case PNextContext::BindImageMemory2:
            return type == VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO;
        case PNextContext::DescriptorSetLayoutSupport:
            return false;
        case PNextContext::FramebufferCreate:
            return type == VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO;
        case PNextContext::RenderPassBegin:
            return type == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO;
        case PNextContext::SemaphoreCreate:
            return type == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        case PNextContext::Submit:
            return type == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        case PNextContext::BufferCreate:
            return type == VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO;
        case PNextContext::MemoryAllocate:
            return type == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        case PNextContext::ShaderModule:
            return false;
    }
    return false;
}

size_t pnext_node_size(VkStructureType type) {
    switch (type) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            return sizeof(VkPhysicalDeviceFeatures2);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan11Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
            return sizeof(VkPhysicalDeviceVulkan11Properties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan12Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
            return sizeof(VkPhysicalDeviceVulkan12Properties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
            return sizeof(VkPhysicalDeviceHostQueryResetFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            return sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
            return sizeof(VkPhysicalDeviceSynchronization2Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:
            return sizeof(VkPhysicalDeviceTimelineSemaphoreProperties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
            return sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
            return sizeof(VkPhysicalDeviceDescriptorIndexingFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:
            return sizeof(VkPhysicalDeviceDescriptorIndexingProperties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
            return sizeof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES:
            return sizeof(VkPhysicalDeviceDepthStencilResolveProperties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
            return sizeof(VkPhysicalDeviceProtectedMemoryFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
            return sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:
            return sizeof(VkPhysicalDeviceSubgroupProperties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES:
            return sizeof(VkPhysicalDeviceIDProperties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:
            return sizeof(VkPhysicalDeviceProtectedMemoryProperties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES:
            return sizeof(VkPhysicalDeviceMaintenance3Properties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
            return sizeof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR:
            return sizeof(VkPhysicalDeviceFragmentShadingRatePropertiesKHR);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
            return sizeof(VkPhysicalDeviceImagelessFramebufferFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
            return sizeof(VkPhysicalDeviceMultiviewFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES:
            return sizeof(VkPhysicalDeviceMultiviewProperties);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES:
            return sizeof(VkPhysicalDevicePointClippingProperties);
        case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO:
            return sizeof(VkRenderPassInputAttachmentAspectCreateInfo);
        case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
            return sizeof(VkRenderPassMultiviewCreateInfo);
        case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR:
            return sizeof(VkFragmentShadingRateAttachmentInfoKHR);
        case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
            return sizeof(VkPipelineFragmentShadingRateStateCreateInfoKHR);
        case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
            return sizeof(VkPipelineTessellationDomainOriginStateCreateInfo);
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO:
            return sizeof(VkImageFormatListCreateInfo);
        case VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO:
            return sizeof(VkImageViewUsageCreateInfo);
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO:
            return sizeof(VkSamplerYcbcrConversionInfo);
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES:
            return sizeof(VkSamplerYcbcrConversionImageFormatProperties);
        case VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO:
            return sizeof(VkImagePlaneMemoryRequirementsInfo);
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS:
            return sizeof(VkMemoryDedicatedRequirements);
        case VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO:
            return sizeof(VkBindImagePlaneMemoryInfo);
        case VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO:
            return sizeof(VkFramebufferAttachmentsCreateInfo);
        case VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO:
            return sizeof(VkRenderPassAttachmentBeginInfo);
        case VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO:
            return sizeof(VkSemaphoreTypeCreateInfo);
        case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:
            return sizeof(VkTimelineSemaphoreSubmitInfo);
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO:
            return sizeof(VkBufferOpaqueCaptureAddressCreateInfo);
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO:
            return sizeof(VkMemoryAllocateFlagsInfo);
        default:
            return 0;
    }
}

bool valid_count(uint32_t count, size_t element_size) {
    return count <= kMaxArrayElements &&
           (element_size == 0
                ? count == 0
                : static_cast<size_t>(count) <=
                      (std::numeric_limits<size_t>::max)() / element_size);
}

bool collect_chain(PNextContext context, const void* head,
                   std::vector<const VkBaseInStructure*>* nodes) {
    for (auto* node = static_cast<const VkBaseInStructure*>(head); node; node = node->pNext) {
        if (context == PNextContext::DeviceCreate &&
            node->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
            continue;
        if (nodes->size() == kMaxPNextNodes || !pnext_allowed(context, node->sType)) return false;
        for (const auto* previous : *nodes)
            if (previous->sType == node->sType) return false;
        nodes->push_back(node);
    }
    return true;
}

void write_extent(Writer& w, const VkExtent2D& e) {
    w.u32(e.width);
    w.u32(e.height);
}

bool read_extent(Reader& r, VkExtent2D* e) {
    e->width = r.u32();
    e->height = r.u32();
    return r.ok();
}

void write_attachment_reference2(Writer& w, const VkAttachmentReference2& s) {
    w.i32(static_cast<int32_t>(s.sType));
    w.u32(s.attachment);
    w.i32(static_cast<int32_t>(s.layout));
    w.u32(s.aspectMask);
}

bool read_attachment_reference2(Reader& r, VkAttachmentReference2* s) {
    *s = {};
    s->sType = static_cast<VkStructureType>(r.i32());
    s->attachment = r.u32();
    s->layout = static_cast<VkImageLayout>(r.i32());
    s->aspectMask = r.u32();
    return r.ok() && s->sType == VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
}

bool write_node_fields(Writer& w, PNextContext context, const VkBaseInStructure* base,
                       uint32_t depth) {
    (void)context;
    switch (base->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            write_PhysicalDeviceFeatures(w,
                reinterpret_cast<const VkPhysicalDeviceFeatures2*>(base)->features);
            return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
            const auto&s=*reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(base);
            w.u32(VK_FALSE);w.u32(VK_FALSE);w.u32(VK_FALSE);w.u32(VK_FALSE);
            w.u32(s.multiview);w.u32(s.multiviewGeometryShader);w.u32(s.multiviewTessellationShader);
            w.u32(VK_FALSE);w.u32(VK_FALSE);w.u32(VK_FALSE);w.u32(s.samplerYcbcrConversion);w.u32(VK_FALSE);return true;
        }

        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:{auto f=*reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(base);remoting::apply_vulkan12_feature_policy(&f);const auto*v=reinterpret_cast<const VkBool32*>(&f.samplerMirrorClampToEdge);constexpr size_t n=(sizeof(f)-offsetof(VkPhysicalDeviceVulkan12Features,samplerMirrorClampToEdge))/sizeof(VkBool32);for(size_t i=0;i<n;++i)w.u32(v[i]);return true;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:w.u32(reinterpret_cast<const VkPhysicalDeviceHostQueryResetFeatures*>(base)->hostQueryReset);return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:w.u32(reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(base)->timelineSemaphore);return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:w.u32(reinterpret_cast<const VkPhysicalDeviceSynchronization2Features*>(base)->synchronization2);return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:{auto f=*reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(base);remoting::apply_buffer_address_feature_policy(&f);w.u32(f.bufferDeviceAddress);w.u32(f.bufferDeviceAddressCaptureReplay);w.u32(f.bufferDeviceAddressMultiDevice);return true;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:{auto f=*reinterpret_cast<const VkPhysicalDeviceDescriptorIndexingFeatures*>(base);remoting::apply_descriptor_indexing_feature_policy(&f);const auto*v=reinterpret_cast<const VkBool32*>(&f.shaderInputAttachmentArrayDynamicIndexing);constexpr size_t n=(sizeof(f)-offsetof(VkPhysicalDeviceDescriptorIndexingFeatures,shaderInputAttachmentArrayDynamicIndexing))/sizeof(VkBool32);for(size_t i=0;i<n;++i)w.u32(v[i]);return true;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:w.u32(reinterpret_cast<const VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures*>(base)->separateDepthStencilLayouts);return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:w.bytes(reinterpret_cast<const char*>(base)+offsetof(VkPhysicalDeviceVulkan12Properties,driverID),sizeof(VkPhysicalDeviceVulkan12Properties)-offsetof(VkPhysicalDeviceVulkan12Properties,driverID));return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:w.u64(reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreProperties*>(base)->maxTimelineSemaphoreValueDifference);return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES:{const auto&x=*reinterpret_cast<const VkPhysicalDeviceDepthStencilResolveProperties*>(base);w.u32(x.supportedDepthResolveModes);w.u32(x.supportedStencilResolveModes);w.u32(x.independentResolveNone);w.u32(x.independentResolve);return true;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:{auto x=*reinterpret_cast<const VkPhysicalDeviceDescriptorIndexingProperties*>(base);std::memset(reinterpret_cast<char*>(&x)+offsetof(VkPhysicalDeviceDescriptorIndexingProperties,maxUpdateAfterBindDescriptorsInAllPools),0,sizeof(x)-offsetof(VkPhysicalDeviceDescriptorIndexingProperties,maxUpdateAfterBindDescriptorsInAllPools));w.bytes(reinterpret_cast<const char*>(&x)+offsetof(VkPhysicalDeviceDescriptorIndexingProperties,maxUpdateAfterBindDescriptorsInAllPools),sizeof(x)-offsetof(VkPhysicalDeviceDescriptorIndexingProperties,maxUpdateAfterBindDescriptorsInAllPools));return true;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
            w.u32(VK_FALSE);return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
            w.u32(reinterpret_cast<const VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(base)->samplerYcbcrConversion);return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
            const auto&s=*reinterpret_cast<const VkPhysicalDeviceVulkan11Properties*>(base);
            w.bytes(s.deviceUUID,VK_UUID_SIZE);w.bytes(s.driverUUID,VK_UUID_SIZE);w.bytes(s.deviceLUID,VK_LUID_SIZE);w.u32(s.deviceNodeMask);w.u32(s.deviceLUIDValid);
            w.u32(s.subgroupSize);w.u32(s.subgroupSupportedStages);w.u32(s.subgroupSupportedOperations);w.u32(s.subgroupQuadOperationsInAllStages);
            w.i32(s.pointClippingBehavior);w.u32(s.maxMultiviewViewCount);w.u32(s.maxMultiviewInstanceIndex);w.u32(VK_FALSE);w.u32(s.maxPerSetDescriptors);w.u64(s.maxMemoryAllocationSize);return true;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:{const auto&s=*reinterpret_cast<const VkPhysicalDeviceSubgroupProperties*>(base);w.u32(s.subgroupSize);w.u32(s.supportedStages);w.u32(s.supportedOperations);w.u32(s.quadOperationsInAllStages);return true;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES:{const auto&s=*reinterpret_cast<const VkPhysicalDeviceIDProperties*>(base);w.bytes(s.deviceUUID,VK_UUID_SIZE);w.bytes(s.driverUUID,VK_UUID_SIZE);w.bytes(s.deviceLUID,VK_LUID_SIZE);w.u32(s.deviceNodeMask);w.u32(s.deviceLUIDValid);return true;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:w.u32(VK_FALSE);return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES:{const auto&s=*reinterpret_cast<const VkPhysicalDeviceMaintenance3Properties*>(base);w.u32(s.maxPerSetDescriptors);w.u64(s.maxMemoryAllocationSize);return true;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR: {
            const auto& s = *reinterpret_cast<const VkPhysicalDeviceFragmentShadingRateFeaturesKHR*>(base);
            w.u32(s.pipelineFragmentShadingRate); w.u32(s.primitiveFragmentShadingRate);
            w.u32(s.attachmentFragmentShadingRate); return true;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR: {
            const auto& s = *reinterpret_cast<const VkPhysicalDeviceFragmentShadingRatePropertiesKHR*>(base);
            write_extent(w, s.minFragmentShadingRateAttachmentTexelSize);
            write_extent(w, s.maxFragmentShadingRateAttachmentTexelSize);
            w.u32(s.maxFragmentShadingRateAttachmentTexelSizeAspectRatio);
            w.u32(s.primitiveFragmentShadingRateWithMultipleViewports);
            w.u32(s.layeredShadingRateAttachments); w.u32(s.fragmentShadingRateNonTrivialCombinerOps);
            write_extent(w, s.maxFragmentSize); w.u32(s.maxFragmentSizeAspectRatio);
            w.u32(s.maxFragmentShadingRateCoverageSamples);
            w.u32(static_cast<uint32_t>(s.maxFragmentShadingRateRasterizationSamples));
            w.u32(s.fragmentShadingRateWithShaderDepthStencilWrites);
            w.u32(s.fragmentShadingRateWithSampleMask); w.u32(s.fragmentShadingRateWithShaderSampleMask);
            w.u32(s.fragmentShadingRateWithConservativeRasterization);
            w.u32(s.fragmentShadingRateWithFragmentShaderInterlock);
            w.u32(s.fragmentShadingRateWithCustomSampleLocations);
            w.u32(s.fragmentShadingRateStrictMultiplyCombiner); return true;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
            w.u32(reinterpret_cast<const VkPhysicalDeviceImagelessFramebufferFeatures*>(base)->imagelessFramebuffer); return true;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
            const auto& s = *reinterpret_cast<const VkPhysicalDeviceMultiviewFeatures*>(base);
            w.u32(s.multiview); w.u32(s.multiviewGeometryShader); w.u32(s.multiviewTessellationShader); return true;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES: {
            const auto& s = *reinterpret_cast<const VkPhysicalDeviceMultiviewProperties*>(base);
            w.u32(s.maxMultiviewViewCount); w.u32(s.maxMultiviewInstanceIndex); return true;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES:
            w.i32(static_cast<int32_t>(reinterpret_cast<const VkPhysicalDevicePointClippingProperties*>(base)->pointClippingBehavior)); return true;
        case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO: {
            const auto& s = *reinterpret_cast<const VkRenderPassInputAttachmentAspectCreateInfo*>(base);
            if (!valid_count(s.aspectReferenceCount, sizeof(VkInputAttachmentAspectReference)) ||
                (s.aspectReferenceCount && !s.pAspectReferences)) return false;
            w.u32(s.aspectReferenceCount);
            for (uint32_t i = 0; i < s.aspectReferenceCount; ++i) {
                w.u32(s.pAspectReferences[i].subpass); w.u32(s.pAspectReferences[i].inputAttachmentIndex);
                w.u32(s.pAspectReferences[i].aspectMask);
            }
            return true;
        }
        case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO: {
            const auto& s = *reinterpret_cast<const VkRenderPassMultiviewCreateInfo*>(base);
            if (!valid_count(s.subpassCount, sizeof(uint32_t)) ||
                !valid_count(s.dependencyCount, sizeof(int32_t)) ||
                !valid_count(s.correlationMaskCount, sizeof(uint32_t)) ||
                (s.subpassCount && !s.pViewMasks) || (s.dependencyCount && !s.pViewOffsets) ||
                (s.correlationMaskCount && !s.pCorrelationMasks)) return false;
            w.u32(s.subpassCount); for (uint32_t i=0;i<s.subpassCount;++i) w.u32(s.pViewMasks[i]);
            w.u32(s.dependencyCount); for (uint32_t i=0;i<s.dependencyCount;++i) w.i32(s.pViewOffsets[i]);
            w.u32(s.correlationMaskCount); for (uint32_t i=0;i<s.correlationMaskCount;++i) w.u32(s.pCorrelationMasks[i]);
            return true;
        }
        case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR: {
            if (depth >= kMaxPNextDepth) return false;
            const auto& s = *reinterpret_cast<const VkFragmentShadingRateAttachmentInfoKHR*>(base);
            w.u32(s.pFragmentShadingRateAttachment ? 1u : 0u);
            if (s.pFragmentShadingRateAttachment) {
                if (s.pFragmentShadingRateAttachment->pNext) return false;
                write_attachment_reference2(w, *s.pFragmentShadingRateAttachment);
            }
            write_extent(w, s.shadingRateAttachmentTexelSize); return true;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR: {
            const auto& s = *reinterpret_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR*>(base);
            write_extent(w, s.fragmentSize); w.i32(static_cast<int32_t>(s.combinerOps[0]));
            w.i32(static_cast<int32_t>(s.combinerOps[1])); return true;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
            w.i32(static_cast<int32_t>(reinterpret_cast<const VkPipelineTessellationDomainOriginStateCreateInfo*>(base)->domainOrigin)); return true;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            const auto& s = *reinterpret_cast<const VkImageFormatListCreateInfo*>(base);
            if (!valid_count(s.viewFormatCount, sizeof(VkFormat)) || (s.viewFormatCount && !s.pViewFormats)) return false;
            w.u32(s.viewFormatCount); for (uint32_t i=0;i<s.viewFormatCount;++i) w.i32(static_cast<int32_t>(s.pViewFormats[i])); return true;
        }
        case VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO:
            w.u32(reinterpret_cast<const VkImageViewUsageCreateInfo*>(base)->usage); return true;
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO:
            w.handle(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(reinterpret_cast<const VkSamplerYcbcrConversionInfo*>(base)->conversion))); return true;
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES:
            w.u32(reinterpret_cast<const VkSamplerYcbcrConversionImageFormatProperties*>(base)->combinedImageSamplerDescriptorCount);return true;
        case VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO:
            w.u32(reinterpret_cast<const VkImagePlaneMemoryRequirementsInfo*>(base)->planeAspect); return true;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
            const auto& s=*reinterpret_cast<const VkMemoryDedicatedRequirements*>(base);
            w.u32(s.prefersDedicatedAllocation);w.u32(s.requiresDedicatedAllocation);return true;
        }
        case VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO:
            w.u32(reinterpret_cast<const VkBindImagePlaneMemoryInfo*>(base)->planeAspect); return true;
        case VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO: {
            if (depth >= kMaxPNextDepth) return false;
            const auto& s = *reinterpret_cast<const VkFramebufferAttachmentsCreateInfo*>(base);
            if (!valid_count(s.attachmentImageInfoCount, sizeof(VkFramebufferAttachmentImageInfo)) ||
                (s.attachmentImageInfoCount && !s.pAttachmentImageInfos)) return false;
            w.u32(s.attachmentImageInfoCount);
            for (uint32_t i=0;i<s.attachmentImageInfoCount;++i) {
                const auto& a=s.pAttachmentImageInfos[i];
                if (a.sType != VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO || a.pNext ||
                    !valid_count(a.viewFormatCount,sizeof(VkFormat)) || (a.viewFormatCount && !a.pViewFormats)) return false;
                w.u32(a.flags); w.u32(a.usage); w.u32(a.width); w.u32(a.height); w.u32(a.layerCount);
                w.u32(a.viewFormatCount); for(uint32_t j=0;j<a.viewFormatCount;++j) w.i32(static_cast<int32_t>(a.pViewFormats[j]));
            }
            return true;
        }

        case VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO:{const auto&x=*reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(base);w.i32(x.semaphoreType);w.u64(x.initialValue);return true;}
        case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:{const auto&x=*reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(base);if(!valid_count(x.waitSemaphoreValueCount,sizeof(uint64_t))||!valid_count(x.signalSemaphoreValueCount,sizeof(uint64_t))||(x.waitSemaphoreValueCount&&!x.pWaitSemaphoreValues)||(x.signalSemaphoreValueCount&&!x.pSignalSemaphoreValues))return false;w.u32(x.waitSemaphoreValueCount);for(uint32_t i=0;i<x.waitSemaphoreValueCount;++i)w.u64(x.pWaitSemaphoreValues[i]);w.u32(x.signalSemaphoreValueCount);for(uint32_t i=0;i<x.signalSemaphoreValueCount;++i)w.u64(x.pSignalSemaphoreValues[i]);return true;}
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO:w.u64(reinterpret_cast<const VkBufferOpaqueCaptureAddressCreateInfo*>(base)->opaqueCaptureAddress);return true;
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO:{const auto&x=*reinterpret_cast<const VkMemoryAllocateFlagsInfo*>(base);if((x.flags&~VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)!=0||x.deviceMask!=0)return false;w.u32(x.flags);w.u32(x.deviceMask);return true;}
        case VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO: {
            const auto& s = *reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(base);
            if (!valid_count(s.attachmentCount,sizeof(VkImageView)) || (s.attachmentCount && !s.pAttachments)) return false;
            w.u32(s.attachmentCount); write_handle_array(w,s.pAttachments,s.attachmentCount); return true;
        }
        default: return false;
    }
}

bool read_node_fields(Reader& r, Arena& arena, const HandleResolver& hr,
                      VkStructureType type, void* storage, uint32_t depth) {
    switch (type) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            if (!read_PhysicalDeviceFeatures(
                    r, &static_cast<VkPhysicalDeviceFeatures2*>(storage)->features))
                return false;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:{auto&s=*static_cast<VkPhysicalDeviceVulkan11Features*>(storage);s.storageBuffer16BitAccess=r.u32();s.uniformAndStorageBuffer16BitAccess=r.u32();s.storagePushConstant16=r.u32();s.storageInputOutput16=r.u32();s.multiview=r.u32();s.multiviewGeometryShader=r.u32();s.multiviewTessellationShader=r.u32();s.variablePointersStorageBuffer=r.u32();s.variablePointers=r.u32();s.protectedMemory=r.u32();s.samplerYcbcrConversion=r.u32();s.shaderDrawParameters=r.u32();break;}

        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:{auto&f=*static_cast<VkPhysicalDeviceVulkan12Features*>(storage);auto*v=reinterpret_cast<VkBool32*>(&f.samplerMirrorClampToEdge);constexpr size_t n=(sizeof(f)-offsetof(VkPhysicalDeviceVulkan12Features,samplerMirrorClampToEdge))/sizeof(VkBool32);for(size_t i=0;i<n;++i)v[i]=r.u32();break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:static_cast<VkPhysicalDeviceHostQueryResetFeatures*>(storage)->hostQueryReset=r.u32();break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:static_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(storage)->timelineSemaphore=r.u32();break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:static_cast<VkPhysicalDeviceSynchronization2Features*>(storage)->synchronization2=r.u32();break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:{auto&f=*static_cast<VkPhysicalDeviceBufferDeviceAddressFeatures*>(storage);f.bufferDeviceAddress=r.u32();f.bufferDeviceAddressCaptureReplay=r.u32();f.bufferDeviceAddressMultiDevice=r.u32();break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:{auto&f=*static_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(storage);auto*v=reinterpret_cast<VkBool32*>(&f.shaderInputAttachmentArrayDynamicIndexing);constexpr size_t n=(sizeof(f)-offsetof(VkPhysicalDeviceDescriptorIndexingFeatures,shaderInputAttachmentArrayDynamicIndexing))/sizeof(VkBool32);for(size_t i=0;i<n;++i)v[i]=r.u32();break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:static_cast<VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures*>(storage)->separateDepthStencilLayouts=r.u32();break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:{std::vector<char>b;constexpr size_t n=sizeof(VkPhysicalDeviceVulkan12Properties)-offsetof(VkPhysicalDeviceVulkan12Properties,driverID);if(!r.bytes(&b,n)||b.size()!=n)return false;memcpy(reinterpret_cast<char*>(storage)+offsetof(VkPhysicalDeviceVulkan12Properties,driverID),b.data(),n);break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:static_cast<VkPhysicalDeviceTimelineSemaphoreProperties*>(storage)->maxTimelineSemaphoreValueDifference=r.u64();break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES:{auto&x=*static_cast<VkPhysicalDeviceDepthStencilResolveProperties*>(storage);x.supportedDepthResolveModes=r.u32();x.supportedStencilResolveModes=r.u32();x.independentResolveNone=r.u32();x.independentResolve=r.u32();break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:{std::vector<char>b;constexpr size_t n=sizeof(VkPhysicalDeviceDescriptorIndexingProperties)-offsetof(VkPhysicalDeviceDescriptorIndexingProperties,maxUpdateAfterBindDescriptorsInAllPools);if(!r.bytes(&b,n)||b.size()!=n)return false;memcpy(reinterpret_cast<char*>(storage)+offsetof(VkPhysicalDeviceDescriptorIndexingProperties,maxUpdateAfterBindDescriptorsInAllPools),b.data(),n);break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:static_cast<VkPhysicalDeviceProtectedMemoryFeatures*>(storage)->protectedMemory=r.u32();break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:static_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(storage)->samplerYcbcrConversion=r.u32();break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:{auto&s=*static_cast<VkPhysicalDeviceVulkan11Properties*>(storage);std::vector<char>b;if(!r.bytes(&b,VK_UUID_SIZE)||b.size()!=VK_UUID_SIZE)return false;memcpy(s.deviceUUID,b.data(),VK_UUID_SIZE);if(!r.bytes(&b,VK_UUID_SIZE)||b.size()!=VK_UUID_SIZE)return false;memcpy(s.driverUUID,b.data(),VK_UUID_SIZE);if(!r.bytes(&b,VK_LUID_SIZE)||b.size()!=VK_LUID_SIZE)return false;memcpy(s.deviceLUID,b.data(),VK_LUID_SIZE);s.deviceNodeMask=r.u32();s.deviceLUIDValid=r.u32();s.subgroupSize=r.u32();s.subgroupSupportedStages=r.u32();s.subgroupSupportedOperations=r.u32();s.subgroupQuadOperationsInAllStages=r.u32();s.pointClippingBehavior=static_cast<VkPointClippingBehavior>(r.i32());s.maxMultiviewViewCount=r.u32();s.maxMultiviewInstanceIndex=r.u32();s.protectedNoFault=r.u32();s.maxPerSetDescriptors=r.u32();s.maxMemoryAllocationSize=r.u64();break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:{auto&s=*static_cast<VkPhysicalDeviceSubgroupProperties*>(storage);s.subgroupSize=r.u32();s.supportedStages=r.u32();s.supportedOperations=r.u32();s.quadOperationsInAllStages=r.u32();break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES:{auto&s=*static_cast<VkPhysicalDeviceIDProperties*>(storage);std::vector<char>b;if(!r.bytes(&b,VK_UUID_SIZE)||b.size()!=VK_UUID_SIZE)return false;memcpy(s.deviceUUID,b.data(),VK_UUID_SIZE);if(!r.bytes(&b,VK_UUID_SIZE)||b.size()!=VK_UUID_SIZE)return false;memcpy(s.driverUUID,b.data(),VK_UUID_SIZE);if(!r.bytes(&b,VK_LUID_SIZE)||b.size()!=VK_LUID_SIZE)return false;memcpy(s.deviceLUID,b.data(),VK_LUID_SIZE);s.deviceNodeMask=r.u32();s.deviceLUIDValid=r.u32();break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:static_cast<VkPhysicalDeviceProtectedMemoryProperties*>(storage)->protectedNoFault=r.u32();break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES:{auto&s=*static_cast<VkPhysicalDeviceMaintenance3Properties*>(storage);s.maxPerSetDescriptors=r.u32();s.maxMemoryAllocationSize=r.u64();break;}
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR: {
            auto& s=*static_cast<VkPhysicalDeviceFragmentShadingRateFeaturesKHR*>(storage);
            s.pipelineFragmentShadingRate=r.u32(); s.primitiveFragmentShadingRate=r.u32(); s.attachmentFragmentShadingRate=r.u32(); break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR: {
            auto& s=*static_cast<VkPhysicalDeviceFragmentShadingRatePropertiesKHR*>(storage);
            if(!read_extent(r,&s.minFragmentShadingRateAttachmentTexelSize)||!read_extent(r,&s.maxFragmentShadingRateAttachmentTexelSize)) return false;
            s.maxFragmentShadingRateAttachmentTexelSizeAspectRatio=r.u32(); s.primitiveFragmentShadingRateWithMultipleViewports=r.u32();
            s.layeredShadingRateAttachments=r.u32(); s.fragmentShadingRateNonTrivialCombinerOps=r.u32();
            if(!read_extent(r,&s.maxFragmentSize)) return false; s.maxFragmentSizeAspectRatio=r.u32(); s.maxFragmentShadingRateCoverageSamples=r.u32();
            s.maxFragmentShadingRateRasterizationSamples=static_cast<VkSampleCountFlagBits>(r.u32());
            s.fragmentShadingRateWithShaderDepthStencilWrites=r.u32(); s.fragmentShadingRateWithSampleMask=r.u32(); s.fragmentShadingRateWithShaderSampleMask=r.u32();
            s.fragmentShadingRateWithConservativeRasterization=r.u32(); s.fragmentShadingRateWithFragmentShaderInterlock=r.u32();
            s.fragmentShadingRateWithCustomSampleLocations=r.u32(); s.fragmentShadingRateStrictMultiplyCombiner=r.u32(); break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
            static_cast<VkPhysicalDeviceImagelessFramebufferFeatures*>(storage)->imagelessFramebuffer=r.u32(); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
            auto& s=*static_cast<VkPhysicalDeviceMultiviewFeatures*>(storage); s.multiview=r.u32(); s.multiviewGeometryShader=r.u32(); s.multiviewTessellationShader=r.u32(); break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES: {
            auto& s=*static_cast<VkPhysicalDeviceMultiviewProperties*>(storage); s.maxMultiviewViewCount=r.u32(); s.maxMultiviewInstanceIndex=r.u32(); break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES:
            static_cast<VkPhysicalDevicePointClippingProperties*>(storage)->pointClippingBehavior=static_cast<VkPointClippingBehavior>(r.i32()); break;
        case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO: {
            auto& s=*static_cast<VkRenderPassInputAttachmentAspectCreateInfo*>(storage); s.aspectReferenceCount=r.u32();
            if(!r.ok()||!valid_count(s.aspectReferenceCount,sizeof(VkInputAttachmentAspectReference))) return false;
            auto* p=s.aspectReferenceCount?arena.array<VkInputAttachmentAspectReference>(s.aspectReferenceCount):nullptr;
            for(uint32_t i=0;i<s.aspectReferenceCount;++i){p[i].subpass=r.u32();p[i].inputAttachmentIndex=r.u32();p[i].aspectMask=r.u32();} s.pAspectReferences=p; break;
        }
        case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO: {
            auto& s=*static_cast<VkRenderPassMultiviewCreateInfo*>(storage); s.subpassCount=r.u32();
            if(!r.ok()||!valid_count(s.subpassCount,sizeof(uint32_t))) return false; auto* vm=s.subpassCount?arena.array<uint32_t>(s.subpassCount):nullptr; for(uint32_t i=0;i<s.subpassCount;++i)vm[i]=r.u32();s.pViewMasks=vm;
            s.dependencyCount=r.u32(); if(!r.ok()||!valid_count(s.dependencyCount,sizeof(int32_t)))return false;auto* vo=s.dependencyCount?arena.array<int32_t>(s.dependencyCount):nullptr;for(uint32_t i=0;i<s.dependencyCount;++i)vo[i]=r.i32();s.pViewOffsets=vo;
            s.correlationMaskCount=r.u32();if(!r.ok()||!valid_count(s.correlationMaskCount,sizeof(uint32_t)))return false;auto* cm=s.correlationMaskCount?arena.array<uint32_t>(s.correlationMaskCount):nullptr;for(uint32_t i=0;i<s.correlationMaskCount;++i)cm[i]=r.u32();s.pCorrelationMasks=cm;break;
        }
        case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR: {
            if(depth>=kMaxPNextDepth)return false;auto& s=*static_cast<VkFragmentShadingRateAttachmentInfoKHR*>(storage);uint32_t present=r.u32();if(!r.ok()||present>1)return false;
            if(present){auto* a=arena.array<VkAttachmentReference2>(1);if(!read_attachment_reference2(r,a))return false;s.pFragmentShadingRateAttachment=a;}if(!read_extent(r,&s.shadingRateAttachmentTexelSize))return false;break;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR: {
            auto& s=*static_cast<VkPipelineFragmentShadingRateStateCreateInfoKHR*>(storage);if(!read_extent(r,&s.fragmentSize))return false;s.combinerOps[0]=static_cast<VkFragmentShadingRateCombinerOpKHR>(r.i32());s.combinerOps[1]=static_cast<VkFragmentShadingRateCombinerOpKHR>(r.i32());break;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
            static_cast<VkPipelineTessellationDomainOriginStateCreateInfo*>(storage)->domainOrigin=static_cast<VkTessellationDomainOrigin>(r.i32());break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            auto& s=*static_cast<VkImageFormatListCreateInfo*>(storage);s.viewFormatCount=r.u32();if(!r.ok()||!valid_count(s.viewFormatCount,sizeof(VkFormat)))return false;auto* p=s.viewFormatCount?arena.array<VkFormat>(s.viewFormatCount):nullptr;for(uint32_t i=0;i<s.viewFormatCount;++i)p[i]=static_cast<VkFormat>(r.i32());s.pViewFormats=p;break;
        }
        case VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO:
            static_cast<VkImageViewUsageCreateInfo*>(storage)->usage=r.u32();break;
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES:
            static_cast<VkSamplerYcbcrConversionImageFormatProperties*>(storage)->combinedImageSamplerDescriptorCount=r.u32();break;
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO: {
            const uint64_t id=r.handle();
            static_cast<VkSamplerYcbcrConversionInfo*>(storage)->conversion=
                id ? hr.sampler_ycbcr_conversion(id) : VK_NULL_HANDLE;
            break;
        }
        case VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO:
            static_cast<VkImagePlaneMemoryRequirementsInfo*>(storage)->planeAspect=static_cast<VkImageAspectFlagBits>(r.u32());break;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
            auto& s=*static_cast<VkMemoryDedicatedRequirements*>(storage);
            s.prefersDedicatedAllocation=r.u32();s.requiresDedicatedAllocation=r.u32();break;
        }
        case VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO:
            static_cast<VkBindImagePlaneMemoryInfo*>(storage)->planeAspect=static_cast<VkImageAspectFlagBits>(r.u32());break;
        case VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO: {
            if(depth>=kMaxPNextDepth)return false;auto& s=*static_cast<VkFramebufferAttachmentsCreateInfo*>(storage);s.attachmentImageInfoCount=r.u32();if(!r.ok()||!valid_count(s.attachmentImageInfoCount,sizeof(VkFramebufferAttachmentImageInfo)))return false;
            auto* p=s.attachmentImageInfoCount?arena.array<VkFramebufferAttachmentImageInfo>(s.attachmentImageInfoCount):nullptr;
            for(uint32_t i=0;i<s.attachmentImageInfoCount;++i){auto& a=p[i];a={};a.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO;a.flags=r.u32();a.usage=r.u32();a.width=r.u32();a.height=r.u32();a.layerCount=r.u32();a.viewFormatCount=r.u32();if(!r.ok()||!valid_count(a.viewFormatCount,sizeof(VkFormat)))return false;auto* f=a.viewFormatCount?arena.array<VkFormat>(a.viewFormatCount):nullptr;for(uint32_t j=0;j<a.viewFormatCount;++j)f[j]=static_cast<VkFormat>(r.i32());a.pViewFormats=f;}s.pAttachmentImageInfos=p;break;
        }

        case VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO:{auto&x=*static_cast<VkSemaphoreTypeCreateInfo*>(storage);x.semaphoreType=static_cast<VkSemaphoreType>(r.i32());x.initialValue=r.u64();break;}
        case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:{auto&x=*static_cast<VkTimelineSemaphoreSubmitInfo*>(storage);x.waitSemaphoreValueCount=r.u32();if(!r.ok()||!valid_count(x.waitSemaphoreValueCount,sizeof(uint64_t)))return false;auto*w=x.waitSemaphoreValueCount?arena.array<uint64_t>(x.waitSemaphoreValueCount):nullptr;for(uint32_t i=0;i<x.waitSemaphoreValueCount;++i)w[i]=r.u64();x.pWaitSemaphoreValues=w;x.signalSemaphoreValueCount=r.u32();if(!r.ok()||!valid_count(x.signalSemaphoreValueCount,sizeof(uint64_t)))return false;auto*v=x.signalSemaphoreValueCount?arena.array<uint64_t>(x.signalSemaphoreValueCount):nullptr;for(uint32_t i=0;i<x.signalSemaphoreValueCount;++i)v[i]=r.u64();x.pSignalSemaphoreValues=v;break;}
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO:static_cast<VkBufferOpaqueCaptureAddressCreateInfo*>(storage)->opaqueCaptureAddress=r.u64();break;
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO:{auto&x=*static_cast<VkMemoryAllocateFlagsInfo*>(storage);x.flags=r.u32();x.deviceMask=r.u32();if((x.flags&~VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)!=0||x.deviceMask!=0)return false;break;}
        case VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO: {
            auto& s=*static_cast<VkRenderPassAttachmentBeginInfo*>(storage);s.attachmentCount=r.u32();if(!r.ok()||!valid_count(s.attachmentCount,sizeof(VkImageView)))return false;VkImageView* p=nullptr;if(!read_handle_array(r,arena,s.attachmentCount,[&](uint64_t id){return hr.image_view(id);},&p))return false;s.pAttachments=p;break;
        }
        default:return false;
    }
    return r.ok();
}

}  // namespace

bool write_pnext_chain(Writer& w, PNextContext context, const void* head, uint32_t depth) {
    if (depth > kMaxPNextDepth) return false;
    std::vector<const VkBaseInStructure*> nodes;
    if (!collect_chain(context, head, &nodes)) return false;

    // Encode transactionally. A rejected nested count or node must not leave a
    // syntactically valid prefix in the caller's request.
    Writer encoded;
    encoded.u32(static_cast<uint32_t>(nodes.size()));
    for (const auto* node : nodes) {
        encoded.i32(static_cast<int32_t>(node->sType));
        if (!write_node_fields(encoded, context, node, depth)) return false;
    }
    w.append(encoded);
    return true;
}

bool read_pnext_chain(Reader& r, Arena& arena, const HandleResolver& hr,
                      PNextContext context, const void** out_head, uint32_t depth) {
    if (!out_head || depth > kMaxPNextDepth) return false;
    *out_head = nullptr;
    const uint32_t count = r.u32();
    if (!r.ok() || count > kMaxPNextNodes) return false;
    VkBaseOutStructure* first = nullptr;
    VkBaseOutStructure* previous = nullptr;
    VkStructureType seen[kMaxPNextNodes]{};
    for (uint32_t i = 0; i < count; ++i) {
        const auto type = static_cast<VkStructureType>(r.i32());
        if (!r.ok() || !pnext_allowed(context, type)) return false;
        for (uint32_t j = 0; j < i; ++j) if (seen[j] == type) return false;
        seen[i] = type;
        const size_t size = pnext_node_size(type);
        if (!size) return false;
        void* storage = arena.allocate(size);
        if (!storage) return false;
        std::memset(storage, 0, size);
        auto* node = static_cast<VkBaseOutStructure*>(storage);
        node->sType = type;
        if (!read_node_fields(r, arena, hr, type, storage, depth)) return false;
        if (previous) previous->pNext = node; else first = node;
        previous = node;
    }
    *out_head = first;
    return r.ok();
}

bool write_pnext_output_topology(Writer& w, PNextContext context, const void* head) {
    std::vector<const VkBaseInStructure*> nodes;
    if (!collect_chain(context, head, &nodes)) return false;
    w.u32(static_cast<uint32_t>(nodes.size()));
    for (const auto* node : nodes) w.i32(static_cast<int32_t>(node->sType));
    return true;
}

bool read_pnext_output_topology(Reader& r, Arena& arena, PNextContext context, void** out_head) {
    if (!out_head) return false;
    *out_head = nullptr;
    const uint32_t count = r.u32();
    if (!r.ok() || count > kMaxPNextNodes) return false;
    VkBaseOutStructure* first = nullptr;
    VkBaseOutStructure* previous = nullptr;
    VkStructureType seen[kMaxPNextNodes]{};
    for (uint32_t i=0;i<count;++i) {
        auto type=static_cast<VkStructureType>(r.i32());
        if(!r.ok()||!pnext_allowed(context,type))return false;
        for(uint32_t j=0;j<i;++j)if(seen[j]==type)return false;seen[i]=type;
        size_t size=pnext_node_size(type);if(!size)return false;void* storage=arena.allocate(size);if(!storage)return false;std::memset(storage,0,size);
        auto* node=static_cast<VkBaseOutStructure*>(storage);node->sType=type;if(previous)previous->pNext=node;else first=node;previous=node;
    }
    *out_head=first;return r.ok();
}

bool write_pnext_output_values(Writer& w, PNextContext context, const void* head) {
    return write_pnext_chain(w, context, head);
}

bool apply_pnext_output_values(Reader& r, PNextContext context, void* caller_head) {
    std::vector<const VkBaseInStructure*> caller_nodes;
    if (!collect_chain(context, caller_head, &caller_nodes)) return false;
    const uint32_t count=r.u32();if(!r.ok()||count!=caller_nodes.size()||count>kMaxPNextNodes)return false;
    // Decode into temporary fixed-size storage, then copy only semantic fields;
    // caller sType/pNext links are never modified.
    struct NullResolver final : HandleResolver {
#define NULL_HANDLE(name, type) type name(uint64_t) const override { return static_cast<type>(0); }
        NULL_HANDLE(buffer,VkBuffer) NULL_HANDLE(image,VkImage) NULL_HANDLE(image_view,VkImageView)
        NULL_HANDLE(buffer_view,VkBufferView) NULL_HANDLE(memory,VkDeviceMemory) NULL_HANDLE(render_pass,VkRenderPass) NULL_HANDLE(framebuffer,VkFramebuffer)
        NULL_HANDLE(shader_module,VkShaderModule) NULL_HANDLE(pipeline,VkPipeline) NULL_HANDLE(pipeline_layout,VkPipelineLayout)
        NULL_HANDLE(pipeline_cache,VkPipelineCache) NULL_HANDLE(descriptor_set_layout,VkDescriptorSetLayout)
        NULL_HANDLE(descriptor_pool,VkDescriptorPool) NULL_HANDLE(descriptor_set,VkDescriptorSet) NULL_HANDLE(sampler,VkSampler)
        NULL_HANDLE(sampler_ycbcr_conversion,VkSamplerYcbcrConversion)
        NULL_HANDLE(semaphore,VkSemaphore) NULL_HANDLE(fence,VkFence) NULL_HANDLE(event,VkEvent) NULL_HANDLE(query_pool,VkQueryPool) NULL_HANDLE(surface,VkSurfaceKHR)
        NULL_HANDLE(swapchain,VkSwapchainKHR) NULL_HANDLE(command_pool,VkCommandPool) NULL_HANDLE(command_buffer,VkCommandBuffer)
#undef NULL_HANDLE
    } null_resolver;
    Arena arena;
    for(uint32_t i=0;i<count;++i){auto type=static_cast<VkStructureType>(r.i32());if(!r.ok()||type!=caller_nodes[i]->sType)return false;size_t size=pnext_node_size(type);void* temp=arena.allocate(size);if(!temp)return false;std::memset(temp,0,size);static_cast<VkBaseOutStructure*>(temp)->sType=type;if(!read_node_fields(r,arena,null_resolver,type,temp,0))return false;
        void* next=const_cast<void*>(reinterpret_cast<const void*>(caller_nodes[i]->pNext));
        switch(type){
#define COPY_OUTPUT(T) do { auto* dst=reinterpret_cast<T*>(const_cast<VkBaseInStructure*>(caller_nodes[i])); auto* src=static_cast<T*>(temp); void* saved=const_cast<void*>(dst->pNext); *dst=*src; dst->pNext=saved; } while(0)
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:COPY_OUTPUT(VkPhysicalDeviceFragmentShadingRateFeaturesKHR);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:COPY_OUTPUT(VkPhysicalDeviceVulkan11Features);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:COPY_OUTPUT(VkPhysicalDeviceVulkan12Features);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:COPY_OUTPUT(VkPhysicalDeviceHostQueryResetFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:COPY_OUTPUT(VkPhysicalDeviceTimelineSemaphoreFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:COPY_OUTPUT(VkPhysicalDeviceSynchronization2Features);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:COPY_OUTPUT(VkPhysicalDeviceBufferDeviceAddressFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:COPY_OUTPUT(VkPhysicalDeviceDescriptorIndexingFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:COPY_OUTPUT(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:COPY_OUTPUT(VkPhysicalDeviceVulkan11Properties);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:COPY_OUTPUT(VkPhysicalDeviceVulkan12Properties);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:COPY_OUTPUT(VkPhysicalDeviceTimelineSemaphoreProperties);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES:COPY_OUTPUT(VkPhysicalDeviceDepthStencilResolveProperties);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:COPY_OUTPUT(VkPhysicalDeviceDescriptorIndexingProperties);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:COPY_OUTPUT(VkPhysicalDeviceProtectedMemoryFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:COPY_OUTPUT(VkPhysicalDeviceSamplerYcbcrConversionFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:COPY_OUTPUT(VkPhysicalDeviceSubgroupProperties);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES:COPY_OUTPUT(VkPhysicalDeviceIDProperties);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:COPY_OUTPUT(VkPhysicalDeviceProtectedMemoryProperties);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES:COPY_OUTPUT(VkPhysicalDeviceMaintenance3Properties);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR:COPY_OUTPUT(VkPhysicalDeviceFragmentShadingRatePropertiesKHR);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:COPY_OUTPUT(VkPhysicalDeviceImagelessFramebufferFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:COPY_OUTPUT(VkPhysicalDeviceMultiviewFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES:COPY_OUTPUT(VkPhysicalDeviceMultiviewProperties);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES:COPY_OUTPUT(VkPhysicalDevicePointClippingProperties);break;
            case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS:COPY_OUTPUT(VkMemoryDedicatedRequirements);break;
            case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES:COPY_OUTPUT(VkSamplerYcbcrConversionImageFormatProperties);break;
            default:(void)next;return false;
#undef COPY_OUTPUT
        }
    }
    return r.ok();
}

// ---------------------------------------------------------------------------
// Group A
// ---------------------------------------------------------------------------

void write_DeviceQueueCreateInfo(Writer& w, const VkDeviceQueueCreateInfo& s) {
    VkDeviceQueueCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pQueuePriorities = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_pod_array(w, s.pQueuePriorities, s.queueCount);
}

bool read_DeviceQueueCreateInfo(Reader& r, Arena& arena, const HandleResolver&,
                                 VkDeviceQueueCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    return read_pod_array(r, arena, out->queueCount, const_cast<float**>(&out->pQueuePriorities));
}

bool write_DeviceCreateInfo(Writer& w, const VkDeviceCreateInfo& s) {
    Writer encoded;
    VkDeviceCreateInfo tmp = s;
    // Device feature nodes are encoded separately through the bounded
    // DeviceCreate pNext allowlist; never serialize caller pointers.
    tmp.pNext = nullptr;
    tmp.pQueueCreateInfos = nullptr;
    tmp.ppEnabledLayerNames = nullptr;
    tmp.ppEnabledExtensionNames = nullptr;
    tmp.pEnabledFeatures = nullptr;
    encoded.bytes(&tmp, sizeof(tmp));
    if (!write_pnext_chain(encoded, PNextContext::DeviceCreate, s.pNext)) return false;
    for (uint32_t i = 0; i < s.queueCreateInfoCount; ++i)
        write_DeviceQueueCreateInfo(encoded, s.pQueueCreateInfos[i]);
    write_string_array(encoded, s.ppEnabledLayerNames, s.enabledLayerCount);
    write_string_array(encoded, s.ppEnabledExtensionNames, s.enabledExtensionCount);
    write_opt_pod(encoded, s.pEnabledFeatures);
    w.append(encoded);
    return true;
}

bool read_DeviceCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                            VkDeviceCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    if (!read_pnext_chain(r, arena, hr, PNextContext::DeviceCreate, &out->pNext)) return false;
    if (out->queueCreateInfoCount > kMaxArrayElements) return false;
    VkDeviceQueueCreateInfo* queues =
        out->queueCreateInfoCount ? arena.array<VkDeviceQueueCreateInfo>(out->queueCreateInfoCount)
                                   : nullptr;
    for (uint32_t i = 0; i < out->queueCreateInfoCount; ++i)
        if (!read_DeviceQueueCreateInfo(r, arena, hr, &queues[i])) return false;
    out->pQueueCreateInfos = queues;

    if (!read_string_array(r, arena, out->enabledLayerCount,
                            const_cast<const char***>(&out->ppEnabledLayerNames)))
        return false;
    if (!read_string_array(r, arena, out->enabledExtensionCount,
                            const_cast<const char***>(&out->ppEnabledExtensionNames)))
        return false;
    return read_opt_pod(r, arena, const_cast<VkPhysicalDeviceFeatures**>(&out->pEnabledFeatures));
}

void write_SwapchainCreateInfoKHR(Writer& w, const VkSwapchainCreateInfoKHR& s) {
    VkSwapchainCreateInfoKHR tmp = s;
    tmp.pNext = nullptr;
    tmp.pQueueFamilyIndices = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_pod_array(w, s.pQueueFamilyIndices, s.queueFamilyIndexCount);
}

bool read_SwapchainCreateInfoKHR(Reader& r, Arena& arena, const HandleResolver& hr,
                                  VkSwapchainCreateInfoKHR* out) {
    if (!read_raw(r, out)) return false;
    if (!read_pod_array(r, arena, out->queueFamilyIndexCount,
                         const_cast<uint32_t**>(&out->pQueueFamilyIndices)))
        return false;
    patch_handle(&out->surface, [&](uint64_t id) { return hr.surface(id); });
    patch_handle(&out->oldSwapchain, [&](uint64_t id) { return hr.swapchain(id); });
    return true;
}

bool write_ImageViewCreateInfo(Writer& w, const VkImageViewCreateInfo& s) {
    Writer e; VkImageViewCreateInfo tmp=s; tmp.pNext=nullptr; e.bytes(&tmp,sizeof(tmp));
    if(!write_pnext_chain(e,PNextContext::ImageViewCreate,s.pNext))return false;w.append(e);return true;
}

bool read_ImageViewCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkImageViewCreateInfo* out) {
    if (!read_raw(r, out) || !read_pnext_chain(r,arena,hr,PNextContext::ImageViewCreate,&out->pNext)) return false;
    patch_handle(&out->image, [&](uint64_t id) { return hr.image(id); });
    return true;
}

bool write_RenderPassCreateInfo(Writer& w, const VkRenderPassCreateInfo& s) {
    Writer e; VkRenderPassCreateInfo tmp = s;
    tmp.pNext = nullptr; tmp.pAttachments = nullptr; tmp.pSubpasses = nullptr; tmp.pDependencies = nullptr;
    e.bytes(&tmp, sizeof(tmp));
    if(!write_pnext_chain(e,PNextContext::RenderPass,s.pNext))return false;
    write_pod_array(e, s.pAttachments, s.attachmentCount);
    for (uint32_t i = 0; i < s.subpassCount; ++i) write_SubpassDescription(e, s.pSubpasses[i]);
    write_pod_array(e, s.pDependencies, s.dependencyCount);w.append(e);return true;
}

bool read_RenderPassCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                VkRenderPassCreateInfo* out) {
    if (!read_raw(r, out) || !read_pnext_chain(r,arena,hr,PNextContext::RenderPass,&out->pNext)) return false;
    if (!read_pod_array(r, arena, out->attachmentCount,
                         const_cast<VkAttachmentDescription**>(&out->pAttachments)))
        return false;

    if (out->subpassCount > kMaxArrayElements) return false;
    VkSubpassDescription* subpasses =
        out->subpassCount ? arena.array<VkSubpassDescription>(out->subpassCount) : nullptr;
    for (uint32_t i = 0; i < out->subpassCount; ++i)
        if (!read_SubpassDescription(r, arena, &subpasses[i])) return false;
    out->pSubpasses = subpasses;

    return read_pod_array(r, arena, out->dependencyCount,
                           const_cast<VkSubpassDependency**>(&out->pDependencies));
}

namespace {

bool write_attachment_reference2_full(Writer& w, const VkAttachmentReference2& s) {
    if (s.sType != VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 || s.pNext) return false;
    write_attachment_reference2(w, s);
    return true;
}

bool write_subpass_description2(Writer& w, const VkSubpassDescription2& s) {
    if (s.sType != VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2 ||
        s.inputAttachmentCount > kMaxArrayElements ||
        s.colorAttachmentCount > kMaxArrayElements ||
        s.preserveAttachmentCount > kMaxArrayElements ||
        (s.inputAttachmentCount && !s.pInputAttachments) ||
        (s.colorAttachmentCount && !s.pColorAttachments) ||
        (s.preserveAttachmentCount && !s.pPreserveAttachments)) return false;
    Writer encoded;
    encoded.i32(static_cast<int32_t>(s.sType)); encoded.u32(s.flags);
    encoded.i32(static_cast<int32_t>(s.pipelineBindPoint)); encoded.u32(s.viewMask);
    if (!write_pnext_chain(encoded, PNextContext::RenderPass2, s.pNext)) return false;
    encoded.u32(s.inputAttachmentCount);
    for (uint32_t i=0;i<s.inputAttachmentCount;++i)
        if (!write_attachment_reference2_full(encoded,s.pInputAttachments[i])) return false;
    encoded.u32(s.colorAttachmentCount);
    for (uint32_t i=0;i<s.colorAttachmentCount;++i)
        if (!write_attachment_reference2_full(encoded,s.pColorAttachments[i])) return false;
    encoded.u32(s.pResolveAttachments ? 1u : 0u);
    if (s.pResolveAttachments)
        for (uint32_t i=0;i<s.colorAttachmentCount;++i)
            if (!write_attachment_reference2_full(encoded,s.pResolveAttachments[i])) return false;
    encoded.u32(s.pDepthStencilAttachment ? 1u : 0u);
    if (s.pDepthStencilAttachment &&
        !write_attachment_reference2_full(encoded,*s.pDepthStencilAttachment)) return false;
    encoded.u32(s.preserveAttachmentCount);
    for(uint32_t i=0;i<s.preserveAttachmentCount;++i) encoded.u32(s.pPreserveAttachments[i]);
    w.append(encoded); return true;
}

bool read_subpass_description2(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkSubpassDescription2* s) {
    *s={}; s->sType=static_cast<VkStructureType>(r.i32()); s->flags=r.u32();
    s->pipelineBindPoint=static_cast<VkPipelineBindPoint>(r.i32()); s->viewMask=r.u32();
    if(!r.ok()||s->sType!=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2||
       !read_pnext_chain(r,arena,hr,PNextContext::RenderPass2,&s->pNext)) return false;
    s->inputAttachmentCount=r.u32();
    if(!r.ok()||s->inputAttachmentCount>kMaxArrayElements||s->inputAttachmentCount>r.remaining()/16)return false;
    auto* in=s->inputAttachmentCount?arena.array<VkAttachmentReference2>(s->inputAttachmentCount):nullptr;
    for(uint32_t i=0;i<s->inputAttachmentCount;++i)if(!read_attachment_reference2(r,&in[i]))return false;s->pInputAttachments=in;
    s->colorAttachmentCount=r.u32();
    if(!r.ok()||s->colorAttachmentCount>kMaxArrayElements||s->colorAttachmentCount>r.remaining()/16)return false;
    auto* color=s->colorAttachmentCount?arena.array<VkAttachmentReference2>(s->colorAttachmentCount):nullptr;
    for(uint32_t i=0;i<s->colorAttachmentCount;++i)if(!read_attachment_reference2(r,&color[i]))return false;s->pColorAttachments=color;
    uint32_t present=r.u32();if(!r.ok()||present>1)return false;
    if(present){auto* resolve=s->colorAttachmentCount?arena.array<VkAttachmentReference2>(s->colorAttachmentCount):nullptr;for(uint32_t i=0;i<s->colorAttachmentCount;++i)if(!read_attachment_reference2(r,&resolve[i]))return false;s->pResolveAttachments=resolve;}
    present=r.u32();if(!r.ok()||present>1)return false;
    if(present){auto* depth=arena.array<VkAttachmentReference2>(1);if(!read_attachment_reference2(r,depth))return false;s->pDepthStencilAttachment=depth;}
    s->preserveAttachmentCount=r.u32();if(!r.ok()||s->preserveAttachmentCount>kMaxArrayElements||s->preserveAttachmentCount>r.remaining()/4)return false;
    auto* preserve=s->preserveAttachmentCount?arena.array<uint32_t>(s->preserveAttachmentCount):nullptr;for(uint32_t i=0;i<s->preserveAttachmentCount;++i)preserve[i]=r.u32();s->pPreserveAttachments=preserve;
    return r.ok();
}

} // namespace

bool write_RenderPassCreateInfo2(Writer& w, const VkRenderPassCreateInfo2& s) {
    if(s.sType!=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2||s.attachmentCount>kMaxArrayElements||
       s.subpassCount>kMaxArrayElements||s.dependencyCount>kMaxArrayElements||
       s.correlatedViewMaskCount>kMaxArrayElements||(s.attachmentCount&&!s.pAttachments)||
       (s.subpassCount&&!s.pSubpasses)||(s.dependencyCount&&!s.pDependencies)||
       (s.correlatedViewMaskCount&&!s.pCorrelatedViewMasks))return false;
    Writer e;e.i32(static_cast<int32_t>(s.sType));e.u32(s.flags);
    if(!write_pnext_chain(e,PNextContext::RenderPass,s.pNext))return false;
    e.u32(s.attachmentCount);
    for(uint32_t i=0;i<s.attachmentCount;++i){const auto&a=s.pAttachments[i];if(a.sType!=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2||a.pNext)return false;e.i32(static_cast<int32_t>(a.sType));e.u32(a.flags);e.i32(a.format);e.i32(a.samples);e.i32(a.loadOp);e.i32(a.storeOp);e.i32(a.stencilLoadOp);e.i32(a.stencilStoreOp);e.i32(a.initialLayout);e.i32(a.finalLayout);}
    e.u32(s.subpassCount);for(uint32_t i=0;i<s.subpassCount;++i)if(!write_subpass_description2(e,s.pSubpasses[i]))return false;
    e.u32(s.dependencyCount);
    for(uint32_t i=0;i<s.dependencyCount;++i){const auto&d=s.pDependencies[i];if(d.sType!=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2||d.pNext)return false;e.i32(d.sType);e.u32(d.srcSubpass);e.u32(d.dstSubpass);e.u32(d.srcStageMask);e.u32(d.dstStageMask);e.u32(d.srcAccessMask);e.u32(d.dstAccessMask);e.u32(d.dependencyFlags);e.i32(d.viewOffset);}
    e.u32(s.correlatedViewMaskCount);for(uint32_t i=0;i<s.correlatedViewMaskCount;++i)e.u32(s.pCorrelatedViewMasks[i]);w.append(e);return true;
}

bool read_RenderPassCreateInfo2(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkRenderPassCreateInfo2* s) {
    *s={};s->sType=static_cast<VkStructureType>(r.i32());s->flags=r.u32();if(!r.ok()||s->sType!=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2||!read_pnext_chain(r,arena,hr,PNextContext::RenderPass,&s->pNext))return false;
    s->attachmentCount=r.u32();if(!r.ok()||s->attachmentCount>kMaxArrayElements||s->attachmentCount>r.remaining()/40)return false;auto*a=s->attachmentCount?arena.array<VkAttachmentDescription2>(s->attachmentCount):nullptr;
    for(uint32_t i=0;i<s->attachmentCount;++i){a[i]={};a[i].sType=static_cast<VkStructureType>(r.i32());a[i].flags=r.u32();a[i].format=static_cast<VkFormat>(r.i32());a[i].samples=static_cast<VkSampleCountFlagBits>(r.i32());a[i].loadOp=static_cast<VkAttachmentLoadOp>(r.i32());a[i].storeOp=static_cast<VkAttachmentStoreOp>(r.i32());a[i].stencilLoadOp=static_cast<VkAttachmentLoadOp>(r.i32());a[i].stencilStoreOp=static_cast<VkAttachmentStoreOp>(r.i32());a[i].initialLayout=static_cast<VkImageLayout>(r.i32());a[i].finalLayout=static_cast<VkImageLayout>(r.i32());if(!r.ok()||a[i].sType!=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2)return false;}s->pAttachments=a;
    s->subpassCount=r.u32();if(!r.ok()||s->subpassCount>kMaxArrayElements)return false;auto*sp=s->subpassCount?arena.array<VkSubpassDescription2>(s->subpassCount):nullptr;for(uint32_t i=0;i<s->subpassCount;++i)if(!read_subpass_description2(r,arena,hr,&sp[i]))return false;s->pSubpasses=sp;
    s->dependencyCount=r.u32();if(!r.ok()||s->dependencyCount>kMaxArrayElements||s->dependencyCount>r.remaining()/36)return false;auto*d=s->dependencyCount?arena.array<VkSubpassDependency2>(s->dependencyCount):nullptr;
    for(uint32_t i=0;i<s->dependencyCount;++i){d[i]={};d[i].sType=static_cast<VkStructureType>(r.i32());d[i].srcSubpass=r.u32();d[i].dstSubpass=r.u32();d[i].srcStageMask=r.u32();d[i].dstStageMask=r.u32();d[i].srcAccessMask=r.u32();d[i].dstAccessMask=r.u32();d[i].dependencyFlags=r.u32();d[i].viewOffset=r.i32();if(!r.ok()||d[i].sType!=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2)return false;}s->pDependencies=d;
    s->correlatedViewMaskCount=r.u32();if(!r.ok()||s->correlatedViewMaskCount>kMaxArrayElements||s->correlatedViewMaskCount>r.remaining()/4)return false;auto*m=s->correlatedViewMaskCount?arena.array<uint32_t>(s->correlatedViewMaskCount):nullptr;for(uint32_t i=0;i<s->correlatedViewMaskCount;++i)m[i]=r.u32();s->pCorrelatedViewMasks=m;return r.ok();
}

bool write_ShaderModuleCreateInfo(Writer& w, const VkShaderModuleCreateInfo& s) {
    if (s.sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO || s.codeSize == 0 ||
        (s.codeSize % sizeof(uint32_t)) != 0 || s.codeSize > kMaxShaderCodeBytes ||
        s.pCode == nullptr ||
        (reinterpret_cast<uintptr_t>(s.pCode) % alignof(uint32_t)) != 0) {
        return false;
    }

    Writer encoded;
    encoded.i32(static_cast<int32_t>(s.sType));
    encoded.u32(s.flags);
    if (!write_pnext_chain(encoded, PNextContext::ShaderModule, s.pNext)) return false;
    encoded.u64(static_cast<uint64_t>(s.codeSize));
    for (size_t i = 0; i < s.codeSize / sizeof(uint32_t); ++i) encoded.u32(s.pCode[i]);
    w.append(encoded);
    return true;
}

bool read_ShaderModuleCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkShaderModuleCreateInfo* out) {
    *out = {};
    out->sType = static_cast<VkStructureType>(r.i32());
    out->flags = r.u32();
    if (!r.ok() || out->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        !read_pnext_chain(r, arena, hr, PNextContext::ShaderModule, &out->pNext)) {
        return false;
    }

    const uint64_t code_size = r.u64();
    if (!r.ok() || code_size == 0 || (code_size % sizeof(uint32_t)) != 0 ||
        code_size > kMaxShaderCodeBytes || code_size != r.remaining()) {
        return false;
    }

    const size_t word_count = static_cast<size_t>(code_size / sizeof(uint32_t));
    auto* code = arena.array<uint32_t>(word_count);
    for (size_t i = 0; i < word_count; ++i) code[i] = r.u32();
    if (!r.ok() || r.remaining() != 0) return false;
    out->codeSize = static_cast<size_t>(code_size);
    out->pCode = code;
    return true;
}

void write_PipelineLayoutCreateInfo(Writer& w, const VkPipelineLayoutCreateInfo& s) {
    VkPipelineLayoutCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pSetLayouts = nullptr;
    tmp.pPushConstantRanges = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_handle_array(w, s.pSetLayouts, s.setLayoutCount);
    write_pod_array(w, s.pPushConstantRanges, s.pushConstantRangeCount);
}

bool read_PipelineLayoutCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                    VkPipelineLayoutCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    if (!read_handle_array(
            r, arena, out->setLayoutCount,
            [&](uint64_t id) { return hr.descriptor_set_layout(id); },
            const_cast<VkDescriptorSetLayout**>(&out->pSetLayouts)))
        return false;
    return read_pod_array(r, arena, out->pushConstantRangeCount,
                           const_cast<VkPushConstantRange**>(&out->pPushConstantRanges));
}

bool write_GraphicsPipelineCreateInfo(Writer& w, const VkGraphicsPipelineCreateInfo& s) {
    if (!valid_count(s.stageCount, sizeof(VkPipelineShaderStageCreateInfo)) ||
        (s.stageCount != 0 && s.pStages == nullptr) ||
        (s.pVertexInputState &&
         ((!valid_count(s.pVertexInputState->vertexBindingDescriptionCount,
                        sizeof(VkVertexInputBindingDescription)) ||
           (s.pVertexInputState->vertexBindingDescriptionCount != 0 &&
            s.pVertexInputState->pVertexBindingDescriptions == nullptr)) ||
          (!valid_count(s.pVertexInputState->vertexAttributeDescriptionCount,
                        sizeof(VkVertexInputAttributeDescription)) ||
           (s.pVertexInputState->vertexAttributeDescriptionCount != 0 &&
            s.pVertexInputState->pVertexAttributeDescriptions == nullptr)))) ||
        (s.pViewportState &&
         (!valid_count(s.pViewportState->viewportCount, sizeof(VkViewport)) ||
          !valid_count(s.pViewportState->scissorCount, sizeof(VkRect2D)))) ||
        (s.pMultisampleState &&
         (s.pMultisampleState->rasterizationSamples < VK_SAMPLE_COUNT_1_BIT ||
          s.pMultisampleState->rasterizationSamples > VK_SAMPLE_COUNT_64_BIT ||
          (static_cast<uint32_t>(s.pMultisampleState->rasterizationSamples) &
           (static_cast<uint32_t>(s.pMultisampleState->rasterizationSamples) - 1)) != 0)) ||
        (s.pColorBlendState &&
         (!valid_count(s.pColorBlendState->attachmentCount,
                       sizeof(VkPipelineColorBlendAttachmentState)) ||
          (s.pColorBlendState->attachmentCount != 0 &&
           s.pColorBlendState->pAttachments == nullptr))) ||
        (s.pDynamicState &&
         (!valid_count(s.pDynamicState->dynamicStateCount, sizeof(VkDynamicState)) ||
          (s.pDynamicState->dynamicStateCount != 0 &&
           s.pDynamicState->pDynamicStates == nullptr)))) {
        return false;
    }
    Writer encoded;
    VkGraphicsPipelineCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pStages = nullptr;
    tmp.pVertexInputState = nullptr;
    tmp.pInputAssemblyState = nullptr;
    tmp.pTessellationState = nullptr;
    tmp.pViewportState = nullptr;
    tmp.pRasterizationState = nullptr;
    tmp.pMultisampleState = nullptr;
    tmp.pDepthStencilState = nullptr;
    tmp.pColorBlendState = nullptr;
    tmp.pDynamicState = nullptr;
    encoded.bytes(&tmp, sizeof(tmp));

    for (uint32_t i = 0; i < s.stageCount; ++i)
        write_PipelineShaderStageCreateInfo(encoded, s.pStages[i]);

    encoded.u32(s.pVertexInputState ? 1 : 0);
    if (s.pVertexInputState) {
        const auto& vi = *s.pVertexInputState;
        VkPipelineVertexInputStateCreateInfo t = vi;
        t.pNext = nullptr;
        t.pVertexBindingDescriptions = nullptr;
        t.pVertexAttributeDescriptions = nullptr;
        encoded.bytes(&t, sizeof(t));
        write_pod_array(encoded, vi.pVertexBindingDescriptions, vi.vertexBindingDescriptionCount);
        write_pod_array(encoded, vi.pVertexAttributeDescriptions, vi.vertexAttributeDescriptionCount);
    }

    write_opt_pod(encoded, s.pInputAssemblyState);
    write_opt_pod(encoded, s.pTessellationState);

    encoded.u32(s.pViewportState ? 1 : 0);
    if (s.pViewportState) {
        const auto& vp = *s.pViewportState;
        VkPipelineViewportStateCreateInfo t = vp;
        t.pNext = nullptr;
        t.pViewports = nullptr;
        t.pScissors = nullptr;
        encoded.bytes(&t, sizeof(t));
        write_opt_pod_array(encoded, vp.pViewports, vp.viewportCount);
        write_opt_pod_array(encoded, vp.pScissors, vp.scissorCount);
    }

    write_opt_pod(encoded, s.pRasterizationState);

    encoded.u32(s.pMultisampleState ? 1 : 0);
    if (s.pMultisampleState) {
        const auto& ms = *s.pMultisampleState;
        VkPipelineMultisampleStateCreateInfo t = ms;
        t.pNext = nullptr;
        t.pSampleMask = nullptr;
        encoded.bytes(&t, sizeof(t));
        const uint32_t mask_words = (static_cast<uint32_t>(ms.rasterizationSamples) + 31) / 32;
        write_opt_pod_array(encoded, ms.pSampleMask, mask_words);
    }

    write_opt_pod(encoded, s.pDepthStencilState);

    encoded.u32(s.pColorBlendState ? 1 : 0);
    if (s.pColorBlendState) {
        const auto& cb = *s.pColorBlendState;
        VkPipelineColorBlendStateCreateInfo t = cb;
        t.pNext = nullptr;
        t.pAttachments = nullptr;
        encoded.bytes(&t, sizeof(t));
        write_pod_array(encoded, cb.pAttachments, cb.attachmentCount);
    }

    encoded.u32(s.pDynamicState ? 1 : 0);
    if (s.pDynamicState) {
        const auto& ds = *s.pDynamicState;
        VkPipelineDynamicStateCreateInfo t = ds;
        t.pNext = nullptr;
        t.pDynamicStates = nullptr;
        encoded.bytes(&t, sizeof(t));
        write_pod_array(encoded, ds.pDynamicStates, ds.dynamicStateCount);
    }
    if (!write_pnext_chain(encoded, PNextContext::GraphicsPipeline, s.pNext)) return false;
    // Tessellation state has its own maintenance2 subchain.
    if (s.pTessellationState &&
        !write_pnext_chain(encoded, PNextContext::TessellationState,
                           s.pTessellationState->pNext)) return false;
    w.append(encoded);
    return true;
}

bool read_GraphicsPipelineCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                      VkGraphicsPipelineCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    const uint64_t layout_id = id_of(out->layout);
    const uint64_t render_pass_id = id_of(out->renderPass);
    const uint64_t base_pipeline_id = id_of(out->basePipelineHandle);

    if (!r.ok() || out->stageCount > kMaxArrayElements ||
        out->stageCount > r.remaining() / 12) return false;
    VkPipelineShaderStageCreateInfo* stages =
        out->stageCount ? arena.array<VkPipelineShaderStageCreateInfo>(out->stageCount) : nullptr;
    for (uint32_t i = 0; i < out->stageCount; ++i)
        if (!read_PipelineShaderStageCreateInfo(r, arena, hr, &stages[i])) return false;
    out->pStages = stages;

    uint32_t present = r.u32();
    if (!r.ok()) return false;
    if (present) {
        auto* vi = arena.array<VkPipelineVertexInputStateCreateInfo>(1);
        if (!read_raw(r, vi)) return false;
        if (!read_pod_array(r, arena, vi->vertexBindingDescriptionCount,
                             const_cast<VkVertexInputBindingDescription**>(
                                 &vi->pVertexBindingDescriptions)))
            return false;
        if (!read_pod_array(r, arena, vi->vertexAttributeDescriptionCount,
                             const_cast<VkVertexInputAttributeDescription**>(
                                 &vi->pVertexAttributeDescriptions)))
            return false;
        out->pVertexInputState = vi;
    } else {
        out->pVertexInputState = nullptr;
    }

    if (!read_opt_pod(r, arena,
                       const_cast<VkPipelineInputAssemblyStateCreateInfo**>(
                           &out->pInputAssemblyState)))
        return false;
    if (!read_opt_pod(
            r, arena,
            const_cast<VkPipelineTessellationStateCreateInfo**>(&out->pTessellationState)))
        return false;

    present = r.u32();
    if (!r.ok()) return false;
    if (present) {
        auto* vp = arena.array<VkPipelineViewportStateCreateInfo>(1);
        if (!read_raw(r, vp)) return false;
        if (!read_opt_pod_array(r, arena, vp->viewportCount,
                                 const_cast<VkViewport**>(&vp->pViewports)))
            return false;
        if (!read_opt_pod_array(r, arena, vp->scissorCount,
                                 const_cast<VkRect2D**>(&vp->pScissors)))
            return false;
        out->pViewportState = vp;
    } else {
        out->pViewportState = nullptr;
    }

    if (!read_opt_pod(
            r, arena,
            const_cast<VkPipelineRasterizationStateCreateInfo**>(&out->pRasterizationState)))
        return false;

    present = r.u32();
    if (!r.ok()) return false;
    if (present) {
        auto* ms = arena.array<VkPipelineMultisampleStateCreateInfo>(1);
        if (!read_raw(r, ms)) return false;
        const uint32_t samples = static_cast<uint32_t>(ms->rasterizationSamples);
        if (samples < VK_SAMPLE_COUNT_1_BIT || samples > VK_SAMPLE_COUNT_64_BIT ||
            (samples & (samples - 1)) != 0) return false;
        const uint32_t mask_words = (samples + 31) / 32;
        if (!read_opt_pod_array(r, arena, mask_words, const_cast<VkSampleMask**>(&ms->pSampleMask)))
            return false;
        out->pMultisampleState = ms;
    } else {
        out->pMultisampleState = nullptr;
    }

    if (!read_opt_pod(
            r, arena,
            const_cast<VkPipelineDepthStencilStateCreateInfo**>(&out->pDepthStencilState)))
        return false;

    present = r.u32();
    if (!r.ok()) return false;
    if (present) {
        auto* cb = arena.array<VkPipelineColorBlendStateCreateInfo>(1);
        if (!read_raw(r, cb)) return false;
        if (!read_pod_array(
                r, arena, cb->attachmentCount,
                const_cast<VkPipelineColorBlendAttachmentState**>(&cb->pAttachments)))
            return false;
        out->pColorBlendState = cb;
    } else {
        out->pColorBlendState = nullptr;
    }

    present = r.u32();
    if (!r.ok()) return false;
    if (present) {
        auto* ds = arena.array<VkPipelineDynamicStateCreateInfo>(1);
        if (!read_raw(r, ds)) return false;
        if (!read_pod_array(r, arena, ds->dynamicStateCount,
                             const_cast<VkDynamicState**>(&ds->pDynamicStates)))
            return false;
        out->pDynamicState = ds;
    } else {
        out->pDynamicState = nullptr;
    }

    if (!read_pnext_chain(r, arena, hr, PNextContext::GraphicsPipeline, &out->pNext)) return false;
    if (out->pTessellationState) {
        const void* tessellation_chain = nullptr;
        if (!read_pnext_chain(r, arena, hr, PNextContext::TessellationState,
                              &tessellation_chain)) return false;
        const_cast<VkPipelineTessellationStateCreateInfo*>(out->pTessellationState)->pNext =
            tessellation_chain;
    }
    patch_handle(&out->layout, [&](uint64_t id) { return hr.pipeline_layout(id); });
    patch_handle(&out->renderPass, [&](uint64_t id) { return hr.render_pass(id); });
    patch_handle(&out->basePipelineHandle, [&](uint64_t id) { return hr.pipeline(id); });
    return (!layout_id || out->layout != VK_NULL_HANDLE) &&
           (!render_pass_id || out->renderPass != VK_NULL_HANDLE) &&
           (!base_pipeline_id || out->basePipelineHandle != VK_NULL_HANDLE);
}

void write_ComputePipelineCreateInfo(Writer& w, const VkComputePipelineCreateInfo& s) {
    VkComputePipelineCreateInfo tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_PipelineShaderStageCreateInfo(w, s.stage);
}

bool read_ComputePipelineCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                    VkComputePipelineCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    out->pNext = nullptr;
    const uint64_t module_id = id_of(out->stage.module);
    const uint64_t layout_id = id_of(out->layout);
    const uint64_t base_pipeline_id = id_of(out->basePipelineHandle);
    if (!read_PipelineShaderStageCreateInfo(r, arena, hr, &out->stage)) return false;
    out->stage.pNext = nullptr;
    patch_handle(&out->layout, [&](uint64_t id) { return hr.pipeline_layout(id); });
    patch_handle(&out->basePipelineHandle, [&](uint64_t id) { return hr.pipeline(id); });
    return (!module_id || out->stage.module != VK_NULL_HANDLE) &&
           (!layout_id || out->layout != VK_NULL_HANDLE) &&
           (!base_pipeline_id || out->basePipelineHandle != VK_NULL_HANDLE);
}

bool write_FramebufferCreateInfo(Writer& w, const VkFramebufferCreateInfo& s) {
    Writer e;VkFramebufferCreateInfo tmp=s;tmp.pNext=nullptr;tmp.pAttachments=nullptr;e.bytes(&tmp,sizeof(tmp));
    if(!write_pnext_chain(e,PNextContext::FramebufferCreate,s.pNext))return false;
    if(!(s.flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))write_handle_array(e,s.pAttachments,s.attachmentCount);
    w.append(e);return true;
}

bool read_FramebufferCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkFramebufferCreateInfo* out) {
    if (!read_raw(r, out) || !read_pnext_chain(r,arena,hr,PNextContext::FramebufferCreate,&out->pNext)) return false;
    if (!(out->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && !read_handle_array(
            r, arena, out->attachmentCount, [&](uint64_t id) { return hr.image_view(id); },
            const_cast<VkImageView**>(&out->pAttachments)))
        return false;
    if(out->flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)out->pAttachments=nullptr;
    patch_handle(&out->renderPass, [&](uint64_t id) { return hr.render_pass(id); });
    return true;
}

bool write_BufferViewCreateInfo(Writer& w, const VkBufferViewCreateInfo& s) {
    Writer encoded;
    encoded.i32(static_cast<int32_t>(s.sType));
    encoded.u32(s.flags);
    encoded.handle(id_of(s.buffer));
    encoded.i32(static_cast<int32_t>(s.format));
    encoded.u64(s.offset);
    encoded.u64(s.range);
    if (s.sType != VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO || s.pNext != nullptr) return false;
    w.append(encoded);
    return true;
}

bool read_BufferViewCreateInfo(Reader& r, Arena&, const HandleResolver& hr,
                               VkBufferViewCreateInfo* out) {
    *out = {};
    out->sType = static_cast<VkStructureType>(r.i32());
    out->flags = r.u32();
    const uint64_t buffer_id = r.handle();
    out->format = static_cast<VkFormat>(r.i32());
    out->offset = r.u64();
    out->range = r.u64();
    out->buffer = buffer_id ? hr.buffer(buffer_id) : VK_NULL_HANDLE;
    return r.ok() && out->sType == VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO &&
           out->buffer != VK_NULL_HANDLE;
}

void write_QueryPoolCreateInfo(Writer& w, const VkQueryPoolCreateInfo& s) {
    w.i32(static_cast<int32_t>(s.sType));
    w.u32(s.flags);
    w.i32(static_cast<int32_t>(s.queryType));
    w.u32(s.queryCount);
    w.u32(s.pipelineStatistics);
    w.u32(s.pNext == nullptr ? 0 : 1);
}

bool read_QueryPoolCreateInfo(Reader& r, Arena&, const HandleResolver&,
                              VkQueryPoolCreateInfo* out) {
    *out = {};
    out->sType = static_cast<VkStructureType>(r.i32());
    out->flags = r.u32();
    out->queryType = static_cast<VkQueryType>(r.i32());
    out->queryCount = r.u32();
    out->pipelineStatistics = r.u32();
    const uint32_t has_pnext = r.u32();
    return r.ok() && out->sType == VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO &&
           !has_pnext && out->queryCount != 0 && out->queryCount <= kMaxArrayElements;
}

void write_EventCreateInfo(Writer& w, const VkEventCreateInfo& s) {
    w.i32(static_cast<int32_t>(s.sType));
    w.u32(s.flags);
    w.u32(s.pNext == nullptr ? 0 : 1);
}

bool read_EventCreateInfo(Reader& r, Arena&, const HandleResolver&, VkEventCreateInfo* out) {
    *out = {};
    out->sType = static_cast<VkStructureType>(r.i32());
    out->flags = r.u32();
    const uint32_t has_pnext = r.u32();
    return r.ok() && out->sType == VK_STRUCTURE_TYPE_EVENT_CREATE_INFO && !has_pnext;
}

void write_CommandBufferAllocateInfo(Writer& w, const VkCommandBufferAllocateInfo& s) {
    VkCommandBufferAllocateInfo tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_CommandBufferAllocateInfo(Reader& r, Arena&, const HandleResolver& hr,
                                     VkCommandBufferAllocateInfo* out) {
    if (!read_raw(r, out)) return false;
    // Unlike the array-bearing structs, this count has no wire array whose
    // length could bound it, so nothing else stops it before it sizes an
    // allocation and is handed to the real driver to fill.
    if (out->commandBufferCount > kMaxArrayElements) return false;
    patch_handle(&out->commandPool, [&](uint64_t id) { return hr.command_pool(id); });
    return true;
}

void write_CommandBufferBeginInfo(Writer& w, const VkCommandBufferBeginInfo& s) {
    w.i32(static_cast<int32_t>(s.sType));
    w.u32(s.flags);
    w.u32(s.pNext == nullptr ? 0 : 1);
    w.u32(s.pInheritanceInfo ? 1 : 0);
    if (s.pInheritanceInfo) {
        const auto& i = *s.pInheritanceInfo;
        w.i32(static_cast<int32_t>(i.sType));
        w.u32(i.pNext == nullptr ? 0 : 1);
        w.handle(id_of(i.renderPass));
        w.u32(i.subpass);
        w.handle(id_of(i.framebuffer));
        w.u32(i.occlusionQueryEnable);
        w.u32(i.queryFlags);
        w.u32(i.pipelineStatistics);
    }
}

bool read_CommandBufferBeginInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                  VkCommandBufferBeginInfo* out) {
    *out = {};
    out->sType = static_cast<VkStructureType>(r.i32());
    out->flags = r.u32();
    const uint32_t has_pnext = r.u32();
    const uint32_t has_inheritance = r.u32();
    if (!r.ok() || out->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO || has_pnext > 0 ||
        has_inheritance > 1) return false;
    if (has_inheritance) {
        auto* i = arena.array<VkCommandBufferInheritanceInfo>(1);
        *i = {};
        i->sType = static_cast<VkStructureType>(r.i32());
        const uint32_t inheritance_pnext = r.u32();
        const uint64_t render_pass = r.handle();
        i->subpass = r.u32();
        const uint64_t framebuffer = r.handle();
        i->occlusionQueryEnable = r.u32();
        i->queryFlags = r.u32();
        i->pipelineStatistics = r.u32();
        if (!r.ok() || i->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO ||
            inheritance_pnext != 0) return false;
        i->renderPass = render_pass ? hr.render_pass(render_pass) : VK_NULL_HANDLE;
        i->framebuffer = framebuffer ? hr.framebuffer(framebuffer) : VK_NULL_HANDLE;
        if ((render_pass && i->renderPass == VK_NULL_HANDLE) ||
            (framebuffer && i->framebuffer == VK_NULL_HANDLE)) return false;
        out->pInheritanceInfo = i;
    }
    return true;
}

bool write_RenderPassBeginInfo(Writer& w, const VkRenderPassBeginInfo& s) {
    Writer encoded;
    VkRenderPassBeginInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pClearValues = nullptr;
    encoded.bytes(&tmp, sizeof(tmp));
    if (!write_pnext_chain(encoded, PNextContext::RenderPassBegin, s.pNext)) return false;
    write_pod_array(encoded, s.pClearValues, s.clearValueCount);
    w.append(encoded);
    return true;
}

bool read_RenderPassBeginInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkRenderPassBeginInfo* out) {
    if (!read_raw(r, out)) return false;
    if (!read_pnext_chain(r, arena, hr, PNextContext::RenderPassBegin, &out->pNext)) return false;
    if (!read_pod_array(r, arena, out->clearValueCount,
                         const_cast<VkClearValue**>(&out->pClearValues)))
        return false;
    patch_handle(&out->renderPass, [&](uint64_t id) { return hr.render_pass(id); });
    patch_handle(&out->framebuffer, [&](uint64_t id) { return hr.framebuffer(id); });
    return true;
}

bool write_SubpassBeginInfo(Writer& w, const VkSubpassBeginInfo& s) {
    Writer encoded;
    encoded.i32(static_cast<int32_t>(s.sType));
    encoded.i32(static_cast<int32_t>(s.contents));
    if (s.sType != VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO ||
        !write_pnext_chain(encoded, PNextContext::RenderPass2, s.pNext))
        return false;
    w.append(encoded);
    return true;
}

bool read_SubpassBeginInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                           VkSubpassBeginInfo* out) {
    *out = {};
    out->sType = static_cast<VkStructureType>(r.i32());
    out->contents = static_cast<VkSubpassContents>(r.i32());
    if (!r.ok() || out->sType != VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO) return false;
    return read_pnext_chain(r, arena, hr, PNextContext::RenderPass2, &out->pNext);
}

bool write_SubpassEndInfo(Writer& w, const VkSubpassEndInfo& s) {
    Writer encoded;
    encoded.i32(static_cast<int32_t>(s.sType));
    if (s.sType != VK_STRUCTURE_TYPE_SUBPASS_END_INFO ||
        !write_pnext_chain(encoded, PNextContext::RenderPass2, s.pNext))
        return false;
    w.append(encoded);
    return true;
}

bool read_SubpassEndInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                         VkSubpassEndInfo* out) {
    *out = {};
    out->sType = static_cast<VkStructureType>(r.i32());
    if (!r.ok() || out->sType != VK_STRUCTURE_TYPE_SUBPASS_END_INFO) return false;
    return read_pnext_chain(r, arena, hr, PNextContext::RenderPass2, &out->pNext);
}

void write_DescriptorSetLayoutCreateInfo(Writer& w, const VkDescriptorSetLayoutCreateInfo& s) {
    VkDescriptorSetLayoutCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pBindings = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    for (uint32_t i = 0; i < s.bindingCount; ++i) {
        const VkDescriptorSetLayoutBinding& b = s.pBindings[i];
        VkDescriptorSetLayoutBinding t = b;
        t.pImmutableSamplers = nullptr;
        w.bytes(&t, sizeof(t));
        const bool has_samplers = b.pImmutableSamplers != nullptr;
        w.u32(has_samplers ? 1 : 0);
        if (has_samplers) write_handle_array(w, b.pImmutableSamplers, b.descriptorCount);
    }
}

bool read_DescriptorSetLayoutCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                         VkDescriptorSetLayoutCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    if (out->bindingCount > kMaxArrayElements || out->bindingCount > r.remaining() / 4) return false;
    VkDescriptorSetLayoutBinding* bindings =
        out->bindingCount ? arena.array<VkDescriptorSetLayoutBinding>(out->bindingCount) : nullptr;
    for (uint32_t i = 0; i < out->bindingCount; ++i) {
        if (!read_raw(r, &bindings[i])) return false;
        const uint32_t present = r.u32();
        if (!r.ok()) return false;
        if (present) {
            if (!read_handle_array(
                    r, arena, bindings[i].descriptorCount,
                    [&](uint64_t id) { return hr.sampler(id); },
                    const_cast<VkSampler**>(&bindings[i].pImmutableSamplers)))
                return false;
        } else {
            bindings[i].pImmutableSamplers = nullptr;
        }
    }
    out->pBindings = bindings;
    return true;
}

bool write_DescriptorUpdateTemplateCreateInfo(Writer& w,
    const VkDescriptorUpdateTemplateCreateInfo& s) {
    if (s.sType != VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO || s.pNext ||
        !valid_count(s.descriptorUpdateEntryCount, sizeof(VkDescriptorUpdateTemplateEntry)) ||
        (s.descriptorUpdateEntryCount && !s.pDescriptorUpdateEntries)) return false;
    Writer e; e.i32(s.sType);e.u32(s.flags);e.u32(s.descriptorUpdateEntryCount);
    for(uint32_t i=0;i<s.descriptorUpdateEntryCount;++i){const auto& x=s.pDescriptorUpdateEntries[i];
        e.u32(x.dstBinding);e.u32(x.dstArrayElement);e.u32(x.descriptorCount);e.i32(x.descriptorType);
        e.u64(x.offset);e.u64(x.stride);
    }
    e.i32(s.templateType);e.handle(reinterpret_cast<uint64_t>(s.descriptorSetLayout));
    e.i32(s.pipelineBindPoint);e.handle(reinterpret_cast<uint64_t>(s.pipelineLayout));e.u32(s.set);
    w.append(e);return true;
}

bool read_DescriptorUpdateTemplateCreateInfo(Reader& r, Arena& arena,
    const HandleResolver& hr, VkDescriptorUpdateTemplateCreateInfo* out) {
    *out={};out->sType=static_cast<VkStructureType>(r.i32());out->flags=r.u32();
    out->descriptorUpdateEntryCount=r.u32();
    if(!r.ok()||out->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO||
       !valid_count(out->descriptorUpdateEntryCount,sizeof(VkDescriptorUpdateTemplateEntry))||
       out->descriptorUpdateEntryCount>r.remaining()/32)return false;
    auto* entries=out->descriptorUpdateEntryCount?arena.array<VkDescriptorUpdateTemplateEntry>(out->descriptorUpdateEntryCount):nullptr;
    for(uint32_t i=0;i<out->descriptorUpdateEntryCount;++i){auto& x=entries[i];x.dstBinding=r.u32();x.dstArrayElement=r.u32();x.descriptorCount=r.u32();x.descriptorType=static_cast<VkDescriptorType>(r.i32());x.offset=static_cast<size_t>(r.u64());x.stride=static_cast<size_t>(r.u64());}
    out->pDescriptorUpdateEntries=entries;out->templateType=static_cast<VkDescriptorUpdateTemplateType>(r.i32());
    uint64_t layout=r.handle();out->descriptorSetLayout=layout?hr.descriptor_set_layout(layout):VK_NULL_HANDLE;
    out->pipelineBindPoint=static_cast<VkPipelineBindPoint>(r.i32());uint64_t pipeline=r.handle();
    out->pipelineLayout=pipeline?hr.pipeline_layout(pipeline):VK_NULL_HANDLE;out->set=r.u32();return r.ok();
}

void write_SamplerYcbcrConversionCreateInfo(Writer& w,
    const VkSamplerYcbcrConversionCreateInfo& s) {
    w.i32(s.sType);w.i32(s.format);w.i32(s.ycbcrModel);w.i32(s.ycbcrRange);
    w.i32(s.components.r);w.i32(s.components.g);w.i32(s.components.b);w.i32(s.components.a);
    w.i32(s.xChromaOffset);w.i32(s.yChromaOffset);w.i32(s.chromaFilter);w.u32(s.forceExplicitReconstruction);
}

bool read_SamplerYcbcrConversionCreateInfo(Reader& r, Arena&, const HandleResolver&,
    VkSamplerYcbcrConversionCreateInfo* out) {
    *out={};out->sType=static_cast<VkStructureType>(r.i32());out->format=static_cast<VkFormat>(r.i32());
    out->ycbcrModel=static_cast<VkSamplerYcbcrModelConversion>(r.i32());out->ycbcrRange=static_cast<VkSamplerYcbcrRange>(r.i32());
    out->components.r=static_cast<VkComponentSwizzle>(r.i32());out->components.g=static_cast<VkComponentSwizzle>(r.i32());
    out->components.b=static_cast<VkComponentSwizzle>(r.i32());out->components.a=static_cast<VkComponentSwizzle>(r.i32());
    out->xChromaOffset=static_cast<VkChromaLocation>(r.i32());out->yChromaOffset=static_cast<VkChromaLocation>(r.i32());
    out->chromaFilter=static_cast<VkFilter>(r.i32());out->forceExplicitReconstruction=r.u32();
    return r.ok()&&out->sType==VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
}

void write_DescriptorPoolCreateInfo(Writer& w, const VkDescriptorPoolCreateInfo& s) {
    VkDescriptorPoolCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pPoolSizes = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_pod_array(w, s.pPoolSizes, s.poolSizeCount);
}

bool read_DescriptorPoolCreateInfo(Reader& r, Arena& arena, const HandleResolver&,
                                    VkDescriptorPoolCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    return read_pod_array(r, arena, out->poolSizeCount,
                           const_cast<VkDescriptorPoolSize**>(&out->pPoolSizes));
}

void write_DescriptorSetAllocateInfo(Writer& w, const VkDescriptorSetAllocateInfo& s) {
    VkDescriptorSetAllocateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pSetLayouts = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_handle_array(w, s.pSetLayouts, s.descriptorSetCount);
}

bool read_DescriptorSetAllocateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                     VkDescriptorSetAllocateInfo* out) {
    if (!read_raw(r, out) || out->descriptorSetCount > kMaxArrayElements) return false;
    const uint64_t pool_id = id_of(out->descriptorPool);
    if (!read_handle_array(
            r, arena, out->descriptorSetCount,
            [&](uint64_t id) { return hr.descriptor_set_layout(id); },
            const_cast<VkDescriptorSetLayout**>(&out->pSetLayouts)))
        return false;
    for (uint32_t i = 0; i < out->descriptorSetCount; ++i) {
        if (out->pSetLayouts[i] == VK_NULL_HANDLE) return false;
    }
    patch_handle(&out->descriptorPool, [&](uint64_t id) { return hr.descriptor_pool(id); });
    return pool_id != 0 && out->descriptorPool != VK_NULL_HANDLE;
}

bool write_WriteDescriptorSet(Writer& w, const VkWriteDescriptorSet& s) {
    if (s.descriptorCount > kMaxArrayElements || s.dstSet == VK_NULL_HANDLE) return false;
    Writer encoded;
    VkWriteDescriptorSet tmp = s;
    tmp.pNext = nullptr;
    tmp.pImageInfo = nullptr;
    tmp.pBufferInfo = nullptr;
    tmp.pTexelBufferView = nullptr;
    encoded.bytes(&tmp, sizeof(tmp));

    // Discriminator: which of the three arrays is meaningful for this
    // descriptorType, per the Vulkan spec's rules for VkWriteDescriptorSet.
    switch (s.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            if (s.descriptorCount != 0 && s.pImageInfo == nullptr) return false;
            encoded.u32(1);
            for (uint32_t i = 0; i < s.descriptorCount; ++i) {
                const VkDescriptorImageInfo& info = s.pImageInfo[i];
                const bool sampler_used = s.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                                          s.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                const bool view_used = s.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER;
                if ((sampler_used && info.sampler == VK_NULL_HANDLE) ||
                    (view_used && info.imageView == VK_NULL_HANDLE)) {
                    return false;
                }
                encoded.handle(sampler_used ? id_of(info.sampler) : 0);
                encoded.handle(view_used ? id_of(info.imageView) : 0);
                encoded.i32(static_cast<int32_t>(info.imageLayout));
            }
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            if (s.descriptorCount != 0 && s.pBufferInfo == nullptr) return false;
            encoded.u32(2);
            for (uint32_t i = 0; i < s.descriptorCount; ++i) {
                const VkDescriptorBufferInfo& info = s.pBufferInfo[i];
                if (info.buffer == VK_NULL_HANDLE) return false;
                encoded.handle(id_of(info.buffer));
                encoded.u64(info.offset);
                encoded.u64(info.range);
            }
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            if (s.descriptorCount != 0 && s.pTexelBufferView == nullptr) return false;
            encoded.u32(3);
            for (uint32_t i = 0; i < s.descriptorCount; ++i) {
                if (s.pTexelBufferView[i] == VK_NULL_HANDLE) return false;
                encoded.handle(id_of(s.pTexelBufferView[i]));
            }
            break;
        default:
            return false;
    }
    w.append(encoded);
    return true;
}

bool read_WriteDescriptorSet(Reader& r, Arena& arena, const HandleResolver& hr,
                              VkWriteDescriptorSet* out) {
    if (!read_raw(r, out)) return false;
    if (out->descriptorCount > kMaxArrayElements) return false;

    const uint64_t dst_set_id = id_of(out->dstSet);
    const uint32_t kind = r.u32();
    uint32_t expected_kind = 0;
    switch (out->descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            expected_kind = 1;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            expected_kind = 2;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            expected_kind = 3;
            break;
        default:
            return false;
    }
    if (!r.ok() || kind != expected_kind) return false;

    out->pImageInfo = nullptr;
    out->pBufferInfo = nullptr;
    out->pTexelBufferView = nullptr;

    if (kind == 1) {
        VkDescriptorImageInfo* infos =
            out->descriptorCount ? arena.array<VkDescriptorImageInfo>(out->descriptorCount)
                                  : nullptr;
        for (uint32_t i = 0; i < out->descriptorCount; ++i) {
            const uint64_t sampler_id = r.handle();
            const uint64_t view_id = r.handle();
            const int32_t layout = r.i32();
            if (!r.ok()) return false;
            const bool sampler_used = out->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                                      out->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            const bool view_used = out->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER;
            infos[i].sampler = sampler_used && sampler_id ? hr.sampler(sampler_id)
                                                          : static_cast<VkSampler>(0);
            infos[i].imageView = view_used && view_id ? hr.image_view(view_id)
                                                       : static_cast<VkImageView>(0);
            if ((sampler_used && (sampler_id == 0 || infos[i].sampler == VK_NULL_HANDLE)) ||
                (view_used && (view_id == 0 || infos[i].imageView == VK_NULL_HANDLE))) {
                return false;
            }
            infos[i].imageLayout = static_cast<VkImageLayout>(layout);
        }
        out->pImageInfo = infos;
    } else if (kind == 2) {
        VkDescriptorBufferInfo* infos =
            out->descriptorCount ? arena.array<VkDescriptorBufferInfo>(out->descriptorCount)
                                  : nullptr;
        for (uint32_t i = 0; i < out->descriptorCount; ++i) {
            const uint64_t buffer_id = r.handle();
            const uint64_t offset = r.u64();
            const uint64_t range = r.u64();
            if (!r.ok()) return false;
            infos[i].buffer = buffer_id ? hr.buffer(buffer_id) : static_cast<VkBuffer>(0);
            if (buffer_id == 0 || infos[i].buffer == VK_NULL_HANDLE) return false;
            infos[i].offset = offset;
            infos[i].range = range;
        }
        out->pBufferInfo = infos;
    } else if (kind == 3) {
        VkBufferView* views = out->descriptorCount ? arena.array<VkBufferView>(out->descriptorCount)
                                                   : nullptr;
        for (uint32_t i = 0; i < out->descriptorCount; ++i) {
            const uint64_t id = r.handle();
            if (!r.ok() || id == 0 || hr.buffer_view(id) == VK_NULL_HANDLE) return false;
            views[i] = hr.buffer_view(id);
        }
        out->pTexelBufferView = views;
    } else {
        return false;
    }

    patch_handle(&out->dstSet, [&](uint64_t id) { return hr.descriptor_set(id); });
    return dst_set_id != 0 && out->dstSet != VK_NULL_HANDLE;
}

void write_CopyDescriptorSet(Writer& w, const VkCopyDescriptorSet& s) {
    VkCopyDescriptorSet tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_CopyDescriptorSet(Reader& r, Arena&, const HandleResolver& hr,
                             VkCopyDescriptorSet* out) {
    if (!read_raw(r, out)) return false;
    const uint64_t src_set_id = id_of(out->srcSet);
    const uint64_t dst_set_id = id_of(out->dstSet);
    patch_handle(&out->srcSet, [&](uint64_t id) { return hr.descriptor_set(id); });
    patch_handle(&out->dstSet, [&](uint64_t id) { return hr.descriptor_set(id); });
    return src_set_id != 0 && out->srcSet != VK_NULL_HANDLE &&
           dst_set_id != 0 && out->dstSet != VK_NULL_HANDLE;
}

void write_ImageMemoryBarrier(Writer& w, const VkImageMemoryBarrier& s) {
    VkImageMemoryBarrier tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_ImageMemoryBarrier(Reader& r, Arena&, const HandleResolver& hr,
                              VkImageMemoryBarrier* out) {
    if (!read_raw(r, out)) return false;
    patch_handle(&out->image, [&](uint64_t id) { return hr.image(id); });
    return true;
}

void write_BufferMemoryBarrier(Writer& w, const VkBufferMemoryBarrier& s) {
    VkBufferMemoryBarrier tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_BufferMemoryBarrier(Reader& r, Arena&, const HandleResolver& hr,
                               VkBufferMemoryBarrier* out) {
    if (!read_raw(r, out)) return false;
    patch_handle(&out->buffer, [&](uint64_t id) { return hr.buffer(id); });
    return true;
}

void write_MemoryBarrier(Writer& w, const VkMemoryBarrier& s) {
    VkMemoryBarrier tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_MemoryBarrier(Reader& r, Arena&, const HandleResolver&, VkMemoryBarrier* out) {
    return read_raw(r, out);
}

namespace {
#pragma pack(push, 1)
struct MemoryBarrier2Wire {
    uint64_t src_stage_mask;
    uint64_t src_access_mask;
    uint64_t dst_stage_mask;
    uint64_t dst_access_mask;
};

struct BufferMemoryBarrier2Wire {
    uint64_t src_stage_mask;
    uint64_t src_access_mask;
    uint64_t dst_stage_mask;
    uint64_t dst_access_mask;
    uint32_t src_queue_family_index;
    uint32_t dst_queue_family_index;
    uint64_t buffer;
    uint64_t offset;
    uint64_t size;
};

struct ImageMemoryBarrier2Wire {
    uint64_t src_stage_mask;
    uint64_t src_access_mask;
    uint64_t dst_stage_mask;
    uint64_t dst_access_mask;
    int32_t old_layout;
    int32_t new_layout;
    uint32_t src_queue_family_index;
    uint32_t dst_queue_family_index;
    uint64_t image;
    uint32_t aspect_mask;
    uint32_t base_mip_level;
    uint32_t level_count;
    uint32_t base_array_layer;
    uint32_t layer_count;
};

struct SemaphoreSubmitInfoWire {
    uint64_t semaphore;
    uint64_t value;
    uint64_t stage_mask;
    uint32_t device_index;
};

struct CommandBufferSubmitInfoWire {
    uint64_t command_buffer;
    uint32_t device_mask;
};
#pragma pack(pop)

constexpr size_t kMemoryBarrier2WireBytes = sizeof(MemoryBarrier2Wire);
constexpr size_t kBufferMemoryBarrier2WireBytes = sizeof(BufferMemoryBarrier2Wire);
constexpr size_t kImageMemoryBarrier2WireBytes = sizeof(ImageMemoryBarrier2Wire);
constexpr size_t kSemaphoreSubmitInfoWireBytes = sizeof(SemaphoreSubmitInfoWire);
constexpr size_t kCommandBufferSubmitInfoWireBytes = sizeof(CommandBufferSubmitInfoWire);
}  // namespace

bool write_DependencyInfo(Writer& w, const VkDependencyInfo& s) {
    if (s.sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO || s.pNext ||
        s.memoryBarrierCount > kMaxArrayElements ||
        s.bufferMemoryBarrierCount > kMaxArrayElements ||
        s.imageMemoryBarrierCount > kMaxArrayElements ||
        (s.memoryBarrierCount && !s.pMemoryBarriers) ||
        (s.bufferMemoryBarrierCount && !s.pBufferMemoryBarriers) ||
        (s.imageMemoryBarrierCount && !s.pImageMemoryBarriers)) return false;
    w.u32(s.dependencyFlags);
    w.u32(s.memoryBarrierCount);
    for (uint32_t i=0;i<s.memoryBarrierCount;++i){const auto&b=s.pMemoryBarriers[i];if(b.sType!=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2||b.pNext)return false;w.u64(b.srcStageMask);w.u64(b.srcAccessMask);w.u64(b.dstStageMask);w.u64(b.dstAccessMask);}
    w.u32(s.bufferMemoryBarrierCount);
    for(uint32_t i=0;i<s.bufferMemoryBarrierCount;++i){const auto&b=s.pBufferMemoryBarriers[i];if(b.sType!=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2||b.pNext)return false;w.u64(b.srcStageMask);w.u64(b.srcAccessMask);w.u64(b.dstStageMask);w.u64(b.dstAccessMask);w.u32(b.srcQueueFamilyIndex);w.u32(b.dstQueueFamilyIndex);w.handle(id_of(b.buffer));w.u64(b.offset);w.u64(b.size);}
    w.u32(s.imageMemoryBarrierCount);
    for(uint32_t i=0;i<s.imageMemoryBarrierCount;++i){const auto&b=s.pImageMemoryBarriers[i];if(b.sType!=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2||b.pNext)return false;w.u64(b.srcStageMask);w.u64(b.srcAccessMask);w.u64(b.dstStageMask);w.u64(b.dstAccessMask);w.i32(b.oldLayout);w.i32(b.newLayout);w.u32(b.srcQueueFamilyIndex);w.u32(b.dstQueueFamilyIndex);w.handle(id_of(b.image));w.u32(b.subresourceRange.aspectMask);w.u32(b.subresourceRange.baseMipLevel);w.u32(b.subresourceRange.levelCount);w.u32(b.subresourceRange.baseArrayLayer);w.u32(b.subresourceRange.layerCount);}
    return true;
}

bool read_DependencyInfo(Reader& r,Arena& arena,const HandleResolver&hr,VkDependencyInfo*out){*out={VK_STRUCTURE_TYPE_DEPENDENCY_INFO};out->dependencyFlags=r.u32();uint32_t mn=r.u32();if(mn>kMaxArrayElements||mn>r.remaining()/kMemoryBarrier2WireBytes)return false;auto*m=arena.array<VkMemoryBarrier2>(mn);for(uint32_t i=0;i<mn;++i){m[i]={VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};m[i].srcStageMask=r.u64();m[i].srcAccessMask=r.u64();m[i].dstStageMask=r.u64();m[i].dstAccessMask=r.u64();}out->memoryBarrierCount=mn;out->pMemoryBarriers=m;uint32_t bn=r.u32();if(bn>kMaxArrayElements||bn>r.remaining()/kBufferMemoryBarrier2WireBytes)return false;auto*b=arena.array<VkBufferMemoryBarrier2>(bn);for(uint32_t i=0;i<bn;++i){b[i]={VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};b[i].srcStageMask=r.u64();b[i].srcAccessMask=r.u64();b[i].dstStageMask=r.u64();b[i].dstAccessMask=r.u64();b[i].srcQueueFamilyIndex=r.u32();b[i].dstQueueFamilyIndex=r.u32();uint64_t id=r.handle();b[i].buffer=hr.buffer(id);b[i].offset=r.u64();b[i].size=r.u64();if(id&&!b[i].buffer)return false;}out->bufferMemoryBarrierCount=bn;out->pBufferMemoryBarriers=b;uint32_t in=r.u32();if(in>kMaxArrayElements||in>r.remaining()/kImageMemoryBarrier2WireBytes)return false;auto*im=arena.array<VkImageMemoryBarrier2>(in);for(uint32_t i=0;i<in;++i){im[i]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};im[i].srcStageMask=r.u64();im[i].srcAccessMask=r.u64();im[i].dstStageMask=r.u64();im[i].dstAccessMask=r.u64();im[i].oldLayout=static_cast<VkImageLayout>(r.i32());im[i].newLayout=static_cast<VkImageLayout>(r.i32());im[i].srcQueueFamilyIndex=r.u32();im[i].dstQueueFamilyIndex=r.u32();uint64_t id=r.handle();im[i].image=hr.image(id);im[i].subresourceRange.aspectMask=r.u32();im[i].subresourceRange.baseMipLevel=r.u32();im[i].subresourceRange.levelCount=r.u32();im[i].subresourceRange.baseArrayLayer=r.u32();im[i].subresourceRange.layerCount=r.u32();if(id&&!im[i].image)return false;}out->imageMemoryBarrierCount=in;out->pImageMemoryBarriers=im;return r.ok();}

bool write_SubmitInfo2(Writer&w,const VkSubmitInfo2&s,uint64_t(*command_buffer_id)(VkCommandBuffer)){if(s.sType!=VK_STRUCTURE_TYPE_SUBMIT_INFO_2||s.pNext||s.waitSemaphoreInfoCount>kMaxArrayElements||s.commandBufferInfoCount>kMaxArrayElements||s.signalSemaphoreInfoCount>kMaxArrayElements||(s.waitSemaphoreInfoCount&&!s.pWaitSemaphoreInfos)||(s.commandBufferInfoCount&&!s.pCommandBufferInfos)||(s.signalSemaphoreInfoCount&&!s.pSignalSemaphoreInfos))return false;w.u32(s.flags);w.u32(s.waitSemaphoreInfoCount);for(uint32_t i=0;i<s.waitSemaphoreInfoCount;++i){const auto&x=s.pWaitSemaphoreInfos[i];if(x.sType!=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO||x.pNext)return false;w.handle(id_of(x.semaphore));w.u64(x.value);w.u64(x.stageMask);w.u32(x.deviceIndex);}w.u32(s.commandBufferInfoCount);for(uint32_t i=0;i<s.commandBufferInfoCount;++i){const auto&x=s.pCommandBufferInfos[i];if(x.sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO||x.pNext)return false;w.handle(command_buffer_id?command_buffer_id(x.commandBuffer):id_of(x.commandBuffer));w.u32(x.deviceMask);}w.u32(s.signalSemaphoreInfoCount);for(uint32_t i=0;i<s.signalSemaphoreInfoCount;++i){const auto&x=s.pSignalSemaphoreInfos[i];if(x.sType!=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO||x.pNext)return false;w.handle(id_of(x.semaphore));w.u64(x.value);w.u64(x.stageMask);w.u32(x.deviceIndex);}return true;}

bool read_SubmitInfo2(Reader&r,Arena&arena,const HandleResolver&hr,VkSubmitInfo2*out){*out={VK_STRUCTURE_TYPE_SUBMIT_INFO_2};out->flags=r.u32();uint32_t wn=r.u32();if(wn>kMaxArrayElements||wn>r.remaining()/kSemaphoreSubmitInfoWireBytes)return false;auto*w=arena.array<VkSemaphoreSubmitInfo>(wn);for(uint32_t i=0;i<wn;++i){w[i]={VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};uint64_t id=r.handle();w[i].semaphore=hr.semaphore(id);w[i].value=r.u64();w[i].stageMask=r.u64();w[i].deviceIndex=r.u32();if(id&&!w[i].semaphore)return false;}out->waitSemaphoreInfoCount=wn;out->pWaitSemaphoreInfos=w;uint32_t cn=r.u32();if(cn>kMaxArrayElements||cn>r.remaining()/kCommandBufferSubmitInfoWireBytes)return false;auto*c=arena.array<VkCommandBufferSubmitInfo>(cn);for(uint32_t i=0;i<cn;++i){c[i]={VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};uint64_t id=r.handle();c[i].commandBuffer=hr.command_buffer(id);c[i].deviceMask=r.u32();if(id&&!c[i].commandBuffer)return false;}out->commandBufferInfoCount=cn;out->pCommandBufferInfos=c;uint32_t sn=r.u32();if(sn>kMaxArrayElements||sn>r.remaining()/kSemaphoreSubmitInfoWireBytes)return false;auto*s=arena.array<VkSemaphoreSubmitInfo>(sn);for(uint32_t i=0;i<sn;++i){s[i]={VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};uint64_t id=r.handle();s[i].semaphore=hr.semaphore(id);s[i].value=r.u64();s[i].stageMask=r.u64();s[i].deviceIndex=r.u32();if(id&&!s[i].semaphore)return false;}out->signalSemaphoreInfoCount=sn;out->pSignalSemaphoreInfos=s;return r.ok();}

// ---------------------------------------------------------------------------
// Group B
// ---------------------------------------------------------------------------

void write_BufferCreateInfo(Writer& w, const VkBufferCreateInfo& s) {
    Writer encoded; VkBufferCreateInfo tmp = s;
    tmp.pNext = nullptr; tmp.pQueueFamilyIndices = nullptr;
    encoded.bytes(&tmp, sizeof(tmp));
    if (!write_pnext_chain(encoded, PNextContext::BufferCreate, s.pNext)) return;
    write_pod_array(encoded, s.pQueueFamilyIndices, s.queueFamilyIndexCount);
    w.append(encoded);
}

bool read_BufferCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                            VkBufferCreateInfo* out) {
    if (!read_raw(r, out) || !read_pnext_chain(r, arena, hr, PNextContext::BufferCreate,
                                               &out->pNext)) return false;
    return read_pod_array(r, arena, out->queueFamilyIndexCount,
                           const_cast<uint32_t**>(&out->pQueueFamilyIndices));
}

bool write_ImageCreateInfo(Writer& w, const VkImageCreateInfo& s) {
    Writer e;VkImageCreateInfo tmp=s;tmp.pNext=nullptr;tmp.pQueueFamilyIndices=nullptr;e.bytes(&tmp,sizeof(tmp));
    if(!write_pnext_chain(e,PNextContext::ImageCreate,s.pNext))return false;
    write_pod_array(e,s.pQueueFamilyIndices,s.queueFamilyIndexCount);w.append(e);return true;
}

bool read_ImageCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr, VkImageCreateInfo* out) {
    if (!read_raw(r, out) || !read_pnext_chain(r,arena,hr,PNextContext::ImageCreate,&out->pNext)) return false;
    return read_pod_array(r, arena, out->queueFamilyIndexCount,
                           const_cast<uint32_t**>(&out->pQueueFamilyIndices));
}

void write_MemoryAllocateInfo(Writer& w, const VkMemoryAllocateInfo& s) {
    Writer encoded; VkMemoryAllocateInfo tmp = s; tmp.pNext = nullptr;
    encoded.bytes(&tmp, sizeof(tmp));
    if (!write_pnext_chain(encoded, PNextContext::MemoryAllocate, s.pNext)) return;
    w.append(encoded);
}

bool read_MemoryAllocateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                              VkMemoryAllocateInfo* out) {
    return read_raw(r, out) && read_pnext_chain(r, arena, hr, PNextContext::MemoryAllocate,
                                                &out->pNext);
}

void write_SamplerCreateInfo(Writer& w, const VkSamplerCreateInfo& s) {
    Writer encoded; VkSamplerCreateInfo tmp = s; tmp.pNext = nullptr;
    encoded.bytes(&tmp, sizeof(tmp));
    if (!write_pnext_chain(encoded, PNextContext::SamplerCreate, s.pNext)) return;
    w.append(encoded);
}

bool read_SamplerCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                            VkSamplerCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    return read_pnext_chain(r, arena, hr, PNextContext::SamplerCreate, &out->pNext);
}

void write_FenceCreateInfo(Writer& w, const VkFenceCreateInfo& s) {
    VkFenceCreateInfo tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_FenceCreateInfo(Reader& r, Arena&, const HandleResolver&, VkFenceCreateInfo* out) {
    return read_raw(r, out);
}

void write_SemaphoreCreateInfo(Writer& w, const VkSemaphoreCreateInfo& s) {
    Writer encoded; VkSemaphoreCreateInfo tmp = s; tmp.pNext = nullptr;
    encoded.bytes(&tmp, sizeof(tmp));
    if (!write_pnext_chain(encoded, PNextContext::SemaphoreCreate, s.pNext)) return;
    w.append(encoded);
}

bool read_SemaphoreCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkSemaphoreCreateInfo* out) {
    return read_raw(r, out) && read_pnext_chain(r, arena, hr, PNextContext::SemaphoreCreate,
                                                &out->pNext);
}

void write_CommandPoolCreateInfo(Writer& w, const VkCommandPoolCreateInfo& s) {
    VkCommandPoolCreateInfo tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_CommandPoolCreateInfo(Reader& r, Arena&, const HandleResolver&,
                                 VkCommandPoolCreateInfo* out) {
    return read_raw(r, out);
}

void write_PipelineCacheCreateInfo(Writer& w, const VkPipelineCacheCreateInfo& s) {
    VkPipelineCacheCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pInitialData = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    w.bytes(s.pInitialData, s.initialDataSize);
}

bool read_PipelineCacheCreateInfo(Reader& r, Arena& arena, const HandleResolver&,
                                   VkPipelineCacheCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    void* data = nullptr;
    if (!read_bytes_to_arena(r, arena, out->initialDataSize, &data)) return false;
    out->pInitialData = data;
    return true;
}

void write_BufferImageCopy(Writer& w, const VkBufferImageCopy& s) { w.bytes(&s, sizeof(s)); }

bool read_BufferImageCopy(Reader& r, Arena&, const HandleResolver&, VkBufferImageCopy* out) {
    return read_raw(r, out);
}

void write_ImageCopy(Writer& w, const VkImageCopy& s) { w.bytes(&s, sizeof(s)); }

bool read_ImageCopy(Reader& r, Arena&, const HandleResolver&, VkImageCopy* out) {
    return read_raw(r, out);
}

void write_ImageBlit(Writer& w, const VkImageBlit& s) { w.bytes(&s, sizeof(s)); }

bool read_ImageBlit(Reader& r, Arena&, const HandleResolver&, VkImageBlit* out) {
    return read_raw(r, out);
}

void write_BufferCopy(Writer& w, const VkBufferCopy& s) { w.bytes(&s, sizeof(s)); }

bool read_BufferCopy(Reader& r, Arena&, const HandleResolver&, VkBufferCopy* out) {
    return read_raw(r, out);
}

void write_Viewport(Writer& w, const VkViewport& s) { w.bytes(&s, sizeof(s)); }

bool read_Viewport(Reader& r, Arena&, const HandleResolver&, VkViewport* out) {
    return read_raw(r, out);
}

void write_Rect2D(Writer& w, const VkRect2D& s) { w.bytes(&s, sizeof(s)); }

bool read_Rect2D(Reader& r, Arena&, const HandleResolver&, VkRect2D* out) {
    return read_raw(r, out);
}

void write_MemoryRequirements(Writer& w, const VkMemoryRequirements& s) {
    w.bytes(&s, sizeof(s));
}

bool read_MemoryRequirements(Reader& r, Arena&, const HandleResolver&,
                              VkMemoryRequirements* out) {
    return read_raw(r, out);
}

void write_SubresourceLayout(Writer& w, const VkSubresourceLayout& s) { w.bytes(&s, sizeof(s)); }

bool read_SubresourceLayout(Reader& r, Arena&, const HandleResolver&,
                             VkSubresourceLayout* out) {
    return read_raw(r, out);
}

void write_SurfaceCapabilitiesKHR(Writer& w, const VkSurfaceCapabilitiesKHR& s) {
    w.bytes(&s, sizeof(s));
}

bool read_SurfaceCapabilitiesKHR(Reader& r, Arena&, const HandleResolver&,
                                  VkSurfaceCapabilitiesKHR* out) {
    return read_raw(r, out);
}

void write_SurfaceFormatKHR(Writer& w, const VkSurfaceFormatKHR& s) {
    w.i32(static_cast<int32_t>(s.format));
    w.i32(static_cast<int32_t>(s.colorSpace));
}

bool read_SurfaceFormatKHR(Reader& r, VkSurfaceFormatKHR* out) {
    out->format = static_cast<VkFormat>(r.i32());
    out->colorSpace = static_cast<VkColorSpaceKHR>(r.i32());
    return r.ok();
}

#define VK_PHYSICAL_DEVICE_LIMITS_U32_FIELDS(X)             \
    X(maxImageDimension1D)                                   \
    X(maxImageDimension2D)                                   \
    X(maxImageDimension3D)                                   \
    X(maxImageDimensionCube)                                 \
    X(maxImageArrayLayers)                                   \
    X(maxTexelBufferElements)                                \
    X(maxUniformBufferRange)                                 \
    X(maxStorageBufferRange)                                 \
    X(maxPushConstantsSize)                                  \
    X(maxMemoryAllocationCount)                              \
    X(maxSamplerAllocationCount)                             \
    X(maxBoundDescriptorSets)                                \
    X(maxPerStageDescriptorSamplers)                         \
    X(maxPerStageDescriptorUniformBuffers)                   \
    X(maxPerStageDescriptorStorageBuffers)                   \
    X(maxPerStageDescriptorSampledImages)                    \
    X(maxPerStageDescriptorStorageImages)                    \
    X(maxPerStageDescriptorInputAttachments)                 \
    X(maxPerStageResources)                                  \
    X(maxDescriptorSetSamplers)                              \
    X(maxDescriptorSetUniformBuffers)                        \
    X(maxDescriptorSetUniformBuffersDynamic)                 \
    X(maxDescriptorSetStorageBuffers)                        \
    X(maxDescriptorSetStorageBuffersDynamic)                 \
    X(maxDescriptorSetSampledImages)                         \
    X(maxDescriptorSetStorageImages)                         \
    X(maxDescriptorSetInputAttachments)                      \
    X(maxVertexInputAttributes)                              \
    X(maxVertexInputBindings)                                \
    X(maxVertexInputAttributeOffset)                         \
    X(maxVertexInputBindingStride)                           \
    X(maxVertexOutputComponents)                             \
    X(maxTessellationGenerationLevel)                        \
    X(maxTessellationPatchSize)                              \
    X(maxTessellationControlPerVertexInputComponents)        \
    X(maxTessellationControlPerVertexOutputComponents)       \
    X(maxTessellationControlPerPatchOutputComponents)        \
    X(maxTessellationControlTotalOutputComponents)           \
    X(maxTessellationEvaluationInputComponents)              \
    X(maxTessellationEvaluationOutputComponents)             \
    X(maxGeometryShaderInvocations)                          \
    X(maxGeometryInputComponents)                            \
    X(maxGeometryOutputComponents)                           \
    X(maxGeometryOutputVertices)                             \
    X(maxGeometryTotalOutputComponents)                      \
    X(maxFragmentInputComponents)                            \
    X(maxFragmentOutputAttachments)                          \
    X(maxFragmentDualSrcAttachments)                         \
    X(maxFragmentCombinedOutputResources)                    \
    X(maxComputeSharedMemorySize)                            \
    X(maxComputeWorkGroupInvocations)                        \
    X(subPixelPrecisionBits)                                 \
    X(subTexelPrecisionBits)                                 \
    X(mipmapPrecisionBits)                                   \
    X(maxDrawIndexedIndexValue)                              \
    X(maxDrawIndirectCount)                                  \
    X(maxViewports)                                          \
    X(viewportSubPixelBits)                                  \
    X(maxTexelOffset)                                        \
    X(maxTexelGatherOffset)                                  \
    X(subPixelInterpolationOffsetBits)                       \
    X(maxFramebufferWidth)                                   \
    X(maxFramebufferHeight)                                  \
    X(maxFramebufferLayers)                                  \
    X(framebufferColorSampleCounts)                          \
    X(framebufferDepthSampleCounts)                          \
    X(framebufferStencilSampleCounts)                        \
    X(framebufferNoAttachmentsSampleCounts)                  \
    X(maxColorAttachments)                                   \
    X(sampledImageColorSampleCounts)                         \
    X(sampledImageIntegerSampleCounts)                       \
    X(sampledImageDepthSampleCounts)                         \
    X(sampledImageStencilSampleCounts)                       \
    X(storageImageSampleCounts)                              \
    X(maxSampleMaskWords)                                    \
    X(timestampComputeAndGraphics)                           \
    X(maxClipDistances)                                      \
    X(maxCullDistances)                                      \
    X(maxCombinedClipAndCullDistances)                       \
    X(discreteQueuePriorities)                               \
    X(strictLines)                                           \
    X(standardSampleLocations)

#define VK_PHYSICAL_DEVICE_LIMITS_U64_FIELDS(X) \
    X(bufferImageGranularity)                    \
    X(sparseAddressSpaceSize)                    \
    X(minTexelBufferOffsetAlignment)             \
    X(minUniformBufferOffsetAlignment)           \
    X(minStorageBufferOffsetAlignment)           \
    X(optimalBufferCopyOffsetAlignment)          \
    X(optimalBufferCopyRowPitchAlignment)        \
    X(nonCoherentAtomSize)

#define VK_PHYSICAL_DEVICE_LIMITS_F32_FIELDS(X) \
    X(maxSamplerLodBias)                         \
    X(maxSamplerAnisotropy)                      \
    X(minInterpolationOffset)                    \
    X(maxInterpolationOffset)                    \
    X(timestampPeriod)                           \
    X(pointSizeGranularity)                      \
    X(lineWidthGranularity)

#define VK_PHYSICAL_DEVICE_SPARSE_U32_FIELDS(X)          \
    X(residencyStandard2DBlockShape)                     \
    X(residencyStandard2DMultisampleBlockShape)          \
    X(residencyStandard3DBlockShape)                     \
    X(residencyAlignedMipSize)                           \
    X(residencyNonResidentStrict)

void write_PhysicalDeviceProperties(Writer& w, const VkPhysicalDeviceProperties& s) {
    w.u32(s.apiVersion);
    w.u32(s.driverVersion);
    w.u32(s.vendorID);
    w.u32(s.deviceID);
    w.i32(static_cast<int32_t>(s.deviceType));
    w.bytes(s.deviceName, sizeof(s.deviceName));
    w.bytes(s.pipelineCacheUUID, sizeof(s.pipelineCacheUUID));

    const VkPhysicalDeviceLimits& l = s.limits;
#define WRITE_LIMIT_U32(field) w.u32(static_cast<uint32_t>(l.field));
    VK_PHYSICAL_DEVICE_LIMITS_U32_FIELDS(WRITE_LIMIT_U32)
#undef WRITE_LIMIT_U32
#define WRITE_LIMIT_U64(field) w.u64(static_cast<uint64_t>(l.field));
    VK_PHYSICAL_DEVICE_LIMITS_U64_FIELDS(WRITE_LIMIT_U64)
#undef WRITE_LIMIT_U64
#define WRITE_LIMIT_F32(field) w.f32(l.field);
    VK_PHYSICAL_DEVICE_LIMITS_F32_FIELDS(WRITE_LIMIT_F32)
#undef WRITE_LIMIT_F32
    for (uint32_t value : l.maxComputeWorkGroupCount) w.u32(value);
    for (uint32_t value : l.maxComputeWorkGroupSize) w.u32(value);
    for (uint32_t value : l.maxViewportDimensions) w.u32(value);
    for (float value : l.viewportBoundsRange) w.f32(value);
    w.u64(static_cast<uint64_t>(l.minMemoryMapAlignment));
    w.i32(l.minTexelOffset);
    w.i32(l.minTexelGatherOffset);
    for (float value : l.pointSizeRange) w.f32(value);
    for (float value : l.lineWidthRange) w.f32(value);

#define WRITE_SPARSE_U32(field) w.u32(static_cast<uint32_t>(s.sparseProperties.field));
    VK_PHYSICAL_DEVICE_SPARSE_U32_FIELDS(WRITE_SPARSE_U32)
#undef WRITE_SPARSE_U32
}

bool read_PhysicalDeviceProperties(Reader& r, VkPhysicalDeviceProperties* out) {
    *out = {};
    out->apiVersion = r.u32();
    out->driverVersion = r.u32();
    out->vendorID = r.u32();
    out->deviceID = r.u32();
    out->deviceType = static_cast<VkPhysicalDeviceType>(r.i32());

    std::vector<char> device_name;
    std::vector<char> pipeline_cache_uuid;
    if (!r.bytes(&device_name, sizeof(out->deviceName)) ||
        device_name.size() != sizeof(out->deviceName) ||
        !r.bytes(&pipeline_cache_uuid, sizeof(out->pipelineCacheUUID)) ||
        pipeline_cache_uuid.size() != sizeof(out->pipelineCacheUUID)) {
        return false;
    }
    std::memcpy(out->deviceName, device_name.data(), device_name.size());
    std::memcpy(out->pipelineCacheUUID, pipeline_cache_uuid.data(), pipeline_cache_uuid.size());
    // A conformant server supplies a terminated deviceName. Force termination
    // anyway so malformed remote input can never make client code over-read it.
    out->deviceName[sizeof(out->deviceName) - 1] = '\0';

    VkPhysicalDeviceLimits& l = out->limits;
#define READ_LIMIT_U32(field) l.field = r.u32();
    VK_PHYSICAL_DEVICE_LIMITS_U32_FIELDS(READ_LIMIT_U32)
#undef READ_LIMIT_U32
#define READ_LIMIT_U64(field) l.field = r.u64();
    VK_PHYSICAL_DEVICE_LIMITS_U64_FIELDS(READ_LIMIT_U64)
#undef READ_LIMIT_U64
#define READ_LIMIT_F32(field) l.field = r.f32();
    VK_PHYSICAL_DEVICE_LIMITS_F32_FIELDS(READ_LIMIT_F32)
#undef READ_LIMIT_F32
    for (uint32_t& value : l.maxComputeWorkGroupCount) value = r.u32();
    for (uint32_t& value : l.maxComputeWorkGroupSize) value = r.u32();
    for (uint32_t& value : l.maxViewportDimensions) value = r.u32();
    for (float& value : l.viewportBoundsRange) value = r.f32();
    const uint64_t min_memory_map_alignment = r.u64();
    // Parentheses avoid expansion of Windows' legacy max macro.
    if (min_memory_map_alignment > (std::numeric_limits<size_t>::max)()) return false;
    l.minMemoryMapAlignment = static_cast<size_t>(min_memory_map_alignment);
    l.minTexelOffset = r.i32();
    l.minTexelGatherOffset = r.i32();
    for (float& value : l.pointSizeRange) value = r.f32();
    for (float& value : l.lineWidthRange) value = r.f32();

#define READ_SPARSE_U32(field) out->sparseProperties.field = r.u32();
    VK_PHYSICAL_DEVICE_SPARSE_U32_FIELDS(READ_SPARSE_U32)
#undef READ_SPARSE_U32
    return r.ok();
}

#undef VK_PHYSICAL_DEVICE_SPARSE_U32_FIELDS
#undef VK_PHYSICAL_DEVICE_LIMITS_F32_FIELDS
#undef VK_PHYSICAL_DEVICE_LIMITS_U64_FIELDS
#undef VK_PHYSICAL_DEVICE_LIMITS_U32_FIELDS

void write_PhysicalDeviceMemoryProperties(Writer& w, const VkPhysicalDeviceMemoryProperties& s) {
    w.u32(s.memoryTypeCount);
    for (uint32_t i = 0; i < s.memoryTypeCount; ++i) {
        w.u32(s.memoryTypes[i].propertyFlags);
        w.u32(s.memoryTypes[i].heapIndex);
    }
    w.u32(s.memoryHeapCount);
    for (uint32_t i = 0; i < s.memoryHeapCount; ++i) {
        w.u64(s.memoryHeaps[i].size);
        w.u32(s.memoryHeaps[i].flags);
    }
}

bool read_PhysicalDeviceMemoryProperties(Reader& r, VkPhysicalDeviceMemoryProperties* out) {
    *out = {};
    const uint32_t type_count = r.u32();
    if (!r.ok() || type_count > VK_MAX_MEMORY_TYPES ||
        type_count > r.remaining() / (2 * sizeof(uint32_t))) return false;
    out->memoryTypeCount = type_count;
    for (uint32_t i = 0; i < type_count; ++i) {
        out->memoryTypes[i].propertyFlags = r.u32();
        out->memoryTypes[i].heapIndex = r.u32();
    }
    const uint32_t heap_count = r.u32();
    if (!r.ok() || heap_count > VK_MAX_MEMORY_HEAPS ||
        heap_count > r.remaining() / (sizeof(uint64_t) + sizeof(uint32_t))) return false;
    out->memoryHeapCount = heap_count;
    for (uint32_t i = 0; i < heap_count; ++i) {
        out->memoryHeaps[i].size = r.u64();
        out->memoryHeaps[i].flags = r.u32();
    }
    return r.ok();
}

void write_PhysicalDeviceFeatures(Writer& w, const VkPhysicalDeviceFeatures& s) {
    const VkBool32* fields = reinterpret_cast<const VkBool32*>(&s);
    static_assert(sizeof(s) % sizeof(VkBool32) == 0, "features must contain VkBool32 fields");
    for (size_t i = 0; i < sizeof(s) / sizeof(VkBool32); ++i) w.u32(fields[i]);
}

bool read_PhysicalDeviceFeatures(Reader& r, VkPhysicalDeviceFeatures* out) {
    *out = {};
    VkBool32* fields = reinterpret_cast<VkBool32*>(out);
    for (size_t i = 0; i < sizeof(*out) / sizeof(VkBool32); ++i) fields[i] = r.u32();
    return r.ok();
}

void write_FormatProperties(Writer& w, const VkFormatProperties& s) {
    w.u32(s.linearTilingFeatures);
    w.u32(s.optimalTilingFeatures);
    w.u32(s.bufferFeatures);
}

bool read_FormatProperties(Reader& r, VkFormatProperties* out) {
    *out = {};
    out->linearTilingFeatures = r.u32();
    out->optimalTilingFeatures = r.u32();
    out->bufferFeatures = r.u32();
    return r.ok();
}

void write_ImageFormatProperties(Writer& w, const VkImageFormatProperties& s) {
    w.u32(s.maxExtent.width); w.u32(s.maxExtent.height); w.u32(s.maxExtent.depth);
    w.u32(s.maxMipLevels); w.u32(s.maxArrayLayers); w.u32(s.sampleCounts);
    w.u64(s.maxResourceSize);
}

bool read_ImageFormatProperties(Reader& r, VkImageFormatProperties* out) {
    *out = {};
    out->maxExtent.width = r.u32(); out->maxExtent.height = r.u32(); out->maxExtent.depth = r.u32();
    out->maxMipLevels = r.u32(); out->maxArrayLayers = r.u32();
    out->sampleCounts = r.u32(); out->maxResourceSize = r.u64();
    return r.ok();
}

void write_QueueFamilyProperties(Writer& w, const VkQueueFamilyProperties& s) {
    w.u32(s.queueFlags); w.u32(s.queueCount); w.u32(s.timestampValidBits);
    w.u32(s.minImageTransferGranularity.width); w.u32(s.minImageTransferGranularity.height);
    w.u32(s.minImageTransferGranularity.depth);
}

bool read_QueueFamilyProperties(Reader& r, VkQueueFamilyProperties* out) {
    *out = {};
    out->queueFlags = r.u32(); out->queueCount = r.u32(); out->timestampValidBits = r.u32();
    out->minImageTransferGranularity.width = r.u32();
    out->minImageTransferGranularity.height = r.u32();
    out->minImageTransferGranularity.depth = r.u32();
    return r.ok();
}

void write_SparseImageFormatProperties(Writer& w, const VkSparseImageFormatProperties& s) {
    w.u32(s.aspectMask); w.u32(s.imageGranularity.width); w.u32(s.imageGranularity.height);
    w.u32(s.imageGranularity.depth); w.u32(s.flags);
}

bool read_SparseImageFormatProperties(Reader& r, VkSparseImageFormatProperties* out) {
    *out = {};
    out->aspectMask = r.u32(); out->imageGranularity.width = r.u32();
    out->imageGranularity.height = r.u32(); out->imageGranularity.depth = r.u32();
    out->flags = r.u32();
    return r.ok();
}

void write_ExtensionProperties(Writer& w, const VkExtensionProperties& s) {
    w.string(s.extensionName);
    w.u32(s.specVersion);
}

bool read_ExtensionProperties(Reader& r, Arena&, const HandleResolver&,
                               VkExtensionProperties* out) {
    std::string name;
    if (!r.string(&name) || name.size() >= VK_MAX_EXTENSION_NAME_SIZE) return false;
    *out = {};
    std::memcpy(out->extensionName, name.data(), name.size());
    out->specVersion = r.u32();
    return r.ok();
}

}  // namespace remoting
