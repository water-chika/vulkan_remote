#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "extension_contract.hpp"
#include "marshal.hpp"
#include "objects.hpp"

namespace {

struct NullResolver : remoting::HandleResolver {
#define NULL_HANDLE(name, type) \
    type name(uint64_t) const override { return static_cast<type>(0); }
    NULL_HANDLE(buffer, VkBuffer)
    NULL_HANDLE(image, VkImage)
    NULL_HANDLE(image_view, VkImageView)
    NULL_HANDLE(buffer_view, VkBufferView)
    NULL_HANDLE(memory, VkDeviceMemory)
    NULL_HANDLE(render_pass, VkRenderPass)
    NULL_HANDLE(framebuffer, VkFramebuffer)
    NULL_HANDLE(shader_module, VkShaderModule)
    NULL_HANDLE(pipeline, VkPipeline)
    NULL_HANDLE(pipeline_layout, VkPipelineLayout)
    NULL_HANDLE(pipeline_cache, VkPipelineCache)
    NULL_HANDLE(descriptor_set_layout, VkDescriptorSetLayout)
    NULL_HANDLE(descriptor_pool, VkDescriptorPool)
    NULL_HANDLE(descriptor_set, VkDescriptorSet)
    NULL_HANDLE(sampler, VkSampler)
    NULL_HANDLE(sampler_ycbcr_conversion, VkSamplerYcbcrConversion)
    NULL_HANDLE(semaphore, VkSemaphore)
    NULL_HANDLE(fence, VkFence)
    NULL_HANDLE(event, VkEvent)
    NULL_HANDLE(query_pool, VkQueryPool)
    NULL_HANDLE(surface, VkSurfaceKHR)
    NULL_HANDLE(swapchain, VkSwapchainKHR)
    NULL_HANDLE(command_pool, VkCommandPool)
    NULL_HANDLE(command_buffer, VkCommandBuffer)
#undef NULL_HANDLE
};

void test_unknown_node_is_rejected_transactionally() {
    VkApplicationInfo unknown{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    remoting::Writer writer;
    writer.u32(0x12345678);
    const size_t original_size = writer.data().size();
    assert(!remoting::write_pnext_chain(
        writer, remoting::PNextContext::QueryFeaturesOutput, &unknown));
    assert(writer.data().size() == original_size);
}

void test_duplicate_node_is_rejected_transactionally() {
    VkPhysicalDeviceMultiviewFeatures second{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceMultiviewFeatures first{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, &second};
    remoting::Writer writer;
    assert(!remoting::write_pnext_chain(
        writer, remoting::PNextContext::QueryFeaturesOutput, &first));
    assert(writer.data().empty());
}

void test_truncated_node_is_rejected() {
    remoting::Writer writer;
    writer.u32(1);
    writer.i32(static_cast<int32_t>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES));
    remoting::Reader reader(writer.data().data(), writer.data().size());
    remoting::Arena arena;
    NullResolver resolver;
    const void* head = nullptr;
    assert(!remoting::read_pnext_chain(
        reader, arena, resolver, remoting::PNextContext::QueryFeaturesOutput,
        &head));
}

void test_excessive_node_count_is_rejected_before_allocation() {
    remoting::Writer writer;
    writer.u32(remoting::kMaxPNextNodes + 1);
    remoting::Reader reader(writer.data().data(), writer.data().size());
    remoting::Arena arena;
    NullResolver resolver;
    const void* head = nullptr;
    assert(!remoting::read_pnext_chain(
        reader, arena, resolver, remoting::PNextContext::QueryFeaturesOutput,
        &head));
    assert(head == nullptr);
}

void test_output_chain_round_trip_preserves_topology() {
    VkPhysicalDeviceMultiviewFeatures multiview{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
        &multiview};
    remoting::Writer topology;
    assert(remoting::write_pnext_output_topology(
        topology, remoting::PNextContext::QueryFeaturesOutput, &fsr));

    remoting::Reader topology_reader(topology.data().data(), topology.data().size());
    remoting::Arena arena;
    void* server_chain = nullptr;
    assert(remoting::read_pnext_output_topology(
        topology_reader, arena, remoting::PNextContext::QueryFeaturesOutput,
        &server_chain));
    auto* server_fsr = static_cast<VkPhysicalDeviceFragmentShadingRateFeaturesKHR*>(server_chain);
    auto* server_multiview = static_cast<VkPhysicalDeviceMultiviewFeatures*>(server_fsr->pNext);
    server_fsr->pipelineFragmentShadingRate = VK_TRUE;
    server_fsr->attachmentFragmentShadingRate = VK_TRUE;
    server_multiview->multiview = VK_TRUE;

    remoting::Writer values;
    assert(remoting::write_pnext_output_values(
        values, remoting::PNextContext::QueryFeaturesOutput, server_chain));
    void* original_next = fsr.pNext;
    remoting::Reader values_reader(values.data().data(), values.data().size());
    assert(remoting::apply_pnext_output_values(
        values_reader, remoting::PNextContext::QueryFeaturesOutput, &fsr));
    assert(fsr.pNext == original_next);
    assert(fsr.pipelineFragmentShadingRate == VK_TRUE);
    assert(fsr.attachmentFragmentShadingRate == VK_TRUE);
    assert(multiview.multiview == VK_TRUE);
}

void test_device_features2_chain_round_trip() {
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    fsr.pipelineFragmentShadingRate = VK_TRUE;
    fsr.attachmentFragmentShadingRate = VK_TRUE;
    VkPhysicalDeviceFeatures2 features2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fsr};
    features2.features.fragmentStoresAndAtomics = VK_TRUE;

    remoting::Writer writer;
    assert(remoting::write_pnext_chain(
        writer, remoting::PNextContext::DeviceCreate, &features2));
    remoting::Reader reader(writer.data().data(), writer.data().size());
    remoting::Arena arena;
    NullResolver resolver;
    const void* chain = nullptr;
    assert(remoting::read_pnext_chain(
        reader, arena, resolver, remoting::PNextContext::DeviceCreate, &chain));
    auto* decoded_features2 = static_cast<const VkPhysicalDeviceFeatures2*>(chain);
    assert(decoded_features2->features.fragmentStoresAndAtomics == VK_TRUE);
    auto* decoded_fsr = static_cast<const VkPhysicalDeviceFragmentShadingRateFeaturesKHR*>(
        decoded_features2->pNext);
    assert(decoded_fsr->pipelineFragmentShadingRate == VK_TRUE);
    assert(decoded_fsr->attachmentFragmentShadingRate == VK_TRUE);
}

void test_renderpass2_graph_round_trip() {
    VkAttachmentReference2 rate_ref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    rate_ref.attachment = 1;
    rate_ref.layout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    VkFragmentShadingRateAttachmentInfoKHR fsr{
        VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR, nullptr,
        &rate_ref, {8, 8}};
    VkAttachmentReference2 color{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    color.attachment = 0; color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    subpass.pNext = &fsr; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
    VkAttachmentDescription2 attachments[2]{{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2},
                                             {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2}};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM; attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].format = VK_FORMAT_R8_UINT; attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    VkSubpassDependency2 dependency{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0;
    uint32_t masks[] = {3};
    VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    info.attachmentCount=2;info.pAttachments=attachments;info.subpassCount=1;info.pSubpasses=&subpass;
    info.dependencyCount=1;info.pDependencies=&dependency;info.correlatedViewMaskCount=1;info.pCorrelatedViewMasks=masks;
    remoting::Writer writer; assert(remoting::write_RenderPassCreateInfo2(writer,info));
    remoting::Reader reader(writer.data().data(),writer.data().size());remoting::Arena arena;NullResolver resolver;VkRenderPassCreateInfo2 decoded{};
    assert(remoting::read_RenderPassCreateInfo2(reader,arena,resolver,&decoded));
    assert(decoded.attachmentCount==2&&decoded.subpassCount==1&&decoded.dependencyCount==1);
    auto* decoded_fsr=static_cast<const VkFragmentShadingRateAttachmentInfoKHR*>(decoded.pSubpasses[0].pNext);
    assert(decoded_fsr&&decoded_fsr->pFragmentShadingRateAttachment->attachment==1);
    assert(decoded_fsr->shadingRateAttachmentTexelSize.width==8);
}

void test_renderpass2_rejects_hostile_nested_count() {
    VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=remoting::kMaxArrayElements+1;
    VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    info.subpassCount=1;info.pSubpasses=&subpass;
    remoting::Writer writer;assert(!remoting::write_RenderPassCreateInfo2(writer,info));assert(writer.data().empty());
}

struct ViewResolver final : NullResolver {
    VkImageView image_view(uint64_t id) const override {
        return reinterpret_cast<VkImageView>(static_cast<uintptr_t>(id + 100));
    }
};

struct DescriptorResolver final : NullResolver {
    VkBuffer buffer(uint64_t id) const override {
        return reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(id + 100));
    }
    VkBufferView buffer_view(uint64_t id) const override {
        return reinterpret_cast<VkBufferView>(static_cast<uintptr_t>(id + 200));
    }
    VkDescriptorSet descriptor_set(uint64_t id) const override {
        return reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(id + 300));
    }
};

