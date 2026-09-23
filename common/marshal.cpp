#include "marshal.hpp"

#include <cstring>

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
    if (!r.ok()) return false;
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

    patch_handle(&out->module, [&](uint64_t id) { return hr.shader_module(id); });
    return true;
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

void write_DeviceCreateInfo(Writer& w, const VkDeviceCreateInfo& s) {
    VkDeviceCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pQueueCreateInfos = nullptr;
    tmp.ppEnabledLayerNames = nullptr;
    tmp.ppEnabledExtensionNames = nullptr;
    tmp.pEnabledFeatures = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    for (uint32_t i = 0; i < s.queueCreateInfoCount; ++i)
        write_DeviceQueueCreateInfo(w, s.pQueueCreateInfos[i]);
    write_string_array(w, s.ppEnabledLayerNames, s.enabledLayerCount);
    write_string_array(w, s.ppEnabledExtensionNames, s.enabledExtensionCount);
    write_opt_pod(w, s.pEnabledFeatures);
}

bool read_DeviceCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                            VkDeviceCreateInfo* out) {
    if (!read_raw(r, out)) return false;
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

void write_ImageViewCreateInfo(Writer& w, const VkImageViewCreateInfo& s) {
    VkImageViewCreateInfo tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_ImageViewCreateInfo(Reader& r, Arena&, const HandleResolver& hr,
                               VkImageViewCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    patch_handle(&out->image, [&](uint64_t id) { return hr.image(id); });
    return true;
}

void write_RenderPassCreateInfo(Writer& w, const VkRenderPassCreateInfo& s) {
    VkRenderPassCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pAttachments = nullptr;
    tmp.pSubpasses = nullptr;
    tmp.pDependencies = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_pod_array(w, s.pAttachments, s.attachmentCount);
    for (uint32_t i = 0; i < s.subpassCount; ++i) write_SubpassDescription(w, s.pSubpasses[i]);
    write_pod_array(w, s.pDependencies, s.dependencyCount);
}

bool read_RenderPassCreateInfo(Reader& r, Arena& arena, const HandleResolver&,
                                VkRenderPassCreateInfo* out) {
    if (!read_raw(r, out)) return false;
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

void write_ShaderModuleCreateInfo(Writer& w, const VkShaderModuleCreateInfo& s) {
    VkShaderModuleCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pCode = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    w.bytes(s.pCode, s.codeSize);
}

bool read_ShaderModuleCreateInfo(Reader& r, Arena& arena, const HandleResolver&,
                                  VkShaderModuleCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    void* code = nullptr;
    if (!read_bytes_to_arena(r, arena, out->codeSize, &code)) return false;
    out->pCode = static_cast<uint32_t*>(code);
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

void write_GraphicsPipelineCreateInfo(Writer& w, const VkGraphicsPipelineCreateInfo& s) {
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
    w.bytes(&tmp, sizeof(tmp));

    for (uint32_t i = 0; i < s.stageCount; ++i)
        write_PipelineShaderStageCreateInfo(w, s.pStages[i]);

    w.u32(s.pVertexInputState ? 1 : 0);
    if (s.pVertexInputState) {
        const auto& vi = *s.pVertexInputState;
        VkPipelineVertexInputStateCreateInfo t = vi;
        t.pNext = nullptr;
        t.pVertexBindingDescriptions = nullptr;
        t.pVertexAttributeDescriptions = nullptr;
        w.bytes(&t, sizeof(t));
        write_pod_array(w, vi.pVertexBindingDescriptions, vi.vertexBindingDescriptionCount);
        write_pod_array(w, vi.pVertexAttributeDescriptions, vi.vertexAttributeDescriptionCount);
    }

    write_opt_pod(w, s.pInputAssemblyState);
    write_opt_pod(w, s.pTessellationState);

    w.u32(s.pViewportState ? 1 : 0);
    if (s.pViewportState) {
        const auto& vp = *s.pViewportState;
        VkPipelineViewportStateCreateInfo t = vp;
        t.pNext = nullptr;
        t.pViewports = nullptr;
        t.pScissors = nullptr;
        w.bytes(&t, sizeof(t));
        write_opt_pod_array(w, vp.pViewports, vp.viewportCount);
        write_opt_pod_array(w, vp.pScissors, vp.scissorCount);
    }

    write_opt_pod(w, s.pRasterizationState);

    w.u32(s.pMultisampleState ? 1 : 0);
    if (s.pMultisampleState) {
        const auto& ms = *s.pMultisampleState;
        VkPipelineMultisampleStateCreateInfo t = ms;
        t.pNext = nullptr;
        t.pSampleMask = nullptr;
        w.bytes(&t, sizeof(t));
        const uint32_t mask_words = (static_cast<uint32_t>(ms.rasterizationSamples) + 31) / 32;
        write_opt_pod_array(w, ms.pSampleMask, mask_words);
    }

    write_opt_pod(w, s.pDepthStencilState);

    w.u32(s.pColorBlendState ? 1 : 0);
    if (s.pColorBlendState) {
        const auto& cb = *s.pColorBlendState;
        VkPipelineColorBlendStateCreateInfo t = cb;
        t.pNext = nullptr;
        t.pAttachments = nullptr;
        w.bytes(&t, sizeof(t));
        write_pod_array(w, cb.pAttachments, cb.attachmentCount);
    }

    w.u32(s.pDynamicState ? 1 : 0);
    if (s.pDynamicState) {
        const auto& ds = *s.pDynamicState;
        VkPipelineDynamicStateCreateInfo t = ds;
        t.pNext = nullptr;
        t.pDynamicStates = nullptr;
        w.bytes(&t, sizeof(t));
        write_pod_array(w, ds.pDynamicStates, ds.dynamicStateCount);
    }
}

bool read_GraphicsPipelineCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                      VkGraphicsPipelineCreateInfo* out) {
    if (!read_raw(r, out)) return false;

    if (out->stageCount > kMaxArrayElements) return false;
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
        const uint32_t mask_words = (static_cast<uint32_t>(ms->rasterizationSamples) + 31) / 32;
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

    patch_handle(&out->layout, [&](uint64_t id) { return hr.pipeline_layout(id); });
    patch_handle(&out->renderPass, [&](uint64_t id) { return hr.render_pass(id); });
    patch_handle(&out->basePipelineHandle, [&](uint64_t id) { return hr.pipeline(id); });
    return true;
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

void write_FramebufferCreateInfo(Writer& w, const VkFramebufferCreateInfo& s) {
    VkFramebufferCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pAttachments = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_handle_array(w, s.pAttachments, s.attachmentCount);
}

bool read_FramebufferCreateInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                 VkFramebufferCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    if (!read_handle_array(
            r, arena, out->attachmentCount, [&](uint64_t id) { return hr.image_view(id); },
            const_cast<VkImageView**>(&out->pAttachments)))
        return false;
    patch_handle(&out->renderPass, [&](uint64_t id) { return hr.render_pass(id); });
    return true;
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
    VkCommandBufferBeginInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pInheritanceInfo = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_opt_pod(w, s.pInheritanceInfo);
}

bool read_CommandBufferBeginInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                                  VkCommandBufferBeginInfo* out) {
    if (!read_raw(r, out)) return false;
    VkCommandBufferInheritanceInfo* inh = nullptr;
    if (!read_opt_pod(r, arena, &inh)) return false;
    if (inh) {
        patch_handle(&inh->renderPass, [&](uint64_t id) { return hr.render_pass(id); });
        patch_handle(&inh->framebuffer, [&](uint64_t id) { return hr.framebuffer(id); });
    }
    out->pInheritanceInfo = inh;
    return true;
}

void write_RenderPassBeginInfo(Writer& w, const VkRenderPassBeginInfo& s) {
    VkRenderPassBeginInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pClearValues = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_pod_array(w, s.pClearValues, s.clearValueCount);
}

bool read_RenderPassBeginInfo(Reader& r, Arena& arena, const HandleResolver& hr,
                               VkRenderPassBeginInfo* out) {
    if (!read_raw(r, out)) return false;
    if (!read_pod_array(r, arena, out->clearValueCount,
                         const_cast<VkClearValue**>(&out->pClearValues)))
        return false;
    patch_handle(&out->renderPass, [&](uint64_t id) { return hr.render_pass(id); });
    patch_handle(&out->framebuffer, [&](uint64_t id) { return hr.framebuffer(id); });
    return true;
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
    if (out->bindingCount > kMaxArrayElements) return false;
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
    if (!read_raw(r, out)) return false;
    if (!read_handle_array(
            r, arena, out->descriptorSetCount,
            [&](uint64_t id) { return hr.descriptor_set_layout(id); },
            const_cast<VkDescriptorSetLayout**>(&out->pSetLayouts)))
        return false;
    patch_handle(&out->descriptorPool, [&](uint64_t id) { return hr.descriptor_pool(id); });
    return true;
}

void write_WriteDescriptorSet(Writer& w, const VkWriteDescriptorSet& s) {
    VkWriteDescriptorSet tmp = s;
    tmp.pNext = nullptr;
    tmp.pImageInfo = nullptr;
    tmp.pBufferInfo = nullptr;
    tmp.pTexelBufferView = nullptr;
    w.bytes(&tmp, sizeof(tmp));

    // Discriminator: which of the three arrays is meaningful for this
    // descriptorType, per the Vulkan spec's rules for VkWriteDescriptorSet.
    switch (s.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            w.u32(1);
            for (uint32_t i = 0; i < s.descriptorCount; ++i) {
                const VkDescriptorImageInfo& info = s.pImageInfo[i];
                w.handle(id_of(info.sampler));
                w.handle(id_of(info.imageView));
                w.i32(static_cast<int32_t>(info.imageLayout));
            }
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            w.u32(2);
            for (uint32_t i = 0; i < s.descriptorCount; ++i) {
                const VkDescriptorBufferInfo& info = s.pBufferInfo[i];
                w.handle(id_of(info.buffer));
                w.u64(info.offset);
                w.u64(info.range);
            }
            break;
        default:
            // VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER / STORAGE_TEXEL_BUFFER
            // need pTexelBufferView, which this project never uses.
            w.u32(0);
            break;
    }
}

bool read_WriteDescriptorSet(Reader& r, Arena& arena, const HandleResolver& hr,
                              VkWriteDescriptorSet* out) {
    if (!read_raw(r, out)) return false;
    if (out->descriptorCount > kMaxArrayElements) return false;

    const uint32_t kind = r.u32();
    if (!r.ok()) return false;

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
            infos[i].sampler = sampler_id ? hr.sampler(sampler_id) : static_cast<VkSampler>(0);
            infos[i].imageView =
                view_id ? hr.image_view(view_id) : static_cast<VkImageView>(0);
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
            infos[i].offset = offset;
            infos[i].range = range;
        }
        out->pBufferInfo = infos;
    } else if (kind != 0) {
        return false;
    }
    // kind == 0: pTexelBufferView, unsupported - descriptorCount elements
    // simply were not written, so there is nothing further to read.

    patch_handle(&out->dstSet, [&](uint64_t id) { return hr.descriptor_set(id); });
    return true;
}

