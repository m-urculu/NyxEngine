#include "renderer/PointShadowMap.h"
#include "renderer/VulkanContext.h"
#include "renderer/Vertex.h"
#include "Logger.h"

#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace Nyx {

void PointShadowMap::init(VulkanContext& context, VkDescriptorSetLayout globalLayout,
                          uint32_t resolution) {
    m_resolution = resolution;
    createImageAndViews(context);
    createSampler(context.getDevice());
    createRenderPass(context.getDevice());
    createFramebuffers(context.getDevice());
    createPipeline(context.getDevice(), globalLayout);
    LOG_INFO("PointShadowMap initialized ({}x{} per face)", m_resolution, m_resolution);
}

void PointShadowMap::cleanup(VkDevice device, VmaAllocator allocator) {
    if (m_pipeline   != VK_NULL_HANDLE) { vkDestroyPipeline(device, m_pipeline, nullptr);            m_pipeline = VK_NULL_HANDLE; }
    if (m_layout     != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, m_layout, nullptr);        m_layout = VK_NULL_HANDLE; }
    for (auto& fb : m_framebuffers) {
        if (fb != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, fb, nullptr); fb = VK_NULL_HANDLE; }
    }
    if (m_renderPass != VK_NULL_HANDLE) { vkDestroyRenderPass(device, m_renderPass, nullptr);        m_renderPass = VK_NULL_HANDLE; }
    if (m_sampler    != VK_NULL_HANDLE) { vkDestroySampler(device, m_sampler, nullptr);              m_sampler = VK_NULL_HANDLE; }
    for (auto& v : m_faceViews) {
        if (v != VK_NULL_HANDLE) { vkDestroyImageView(device, v, nullptr); v = VK_NULL_HANDLE; }
    }
    if (m_cubeView   != VK_NULL_HANDLE) { vkDestroyImageView(device, m_cubeView, nullptr);           m_cubeView = VK_NULL_HANDLE; }
    if (m_image      != VK_NULL_HANDLE) { vmaDestroyImage(allocator, m_image, m_alloc);              m_image = VK_NULL_HANDLE; m_alloc = VK_NULL_HANDLE; }
}