struct GraphicsResolver final : NullResolver {
    VkShaderModule shader_module(uint64_t id) const override {
        return reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(id + 100));
    }
    VkPipelineLayout pipeline_layout(uint64_t id) const override {
        return reinterpret_cast<VkPipelineLayout>(static_cast<uintptr_t>(id + 200));
    }
    VkRenderPass render_pass(uint64_t id) const override {
        return reinterpret_cast<VkRenderPass>(static_cast<uintptr_t>(id + 300));
    }
    VkPipeline pipeline(uint64_t id) const override {
        return reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(id + 400));
    }
};

void test_image_view_and_framebuffer_chains() {
    VkFormat formats[] = {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8B8A8_UNORM};
    VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    format_list.viewFormatCount = 2; format_list.pViewFormats = formats;
    remoting::Writer image_wire;
    assert(remoting::write_pnext_chain(image_wire, remoting::PNextContext::ImageCreate, &format_list));
    remoting::Reader image_reader(image_wire.data().data(), image_wire.data().size());
    remoting::Arena image_arena; NullResolver null_resolver; const void* image_chain=nullptr;
    assert(remoting::read_pnext_chain(image_reader,image_arena,null_resolver,remoting::PNextContext::ImageCreate,&image_chain));
    auto* decoded_formats=static_cast<const VkImageFormatListCreateInfo*>(image_chain);
    assert(decoded_formats->viewFormatCount==2&&decoded_formats->pViewFormats[1]==VK_FORMAT_R8G8B8A8_UNORM);

    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};usage.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    remoting::Writer view_wire;assert(remoting::write_pnext_chain(view_wire,remoting::PNextContext::ImageViewCreate,&usage));
    remoting::Reader view_reader(view_wire.data().data(),view_wire.data().size());remoting::Arena view_arena;const void* view_chain=nullptr;
    assert(remoting::read_pnext_chain(view_reader,view_arena,null_resolver,remoting::PNextContext::ImageViewCreate,&view_chain));
    assert(static_cast<const VkImageViewUsageCreateInfo*>(view_chain)->usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    VkFramebufferAttachmentImageInfo attachment{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO};
    attachment.width=64;attachment.height=32;attachment.layerCount=1;attachment.viewFormatCount=2;attachment.pViewFormats=formats;
    VkFramebufferAttachmentsCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO};
    framebuffer.attachmentImageInfoCount=1;framebuffer.pAttachmentImageInfos=&attachment;
    remoting::Writer fb_wire;assert(remoting::write_pnext_chain(fb_wire,remoting::PNextContext::FramebufferCreate,&framebuffer));
    remoting::Reader fb_reader(fb_wire.data().data(),fb_wire.data().size());remoting::Arena fb_arena;const void* fb_chain=nullptr;
    assert(remoting::read_pnext_chain(fb_reader,fb_arena,null_resolver,remoting::PNextContext::FramebufferCreate,&fb_chain));
    auto* decoded_fb=static_cast<const VkFramebufferAttachmentsCreateInfo*>(fb_chain);
    assert(decoded_fb->pAttachmentImageInfos[0].width==64&&decoded_fb->pAttachmentImageInfos[0].viewFormatCount==2);
}

