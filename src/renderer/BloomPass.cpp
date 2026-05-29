#include "renderer/BloomPass.h"
#include "renderer/VulkanContext.h"
#include "Logger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace Nyx {

// Push constants for the bloom shaders: x/y = inverse source-texel size (for sample
// offsets), z = bloom strength (only used on the final upsample step into mip 0),
// w = mode flag (0 = brightpass extract, 1 = plain downsample, 2 = upsample-tent).
struct BloomPC {
    float invTexelX;
    float invTexelY;
    float threshold;   // brightpass cutoff (only used when mode == 0)
    float knee;        // soft-knee half-width
    float strength;    // carried for parity with composite
    float mode;
};

static std::vector<char> readSpv(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("Failed to open shader: " + path);
    size_t n = static_cast<size_t>(f.tellg());
    std::vector<char> buf(n);
    f.seekg(0); f.read(buf.data(), static_cast<std::streamsize>(n));
    return buf;
}

static VkShaderModule makeShader(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    if (vkCreateShaderModule(device, &info, nullptr, &mod) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shader module");
    return mod;
}

void BloomPass::init(VulkanContext& context, VkImageView hdrView, VkSampler /*hdrSampler*/,
                    VkExtent2D sceneExtent, uint32_t mipCount) {
    m_mipCount = std::min(mipCount, MAX_MIPS);
    m_extent   = {std::max(1u, sceneExtent.width  / 2),
                  std::max(1u, sceneExtent.height / 2)};

    createImageAndViews(context);
    createSampler(context.getDevice());
    createRenderPasses(context.getDevice());
    createFramebuffers(context.getDevice());
    createPipelineAndSets(context.getDevice());
    writeSourceSets(context.getDevice(), hdrView);
    LOG_INFO("BloomPass initialized ({} mips, base {}x{})", m_mipCount, m_extent.width, m_extent.height);
}

void BloomPass::cleanup(VkDevice device, VmaAllocator allocator) {
    destroy(device, allocator);
}

void BloomPass::resize(VulkanContext& context, VkImageView hdrView, VkExtent2D sceneExtent) {
    destroy(context.getDevice(), context.getAllocator());
    init(context, hdrView, VK_NULL_HANDLE, sceneExtent, m_mipCount == 0 ? 5 : m_mipCount);
}

void BloomPass::createImageAndViews(VulkanContext& context) {
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.extent        = {m_extent.width, m_extent.height, 1};
    info.mipLevels     = m_mipCount;
    info.arrayLayers   = 1;
    info.format        = m_format;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(context.getAllocator(), &info, &alloc, &m_image, &m_alloc, nullptr) != VK_SUCCESS)
        throw std::runtime_error("Failed to create bloom image");

    m_mipViews.resize(m_mipCount);
    m_mipExtents.resize(m_mipCount);
    for (uint32_t i = 0; i < m_mipCount; i++) {
        VkImageViewCreateInfo v{};
        v.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        v.image    = m_image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format   = m_format;
        v.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        v.subresourceRange.baseMipLevel   = i;
        v.subresourceRange.levelCount     = 1;
        v.subresourceRange.baseArrayLayer = 0;
        v.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(context.getDevice(), &v, nullptr, &m_mipViews[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create bloom mip view");

        m_mipExtents[i] = {
            std::max(1u, m_extent.width  >> i),
            std::max(1u, m_extent.height >> i),
        };
    }
}

void BloomPass::createSampler(VkDevice device) {
    VkSamplerCreateInfo s{};
    s.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    s.magFilter     = VK_FILTER_LINEAR;
    s.minFilter     = VK_FILTER_LINEAR;
    s.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.minLod        = 0.0f;
    s.maxLod        = 0.0f;
    if (vkCreateSampler(device, &s, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("Failed to create bloom sampler");
}

void BloomPass::createRenderPasses(VkDevice device) {
    auto makeRp = [&](VkAttachmentLoadOp loadOp, VkRenderPass& out) {
        VkAttachmentDescription a{};
        a.format         = m_format;
        a.samples        = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp         = loadOp;
        a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Always finish in SHADER_READ_ONLY so the next pass (downsample / upsample /
        // composite) can sample this mip directly. For the LOAD case the previous
        // pass left it in SHADER_READ_ONLY, so initialLayout matches.
        a.initialLayout  = (loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
                         ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         : VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        std::array<VkSubpassDependency, 2> deps{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rp{};
        rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 1;
        rp.pAttachments    = &a;
        rp.subpassCount    = 1;
        rp.pSubpasses      = &sub;
        rp.dependencyCount = static_cast<uint32_t>(deps.size());
        rp.pDependencies   = deps.data();
        if (vkCreateRenderPass(device, &rp, nullptr, &out) != VK_SUCCESS)
            throw std::runtime_error("Failed to create bloom render pass");
    };

    makeRp(VK_ATTACHMENT_LOAD_OP_DONT_CARE, m_downsampleRP);
    makeRp(VK_ATTACHMENT_LOAD_OP_LOAD,      m_upsampleRP);
}

void BloomPass::createFramebuffers(VkDevice device) {
    m_framebuffers.resize(m_mipCount);
    for (uint32_t i = 0; i < m_mipCount; i++) {
        // Each mip is used as a render target by EITHER pass — both render passes
        // are compatible (one color attachment, same format), so a framebuffer
        // created against m_downsampleRP can be used with m_upsampleRP too.
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = m_downsampleRP;
        fb.attachmentCount = 1;
        fb.pAttachments    = &m_mipViews[i];
        fb.width           = m_mipExtents[i].width;
        fb.height          = m_mipExtents[i].height;
        fb.layers          = 1;
        if (vkCreateFramebuffer(device, &fb, nullptr, &m_framebuffers[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create bloom framebuffer");
    }
}

void BloomPass::createPipelineAndSets(VkDevice device) {
    // Descriptor set layout: one combined image sampler.
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 1;
    dsl.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(device, &dsl, nullptr, &m_setLayout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create bloom set layout");

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(BloomPC);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &m_setLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pc;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create bloom pipeline layout");

    // Descriptor pool / sets — scene + N source mips = N+1 sets.
    {
        uint32_t count = m_mipCount + 1;
        VkDescriptorPoolSize sz{};
        sz.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sz.descriptorCount = count;
        VkDescriptorPoolCreateInfo pi{};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.poolSizeCount = 1;
        pi.pPoolSizes    = &sz;
        pi.maxSets       = count;
        if (vkCreateDescriptorPool(device, &pi, nullptr, &m_pool) != VK_SUCCESS)
            throw std::runtime_error("Failed to create bloom descriptor pool");

        std::vector<VkDescriptorSetLayout> layouts(count, m_setLayout);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = m_pool;
        ai.descriptorSetCount = count;
        ai.pSetLayouts        = layouts.data();
        m_sourceSets.resize(count);
        if (vkAllocateDescriptorSets(device, &ai, m_sourceSets.data()) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate bloom descriptor sets");
    }

    // Both pipelines share most state — only the render pass and blend differ.
    auto vertCode = readSpv("shaders/composite.vert.spv");                // reuse fullscreen tri
    VkShaderModule vertMod = makeShader(device, vertCode);
    auto fragDown = readSpv("shaders/bloom_downsample.frag.spv");
    auto fragUp   = readSpv("shaders/bloom_upsample.frag.spv");
    VkShaderModule fragDownMod = makeShader(device, fragDown);
    VkShaderModule fragUpMod   = makeShader(device, fragUp);

    auto makePipe = [&](VkShaderModule fragMod, VkRenderPass rp, bool additive, VkPipeline& out) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vertMod; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragMod; stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rast{};
        rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rast.polygonMode = VK_POLYGON_MODE_FILL;
        rast.lineWidth   = 1.0f;
        rast.cullMode    = VK_CULL_MODE_NONE;
        rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (additive) {
            ba.blendEnable         = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.colorBlendOp        = VK_BLEND_OP_ADD;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.alphaBlendOp        = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1; cb.pAttachments = &ba;
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        std::array<VkDynamicState, 2> dyn = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyns{};
        dyns.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyns.dynamicStateCount = static_cast<uint32_t>(dyn.size());
        dyns.pDynamicStates    = dyn.data();

        VkGraphicsPipelineCreateInfo pi{};
        pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pi.stageCount          = 2; pi.pStages = stages;
        pi.pVertexInputState   = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState      = &vp;
        pi.pRasterizationState = &rast;
        pi.pMultisampleState   = &ms;
        pi.pColorBlendState    = &cb;
        pi.pDepthStencilState  = &ds;
        pi.pDynamicState       = &dyns;
        pi.layout              = m_pipelineLayout;
        pi.renderPass          = rp;
        pi.subpass             = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &out) != VK_SUCCESS)
            throw std::runtime_error("Failed to create bloom pipeline");
    };

    makePipe(fragDownMod, m_downsampleRP, /*additive=*/false, m_downsamplePipeline);
    makePipe(fragUpMod,   m_upsampleRP,   /*additive=*/true,  m_upsamplePipeline);

    vkDestroyShaderModule(device, fragUpMod, nullptr);
    vkDestroyShaderModule(device, fragDownMod, nullptr);
    vkDestroyShaderModule(device, vertMod, nullptr);
}

void BloomPass::writeSourceSets(VkDevice device, VkImageView hdrView) {
    // sets[0] = scene HDR; sets[i+1] = bloom mip i (for sampling).
    std::vector<VkDescriptorImageInfo> infos(m_sourceSets.size());
    std::vector<VkWriteDescriptorSet>  writes(m_sourceSets.size());
    for (size_t i = 0; i < m_sourceSets.size(); i++) {
        infos[i].imageView   = (i == 0) ? hdrView : m_mipViews[i - 1];
        infos[i].sampler     = m_sampler;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_sourceSets[i];
        writes[i].dstBinding      = 0;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &infos[i];
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void BloomPass::render(VkCommandBuffer cmd, float threshold, float knee) {
    if (m_mipCount == 0) return;

    auto beginPass = [&](VkRenderPass rp, uint32_t mip) {
        VkRenderPassBeginInfo bi{};
        bi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass        = rp;
        bi.framebuffer       = m_framebuffers[mip];
        bi.renderArea.offset = {0, 0};
        bi.renderArea.extent = m_mipExtents[mip];
        bi.clearValueCount   = 0;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{};
        vp.x = 0.0f; vp.y = 0.0f;
        vp.width  = static_cast<float>(m_mipExtents[mip].width);
        vp.height = static_cast<float>(m_mipExtents[mip].height);
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.offset = {0, 0};
        sc.extent = m_mipExtents[mip];
        vkCmdSetScissor(cmd, 0, 1, &sc);
    };

    // ── Downsample chain: scene → mip 0 (brightpass) → mip 1 → ... → mip N-1 ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_downsamplePipeline);
    for (uint32_t mip = 0; mip < m_mipCount; mip++) {
        VkExtent2D srcExtent = (mip == 0) ? VkExtent2D{m_extent.width * 2, m_extent.height * 2}
                                          : m_mipExtents[mip - 1];
        BloomPC pc{};
        pc.invTexelX = 1.0f / static_cast<float>(srcExtent.width);
        pc.invTexelY = 1.0f / static_cast<float>(srcExtent.height);
        pc.threshold = threshold;
        pc.knee      = knee;
        pc.strength  = 0.0f;
        pc.mode      = (mip == 0) ? 0.0f : 1.0f;   // 0 = brightpass extract, 1 = plain downsample
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        VkDescriptorSet src = m_sourceSets[mip];   // sets[0] = scene; sets[i] = mip i-1
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &src, 0, nullptr);

        beginPass(m_downsampleRP, mip);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // ── Upsample chain: mip N-1 → N-2 (additive blend) → ... → mip 0 ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_upsamplePipeline);
    for (int32_t mip = static_cast<int32_t>(m_mipCount) - 2; mip >= 0; mip--) {
        VkExtent2D srcExtent = m_mipExtents[mip + 1];
        BloomPC pc{};
        pc.invTexelX = 1.0f / static_cast<float>(srcExtent.width);
        pc.invTexelY = 1.0f / static_cast<float>(srcExtent.height);
        pc.threshold = threshold;
        pc.knee      = knee;
        pc.strength  = 1.0f;
        pc.mode      = 2.0f;
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        VkDescriptorSet src = m_sourceSets[mip + 2];  // sets[i+1] = bloom mip i; we sample mip (mip+1) -> index (mip+1)+1
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &src, 0, nullptr);

        beginPass(m_upsampleRP, static_cast<uint32_t>(mip));
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
}

void BloomPass::destroy(VkDevice device, VmaAllocator allocator) {
    if (m_downsamplePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, m_downsamplePipeline, nullptr); m_downsamplePipeline = VK_NULL_HANDLE; }
    if (m_upsamplePipeline   != VK_NULL_HANDLE) { vkDestroyPipeline(device, m_upsamplePipeline,   nullptr); m_upsamplePipeline   = VK_NULL_HANDLE; }
    if (m_pipelineLayout     != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr); m_pipelineLayout   = VK_NULL_HANDLE; }
    if (m_setLayout          != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr); m_setLayout         = VK_NULL_HANDLE; }
    if (m_pool               != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, m_pool, nullptr);          m_pool              = VK_NULL_HANDLE; }
    for (auto fb : m_framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    m_framebuffers.clear();
    if (m_downsampleRP != VK_NULL_HANDLE) { vkDestroyRenderPass(device, m_downsampleRP, nullptr); m_downsampleRP = VK_NULL_HANDLE; }
    if (m_upsampleRP   != VK_NULL_HANDLE) { vkDestroyRenderPass(device, m_upsampleRP,   nullptr); m_upsampleRP   = VK_NULL_HANDLE; }
    if (m_sampler      != VK_NULL_HANDLE) { vkDestroySampler(device, m_sampler, nullptr);         m_sampler      = VK_NULL_HANDLE; }
    for (auto v : m_mipViews) vkDestroyImageView(device, v, nullptr);
    m_mipViews.clear();
    m_mipExtents.clear();
    m_sourceSets.clear();
    if (m_image != VK_NULL_HANDLE) { vmaDestroyImage(allocator, m_image, m_alloc); m_image = VK_NULL_HANDLE; m_alloc = VK_NULL_HANDLE; }
    m_mipCount = 0;
}

} // namespace Nyx