void write_CopyDescriptorSet(Writer& w, const VkCopyDescriptorSet& s) {
    VkCopyDescriptorSet tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_CopyDescriptorSet(Reader& r, Arena&, const HandleResolver& hr,
                             VkCopyDescriptorSet* out) {
    if (!read_raw(r, out)) return false;
    patch_handle(&out->srcSet, [&](uint64_t id) { return hr.descriptor_set(id); });
    patch_handle(&out->dstSet, [&](uint64_t id) { return hr.descriptor_set(id); });
    return true;
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

// ---------------------------------------------------------------------------
// Group B
// ---------------------------------------------------------------------------

void write_BufferCreateInfo(Writer& w, const VkBufferCreateInfo& s) {
    VkBufferCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pQueueFamilyIndices = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_pod_array(w, s.pQueueFamilyIndices, s.queueFamilyIndexCount);
}

bool read_BufferCreateInfo(Reader& r, Arena& arena, const HandleResolver&,
                            VkBufferCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    return read_pod_array(r, arena, out->queueFamilyIndexCount,
                           const_cast<uint32_t**>(&out->pQueueFamilyIndices));
}

void write_ImageCreateInfo(Writer& w, const VkImageCreateInfo& s) {
    VkImageCreateInfo tmp = s;
    tmp.pNext = nullptr;
    tmp.pQueueFamilyIndices = nullptr;
    w.bytes(&tmp, sizeof(tmp));
    write_pod_array(w, s.pQueueFamilyIndices, s.queueFamilyIndexCount);
}

bool read_ImageCreateInfo(Reader& r, Arena& arena, const HandleResolver&, VkImageCreateInfo* out) {
    if (!read_raw(r, out)) return false;
    return read_pod_array(r, arena, out->queueFamilyIndexCount,
                           const_cast<uint32_t**>(&out->pQueueFamilyIndices));
}

void write_MemoryAllocateInfo(Writer& w, const VkMemoryAllocateInfo& s) {
    VkMemoryAllocateInfo tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_MemoryAllocateInfo(Reader& r, Arena&, const HandleResolver&,
                              VkMemoryAllocateInfo* out) {
    return read_raw(r, out);
}

void write_SamplerCreateInfo(Writer& w, const VkSamplerCreateInfo& s) {
    VkSamplerCreateInfo tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_SamplerCreateInfo(Reader& r, Arena&, const HandleResolver&, VkSamplerCreateInfo* out) {
    return read_raw(r, out);
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
    VkSemaphoreCreateInfo tmp = s;
    tmp.pNext = nullptr;
    w.bytes(&tmp, sizeof(tmp));
}

bool read_SemaphoreCreateInfo(Reader& r, Arena&, const HandleResolver&,
                               VkSemaphoreCreateInfo* out) {
    return read_raw(r, out);
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

void write_PhysicalDeviceProperties(Writer& w, const VkPhysicalDeviceProperties& s) {
    w.bytes(&s, sizeof(s));
}

bool read_PhysicalDeviceProperties(Reader& r, Arena&, const HandleResolver&,
                                    VkPhysicalDeviceProperties* out) {
    return read_raw(r, out);
}

void write_PhysicalDeviceMemoryProperties(Writer& w, const VkPhysicalDeviceMemoryProperties& s) {
    w.bytes(&s, sizeof(s));
}

bool read_PhysicalDeviceMemoryProperties(Reader& r, Arena&, const HandleResolver&,
                                          VkPhysicalDeviceMemoryProperties* out) {
    return read_raw(r, out);
}

void write_PhysicalDeviceFeatures(Writer& w, const VkPhysicalDeviceFeatures& s) {
    w.bytes(&s, sizeof(s));
}

bool read_PhysicalDeviceFeatures(Reader& r, Arena&, const HandleResolver&,
                                  VkPhysicalDeviceFeatures* out) {
    return read_raw(r, out);
}

void write_FormatProperties(Writer& w, const VkFormatProperties& s) { w.bytes(&s, sizeof(s)); }

bool read_FormatProperties(Reader& r, Arena&, const HandleResolver&, VkFormatProperties* out) {
    return read_raw(r, out);
}

void write_ExtensionProperties(Writer& w, const VkExtensionProperties& s) {
    w.bytes(&s, sizeof(s));
}

bool read_ExtensionProperties(Reader& r, Arena&, const HandleResolver&,
                               VkExtensionProperties* out) {
    return read_raw(r, out);
}

}  // namespace remoting