void test_shader_module_round_trip_preserves_spirv_words() {
    // Minimal complete compute modules; only the version word differs. Keeping
    // real module bodies here catches accidental byte/word reinterpretation
    // while exercising both the Vulkan 1.0 and SPIR-V 1.5 headers.
    const uint32_t spirv10[] = {
        0x07230203, 0x00010000, 0, 5, 0,
        0x00020011, 1,
        0x0003000e, 0, 1,
        0x0005000f, 5, 1, 0x6e69616d, 0,
        0x00060010, 1, 17, 1, 1, 1,
        0x00020013, 2,
        0x00030021, 3, 2,
        0x00050036, 2, 1, 0, 3,
        0x000200f8, 4,
        0x000100fd,
        0x00010038,
    };
    const uint32_t spirv15[] = {
        0x07230203, 0x00010500, 0, 5, 0,
        0x00020011, 1,
        0x0003000e, 0, 1,
        0x0005000f, 5, 1, 0x6e69616d, 0,
        0x00060010, 1, 17, 1, 1, 1,
        0x00020013, 2,
        0x00030021, 3, 2,
        0x00050036, 2, 1, 0, 3,
        0x000200f8, 4,
        0x000100fd,
        0x00010038,
    };
    NullResolver resolver;
    for (const auto& words : {std::pair{spirv10, sizeof(spirv10)},
                              std::pair{spirv15, sizeof(spirv15)}}) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.flags = static_cast<VkShaderModuleCreateFlags>(0x1234);
        info.codeSize = words.second;
        info.pCode = words.first;
        remoting::Writer writer;
        assert(remoting::write_ShaderModuleCreateInfo(writer, info));
        remoting::Reader reader(writer.data().data(), writer.data().size());
        remoting::Arena arena;
        VkShaderModuleCreateInfo decoded{};
        assert(remoting::read_ShaderModuleCreateInfo(reader, arena, resolver, &decoded));
        assert(reader.remaining() == 0);
        assert(decoded.sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        assert(decoded.flags == info.flags);
        assert(decoded.pNext == nullptr);
        assert(decoded.codeSize == info.codeSize);
        assert(reinterpret_cast<uintptr_t>(decoded.pCode) % alignof(uint32_t) == 0);
        assert(std::memcmp(decoded.pCode, info.pCode, info.codeSize) == 0);
        assert(decoded.pCode[0] == 0x07230203);
        assert(decoded.pCode[1] == info.pCode[1]);
    }
}