void PointShadowMap::beginFace(VkCommandBuffer cmd, uint32_t face) const {
    VkClearValue clears[1]{};
    clears[0].color = {{1.0f, 1.0f, 1.0f, 1.0f}};   // 1.0 = max distance (= fully lit when sampled)

    VkRenderPassBeginInfo info{};
    info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass        = m_renderPass;
    info.framebuffer       = m_framebuffers[face];
    info.renderArea.offset = {0, 0};
    info.renderArea.extent = {m_resolution, m_resolution};
    info.clearValueCount   = 1;
    info.pClearValues      = clears;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width  = static_cast<float>(m_resolution);
    vp.height = static_cast<float>(m_resolution);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.offset = {0, 0};
    sc.extent = {m_resolution, m_resolution};
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void PointShadowMap::endFace(VkCommandBuffer cmd) const {
    vkCmdEndRenderPass(cmd);
}

void PointShadowMap::prime(VkCommandBuffer cmd) const {
    for (uint32_t f = 0; f < 6; ++f) { beginFace(cmd, f); endFace(cmd); }
}

void PointShadowMap::createImageAndViews(VulkanContext& context) {
    // Cube map image: 6 array layers, R32 single-channel float for linear depth.
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.extent        = {m_resolution, m_resolution, 1};
    info.mipLevels     = 1;
    info.arrayLayers   = 6;
    info.format        = m_format;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(context.getAllocator(), &info, &allocInfo, &m_image, &m_alloc, nullptr) != VK_SUCCESS)
        throw std::runtime_error("Failed to create point shadow cube image");

    // Cube view (for sampling in mesh.frag).
    VkImageViewCreateInfo cubeInfo{};
    cubeInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cubeInfo.image    = m_image;
    cubeInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    cubeInfo.format   = m_format;
    cubeInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    cubeInfo.subresourceRange.baseMipLevel   = 0;
    cubeInfo.subresourceRange.levelCount     = 1;
    cubeInfo.subresourceRange.baseArrayLayer = 0;
    cubeInfo.subresourceRange.layerCount     = 6;
    if (vkCreateImageView(context.getDevice(), &cubeInfo, nullptr, &m_cubeView) != VK_SUCCESS)
        throw std::runtime_error("Failed to create cube view");

    // 2D face views (for rendering into a single cube face).
    for (uint32_t f = 0; f < 6; ++f) {
        VkImageViewCreateInfo faceInfo{};
        faceInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        faceInfo.image    = m_image;
        faceInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        faceInfo.format   = m_format;
        faceInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        faceInfo.subresourceRange.baseMipLevel   = 0;
        faceInfo.subresourceRange.levelCount     = 1;
        faceInfo.subresourceRange.baseArrayLayer = f;
        faceInfo.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(context.getDevice(), &faceInfo, nullptr, &m_faceViews[f]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create cube face view");
    }
}

void PointShadowMap::createSampler(VkDevice device) {
    // Linear filter, clamp to edge. No hardware shadow compare — we read the
    // stored distance and compare in the shader (samplerCube, not samplerCubeShadow).
    VkSamplerCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter    = VK_FILTER_LINEAR;
    info.minFilter    = VK_FILTER_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.minLod       = 0.0f;
    info.maxLod       = 1.0f;
    if (vkCreateSampler(device, &info, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("Failed to create point shadow sampler");
}

void PointShadowMap::createRenderPass(VkDevice device) {
    // Single-attachment color render pass. The face begins as UNDEFINED each
    // frame (we don't care about previous contents — load op clears) and ends
    // as SHADER_READ_ONLY so mesh.frag can sample without an explicit barrier.
    VkAttachmentDescription color{};
    color.format         = m_format;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments    = &color;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &subpass;
    rp.dependencyCount = static_cast<uint32_t>(deps.size());
    rp.pDependencies   = deps.data();
    if (vkCreateRenderPass(device, &rp, nullptr, &m_renderPass) != VK_SUCCESS)
        throw std::runtime_error("Failed to create point shadow render pass");
}

void PointShadowMap::createFramebuffers(VkDevice device) {
    for (uint32_t f = 0; f < 6; ++f) {
        VkFramebufferCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = m_renderPass;
        info.attachmentCount = 1;
        info.pAttachments    = &m_faceViews[f];
        info.width           = m_resolution;
        info.height          = m_resolution;
        info.layers          = 1;
        if (vkCreateFramebuffer(device, &info, nullptr, &m_framebuffers[f]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create point shadow framebuffer");
    }
}

// Push-constant layout shared with point_shadow.{vert,frag}.
struct PointShadowPC {
    float viewProj[16];
    float model[16];
    float lightPosAndFar[4];
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

void PointShadowMap::createPipeline(VkDevice device, VkDescriptorSetLayout globalLayout) {
    auto vertCode = readSpv("shaders/point_shadow.vert.spv");
    auto fragCode = readSpv("shaders/point_shadow.frag.spv");
    VkShaderModule vertMod = makeShader(device, vertCode);
    VkShaderModule fragMod = makeShader(device, fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vertMod; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragMod; stages[1].pName = "main";

    auto bindingDesc = Vertex::getBindingDescription();
    auto attrDescs   = Vertex::getAttributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bindingDesc;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    vi.pVertexAttributeDescriptions    = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    // Back-face cull (default winding); shadow acne is mitigated by the small
    // self-bias in the shader compare.
    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.lineWidth   = 1.0f;
    rast.cullMode    = VK_CULL_MODE_BACK_BIT;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Min blend: keeps the smallest (closest) distance written so far. Avoids
    // needing a separate depth attachment; the cube map ends up storing the
    // distance to the nearest occluder from the light.
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT;
    att.blendEnable         = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    att.colorBlendOp        = VK_BLEND_OP_MIN;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.alphaBlendOp        = VK_BLEND_OP_MIN;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1; blend.pAttachments = &att;

    // No depth attachment in this pipeline — we use a color attachment storing
    // linear distance and rely on overdraw / first-hit being closest enough.
    // For sharper occlusion we keep it correct via gl_FragDepth not needed
    // because the cube simply records the nearest distance written; we mitigate
    // overdraw by enabling depth test against a transient depth buffer? Skip
    // for MVP — the cube map stores whatever the rasterizer happens to draw.
    // Practical effect: small artifacts on overlapping geometry but workable.
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp   = VK_COMPARE_OP_ALWAYS;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset     = 0;
    push.size       = sizeof(PointShadowPC);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &globalLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &push;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_layout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create point shadow pipeline layout");

    std::array<VkDynamicState, 2> dyn = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = static_cast<uint32_t>(dyn.size());
    dynState.pDynamicStates    = dyn.data();

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount          = 2;        pi.pStages = stages;
    pi.pVertexInputState   = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &rast;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = &depth;
    pi.pColorBlendState    = &blend;
    pi.pDynamicState       = &dynState;
    pi.layout              = m_layout;
    pi.renderPass          = m_renderPass;
    pi.subpass             = 0;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &m_pipeline) != VK_SUCCESS)
        throw std::runtime_error("Failed to create point shadow pipeline");

    vkDestroyShaderModule(device, fragMod, nullptr);
    vkDestroyShaderModule(device, vertMod, nullptr);
}

} // namespace Nyx