void test_shader_module_marshal_rejections_are_transactional() {
    const uint32_t code[] = {0x07230203, 0x00010000, 0, 1, 0};
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = sizeof(code);
    info.pCode = code;

    const auto rejected = [&](const VkShaderModuleCreateInfo& candidate) {
        remoting::Writer writer;
        writer.u32(0x12345678);
        const auto before = writer.data();
        assert(!remoting::write_ShaderModuleCreateInfo(writer, candidate));
        assert(writer.data() == before);
    };

    auto invalid = info;
    invalid.pCode = nullptr;
    rejected(invalid);
    invalid = info;
    invalid.codeSize = 0;
    rejected(invalid);
    invalid = info;
    invalid.codeSize = sizeof(code) - 1;
    rejected(invalid);
    invalid = info;
    invalid.codeSize = remoting::kMaxShaderCodeBytes + sizeof(uint32_t);
    rejected(invalid);
    invalid = info;
    invalid.pCode = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const char*>(code) + 1);
    rejected(invalid);
    VkApplicationInfo unsupported{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    invalid = info;
    invalid.pNext = &unsupported;
    rejected(invalid);
}

void test_shader_module_malformed_wire_is_rejected() {
    NullResolver resolver;
    const auto rejected = [&](const remoting::Writer& writer) {
        remoting::Reader reader(writer.data().data(), writer.data().size());
        remoting::Arena arena;
        VkShaderModuleCreateInfo decoded{};
        assert(!remoting::read_ShaderModuleCreateInfo(reader, arena, resolver, &decoded));
        assert(decoded.pCode == nullptr);
    };
    const auto prefix = [](remoting::Writer* writer, uint32_t pnext_count = 0) {
        writer->i32(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        writer->u32(0);
        writer->u32(pnext_count);
    };

    remoting::Writer zero;
    prefix(&zero);
    zero.u64(0);
    rejected(zero);

    remoting::Writer non_word;
    prefix(&non_word);
    non_word.u64(5);
    non_word.u32(0x07230203);
    non_word.u32(0);
    rejected(non_word);

    remoting::Writer oversized;
    prefix(&oversized);
    oversized.u64(remoting::kMaxShaderCodeBytes + sizeof(uint32_t));
    rejected(oversized);

    remoting::Writer mismatch;
    prefix(&mismatch);
    mismatch.u64(8);
    mismatch.u32(0x07230203);
    rejected(mismatch);

    remoting::Writer truncated;
    prefix(&truncated);
    truncated.u64(4);
    truncated.u32(0x07230203);
    auto truncated_bytes = truncated.data();
    truncated_bytes.pop_back();
    remoting::Reader truncated_reader(truncated_bytes.data(), truncated_bytes.size());
    remoting::Arena truncated_arena;
    VkShaderModuleCreateInfo truncated_decoded{};
    assert(!remoting::read_ShaderModuleCreateInfo(
        truncated_reader, truncated_arena, resolver, &truncated_decoded));
    assert(truncated_decoded.pCode == nullptr);

    remoting::Writer trailing;
    prefix(&trailing);
    trailing.u64(4);
    trailing.u32(0x07230203);
    trailing.u32(0xdeadbeef);
    rejected(trailing);

    remoting::Writer unsupported_pnext;
    prefix(&unsupported_pnext, 1);
    unsupported_pnext.i32(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    unsupported_pnext.u64(4);
    unsupported_pnext.u32(0x07230203);
    rejected(unsupported_pnext);
}

void test_graphics_pipeline_nested_bounds_and_handles() {
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.module = reinterpret_cast<VkShaderModule>(7);
    stage.pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 1;
    info.pStages = &stage;
    info.pVertexInputState = &vertex;
    info.pViewportState = &viewport;
    info.pMultisampleState = &multisample;
    info.layout = reinterpret_cast<VkPipelineLayout>(11);
    info.renderPass = reinterpret_cast<VkRenderPass>(13);
    info.basePipelineHandle = reinterpret_cast<VkPipeline>(17);

    remoting::Writer writer;
    assert(remoting::write_GraphicsPipelineCreateInfo(writer, info));
    GraphicsResolver resolver;
    remoting::Reader reader(writer.data().data(), writer.data().size());
    remoting::Arena arena;
    VkGraphicsPipelineCreateInfo decoded{};
    assert(remoting::read_GraphicsPipelineCreateInfo(reader, arena, resolver, &decoded));
    assert(reader.remaining() == 0);
    assert(decoded.pStages[0].module == reinterpret_cast<VkShaderModule>(107));
    assert(decoded.layout == reinterpret_cast<VkPipelineLayout>(211));
    assert(decoded.renderPass == reinterpret_cast<VkRenderPass>(313));
    assert(decoded.basePipelineHandle == reinterpret_cast<VkPipeline>(417));

    NullResolver missing;
    remoting::Reader missing_reader(writer.data().data(), writer.data().size());
    remoting::Arena missing_arena;
    VkGraphicsPipelineCreateInfo missing_decoded{};
    assert(!remoting::read_GraphicsPipelineCreateInfo(
        missing_reader, missing_arena, missing, &missing_decoded));

    viewport.viewportCount = remoting::kMaxArrayElements + 1;
    remoting::Writer excessive_viewports;
    assert(!remoting::write_GraphicsPipelineCreateInfo(excessive_viewports, info));
    assert(excessive_viewports.data().empty());
    viewport.viewportCount = 1;

    multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(0xffffffffu);
    remoting::Writer invalid_samples;
    assert(!remoting::write_GraphicsPipelineCreateInfo(invalid_samples, info));
    assert(invalid_samples.data().empty());
}

void test_write_descriptor_set_round_trip_and_rejections() {
    VkDescriptorBufferInfo buffer_info{reinterpret_cast<VkBuffer>(7), 16, 64};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = reinterpret_cast<VkDescriptorSet>(11);
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_info;

    remoting::Writer writer;
    assert(remoting::write_WriteDescriptorSet(writer, write));
    DescriptorResolver resolver;
    remoting::Reader reader(writer.data().data(), writer.data().size());
    remoting::Arena arena;
    VkWriteDescriptorSet decoded{};
    assert(remoting::read_WriteDescriptorSet(reader, arena, resolver, &decoded));
    assert(reader.remaining() == 0);
    assert(decoded.dstSet == reinterpret_cast<VkDescriptorSet>(311));
    assert(decoded.pBufferInfo[0].buffer == reinterpret_cast<VkBuffer>(107));
    assert(decoded.pBufferInfo[0].offset == 16 && decoded.pBufferInfo[0].range == 64);

    const auto write_rejected = [](const VkWriteDescriptorSet& candidate) {
        remoting::Writer out;
        out.u32(0x12345678);
        const auto before = out.data();
        assert(!remoting::write_WriteDescriptorSet(out, candidate));
        assert(out.data() == before);
    };
    auto invalid = write;
    invalid.descriptorCount = remoting::kMaxArrayElements + 1;
    write_rejected(invalid);
    invalid = write;
    invalid.pBufferInfo = nullptr;
    write_rejected(invalid);
    invalid = write;
    invalid.pBufferInfo = &buffer_info;
    invalid.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write_rejected(invalid);
    VkDescriptorBufferInfo null_buffer{};
    invalid = write;
    invalid.pBufferInfo = &null_buffer;
    write_rejected(invalid);

    const auto read_rejected = [&](const std::vector<char>& wire) {
        remoting::Reader bad_reader(wire.data(), wire.size());
        remoting::Arena bad_arena;
        VkWriteDescriptorSet bad{};
        assert(!remoting::read_WriteDescriptorSet(bad_reader, bad_arena, resolver, &bad));
    };
    auto truncated = writer.data();
    truncated.pop_back();
    read_rejected(truncated);

    auto wrong_kind = writer.data();
    const size_t kind_offset = sizeof(uint32_t) + sizeof(VkWriteDescriptorSet);
    assert(kind_offset + sizeof(uint32_t) <= wrong_kind.size());
    const uint32_t image_kind = 1;
    std::memcpy(wrong_kind.data() + kind_offset, &image_kind, sizeof(image_kind));
    read_rejected(wrong_kind);

    auto invalid_set = writer.data();
    const uint64_t missing_set = 0;
    const size_t raw_offset = sizeof(uint32_t);
    const size_t set_offset = raw_offset + offsetof(VkWriteDescriptorSet, dstSet);
    std::memcpy(invalid_set.data() + set_offset, &missing_set, sizeof(missing_set));
    read_rejected(invalid_set);

    auto invalid_buffer = writer.data();
    const size_t buffer_offset = kind_offset + sizeof(uint32_t);
    const uint64_t missing_buffer = 0;
    std::memcpy(invalid_buffer.data() + buffer_offset, &missing_buffer, sizeof(missing_buffer));
    read_rejected(invalid_buffer);
}

void test_renderpass_begin_attachment_handles() {
    VkImageView views[]{reinterpret_cast<VkImageView>(7),reinterpret_cast<VkImageView>(9)};
    VkRenderPassAttachmentBeginInfo attachments{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
    attachments.attachmentCount=2;attachments.pAttachments=views;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.pNext=&attachments;
    remoting::Writer writer;assert(remoting::write_RenderPassBeginInfo(writer,begin));
    remoting::Reader reader(writer.data().data(),writer.data().size());remoting::Arena arena;ViewResolver resolver;VkRenderPassBeginInfo decoded{};
    assert(remoting::read_RenderPassBeginInfo(reader,arena,resolver,&decoded));
    auto* result=static_cast<const VkRenderPassAttachmentBeginInfo*>(decoded.pNext);
    assert(result->attachmentCount==2);
    assert(result->pAttachments[0]==reinterpret_cast<VkImageView>(107));
    assert(result->pAttachments[1]==reinterpret_cast<VkImageView>(109));
}

void test_proc_address_extension_contract() {
    const char* renderpass2_aliases[] = {
        "vkCreateRenderPass2KHR", "vkCmdBeginRenderPass2KHR",
        "vkCmdNextSubpass2KHR", "vkCmdEndRenderPass2KHR",
    };
    for (const char* command : renderpass2_aliases)
        assert(std::strcmp(remoting::device_extension_for_command(command),
                           VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) == 0);
    const char* renderpass2_core[] = {
        "vkCreateRenderPass2", "vkCmdBeginRenderPass2",
        "vkCmdNextSubpass2", "vkCmdEndRenderPass2",
    };
    for (const char* command : renderpass2_core)
        assert(remoting::device_extension_for_command(command) == nullptr);
    assert(std::strcmp(remoting::device_extension_for_command(
                           "vkCmdSetFragmentShadingRateKHR"),
                       VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0);
    assert(remoting::is_properties2_command("vkGetPhysicalDeviceFeatures2"));
    assert(remoting::is_properties2_command("vkGetPhysicalDeviceFeatures2KHR"));
    assert(!remoting::is_properties2_command("vkCmdBeginRenderingKHR"));
    assert(remoting::device_extension_for_command("vkCmdBeginRenderingKHR") == nullptr);
}

void test_device_extension_dependency_contract() {
    const char* enabled_names[] = {
        VK_KHR_MULTIVIEW_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
    };
    const auto enabled = [&](const char* name) {
        for (const char* candidate : enabled_names)
            if (std::strcmp(candidate, name) == 0) return true;
        return false;
    };
    const auto missing = [&](const char*) { return false; };
    assert(remoting::device_extension_dependencies_satisfied(
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, enabled, true));
    assert(remoting::device_extension_dependencies_satisfied(
        VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, enabled, true));
    assert(remoting::device_extension_dependencies_satisfied(
        VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME, enabled, true));
    assert(!remoting::device_extension_dependencies_satisfied(
        VK_KHR_MULTIVIEW_EXTENSION_NAME, enabled, false));
    assert(!remoting::device_extension_dependencies_satisfied(
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, missing, true));
    assert(!remoting::device_extension_dependencies_satisfied(
        VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, missing, true));
    assert(!remoting::device_extension_dependencies_satisfied(
        VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, enabled, false));
    assert(!remoting::device_extension_dependencies_satisfied(
        VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME, missing, true));
    assert(!remoting::is_required_device_extension("VK_EXT_does_not_exist"));
    assert(remoting::device_extension_for_command("vkCmdBeginRenderingKHR") == nullptr);
}

void test_fsr_rate_count_capacity_and_incomplete() {
    VkPhysicalDeviceFragmentShadingRateKHR source[2]{
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR, nullptr,
         VK_SAMPLE_COUNT_1_BIT, {1, 1}},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR, nullptr,
         VK_SAMPLE_COUNT_2_BIT, {2, 2}},
    };
    uint32_t count = 0;
    assert(remoting::copy_fragment_shading_rates(source, 2, &count, nullptr) == VK_SUCCESS);
    assert(count == 2);

    VkPhysicalDeviceFragmentShadingRateKHR destination{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR};
    count = 1;
    assert(remoting::copy_fragment_shading_rates(source, 2, &count, &destination) ==
           VK_INCOMPLETE);
    assert(count == 1);
    assert(destination.sampleCounts == VK_SAMPLE_COUNT_1_BIT);
    assert(destination.fragmentSize.width == 1 && destination.fragmentSize.height == 1);

    VkPhysicalDeviceFragmentShadingRateKHR all[2]{
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR},
    };
    count = 2;
    assert(remoting::copy_fragment_shading_rates(source, 2, &count, all) == VK_SUCCESS);
    assert(count == 2 && all[1].fragmentSize.width == 2);
}

void test_excessive_nested_count_is_rejected() {
    remoting::Writer writer;
    writer.u32(1);
    writer.i32(static_cast<int32_t>(
        VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO));
    writer.u32(remoting::kMaxArrayElements + 1);
    remoting::Reader reader(writer.data().data(), writer.data().size());
    remoting::Arena arena;
    NullResolver resolver;
    const void* head = nullptr;
    assert(!remoting::read_pnext_chain(
        reader, arena, resolver, remoting::PNextContext::FramebufferCreate,
        &head));
}

void test_vulkan12_policy_and_timeline_round_trip() {
    VkPhysicalDeviceVulkan12Features f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f.hostQueryReset=VK_TRUE; f.timelineSemaphore=VK_TRUE; f.drawIndirectCount=VK_TRUE;
    f.bufferDeviceAddress=VK_TRUE; f.descriptorIndexing=VK_TRUE;
    remoting::Writer w; assert(remoting::write_pnext_chain(w,remoting::PNextContext::QueryFeaturesOutput,&f));
    remoting::Reader r(w.data().data(),w.data().size());remoting::Arena a;NullResolver resolver;const void* chain=nullptr;
    assert(remoting::read_pnext_chain(r,a,resolver,remoting::PNextContext::QueryFeaturesOutput,&chain));
    auto* out=static_cast<const VkPhysicalDeviceVulkan12Features*>(chain);
    assert(out->hostQueryReset&&out->timelineSemaphore&&out->drawIndirectCount);
    assert(!out->bufferDeviceAddress&&!out->bufferDeviceAddressCaptureReplay&&!out->descriptorIndexing);

    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;type.initialValue=7;
    remoting::Writer sw;assert(remoting::write_pnext_chain(sw,remoting::PNextContext::SemaphoreCreate,&type));
    remoting::Reader sr(sw.data().data(),sw.data().size());remoting::Arena sa;const void* schain=nullptr;
    assert(remoting::read_pnext_chain(sr,sa,resolver,remoting::PNextContext::SemaphoreCreate,&schain));
    auto* st=static_cast<const VkSemaphoreTypeCreateInfo*>(schain);assert(st->semaphoreType==VK_SEMAPHORE_TYPE_TIMELINE&&st->initialValue==7);

    uint64_t waits[]={3,4},signals[]={8};VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};ti.waitSemaphoreValueCount=2;ti.pWaitSemaphoreValues=waits;ti.signalSemaphoreValueCount=1;ti.pSignalSemaphoreValues=signals;
    remoting::Writer tw;assert(remoting::write_pnext_chain(tw,remoting::PNextContext::Submit,&ti));
    remoting::Reader tr(tw.data().data(),tw.data().size());remoting::Arena ta;const void* tchain=nullptr;
    assert(remoting::read_pnext_chain(tr,ta,resolver,remoting::PNextContext::Submit,&tchain));
    auto* to=static_cast<const VkTimelineSemaphoreSubmitInfo*>(tchain);assert(to->waitSemaphoreValueCount==2&&to->pWaitSemaphoreValues[1]==4&&to->pSignalSemaphoreValues[0]==8);
}

struct Sync2Resolver final : NullResolver {
    VkBuffer buffer(uint64_t id) const override {
        return id == 0x11 ? reinterpret_cast<VkBuffer>(uintptr_t{0x11}) : VK_NULL_HANDLE;
    }
    VkImage image(uint64_t id) const override {
        return id == 0x22 ? reinterpret_cast<VkImage>(uintptr_t{0x22}) : VK_NULL_HANDLE;
    }
    VkSemaphore semaphore(uint64_t id) const override {
        return id == 0x33 ? reinterpret_cast<VkSemaphore>(uintptr_t{0x33}) : VK_NULL_HANDLE;
    }
    VkCommandBuffer command_buffer(uint64_t id) const override {
        return id == 0x44 ? reinterpret_cast<VkCommandBuffer>(uintptr_t{0x44}) : VK_NULL_HANDLE;
    }
};

void test_synchronization2_dependency_info_wire_sizes() {
    VkBufferMemoryBarrier2 buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    buffer.srcStageMask = 1ull << 40;
    buffer.srcAccessMask = 1ull << 41;
    buffer.dstStageMask = 1ull << 42;
    buffer.dstAccessMask = 1ull << 43;
    buffer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer.dstQueueFamilyIndex = 0xdeadbeefu;
    buffer.buffer = reinterpret_cast<VkBuffer>(uintptr_t{0x11});
    buffer.offset = 7;
    buffer.size = 19;
    VkImageMemoryBarrier2 image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    image.srcStageMask = 1ull << 44;
    image.dstStageMask = 1ull << 45;
    image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image.dstQueueFamilyIndex = 3;
    image.image = reinterpret_cast<VkImage>(uintptr_t{0x22});
    image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 3, 4, 5};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    info.bufferMemoryBarrierCount = 1;
    info.pBufferMemoryBarriers = &buffer;
    info.imageMemoryBarrierCount = 1;
    info.pImageMemoryBarriers = &image;
    remoting::Writer writer;
    assert(remoting::write_DependencyInfo(writer, info));
    Sync2Resolver resolver;
    remoting::Arena arena;
    remoting::Reader reader(writer.data().data(), writer.data().size());
    VkDependencyInfo decoded{};
    assert(remoting::read_DependencyInfo(reader, arena, resolver, &decoded));
    assert(reader.remaining() == 0);
    assert(decoded.pBufferMemoryBarriers[0].srcStageMask == buffer.srcStageMask);
    assert(decoded.pBufferMemoryBarriers[0].dstAccessMask == buffer.dstAccessMask);
    assert(decoded.pBufferMemoryBarriers[0].dstQueueFamilyIndex == 0xdeadbeefu);
    assert(decoded.pBufferMemoryBarriers[0].offset == 7);
    assert(decoded.pImageMemoryBarriers[0].oldLayout == image.oldLayout);
    assert(decoded.pImageMemoryBarriers[0].newLayout == image.newLayout);
    assert(decoded.pImageMemoryBarriers[0].subresourceRange.baseArrayLayer == 4);
    remoting::Reader truncated(writer.data().data(), writer.data().size() - 1);
    remoting::Arena truncated_arena;
    VkDependencyInfo rejected{};
    assert(!remoting::read_DependencyInfo(truncated, truncated_arena, resolver, &rejected));
}

void test_owned_table_ids_never_revive() {
    remoting::Table<VkBuffer> table;
    const VkDevice first_device = reinterpret_cast<VkDevice>(uintptr_t{0x1000});
    const VkDevice second_device = reinterpret_cast<VkDevice>(uintptr_t{0x2000});
    const VkBuffer first = reinterpret_cast<VkBuffer>(uintptr_t{0x3000});
    const VkBuffer second = reinterpret_cast<VkBuffer>(uintptr_t{0x4000});
    const uint64_t stale_id = table.add(first, first_device);
    assert(table.owned_by(stale_id, first_device));
    table.retire_owned(first_device);
    assert(table.get(stale_id) == VK_NULL_HANDLE);
    assert(!table.owned_by(stale_id, first_device));
    const uint64_t fresh_id = table.add(second, second_device);
    assert(fresh_id != stale_id);
    assert(table.get(stale_id) == VK_NULL_HANDLE);
    assert(table.get(fresh_id) == second);
}

}  // namespace

int main() {
    test_unknown_node_is_rejected_transactionally();
    test_duplicate_node_is_rejected_transactionally();
    test_truncated_node_is_rejected();
    test_excessive_node_count_is_rejected_before_allocation();
    test_output_chain_round_trip_preserves_topology();
    test_device_features2_chain_round_trip();
    test_renderpass2_graph_round_trip();
    test_renderpass2_rejects_hostile_nested_count();
    test_image_view_and_framebuffer_chains();
    test_shader_module_round_trip_preserves_spirv_words();
    test_shader_module_marshal_rejections_are_transactional();
    test_shader_module_malformed_wire_is_rejected();
    test_graphics_pipeline_nested_bounds_and_handles();
    test_write_descriptor_set_round_trip_and_rejections();
    test_renderpass_begin_attachment_handles();
    test_proc_address_extension_contract();
    test_device_extension_dependency_contract();
    test_fsr_rate_count_capacity_and_incomplete();
    test_excessive_nested_count_is_rejected();
    test_vulkan12_policy_and_timeline_round_trip();
    test_synchronization2_dependency_info_wire_sizes();
    test_owned_table_ids_never_revive();
    return 0;
}
