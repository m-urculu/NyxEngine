#include "renderer/ShadowMap.h"
#include "renderer/VulkanContext.h"
#include "renderer/Vertex.h"
#include "renderer/UniformTypes.h"
#include "Logger.h"

#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace Nyx {

void ShadowMap::init(VulkanContext& context, VkDescriptorSetLayout globalLayout, uint32_t resolution) {
    m_resolution = resolution;
    createImage(context);
    createRenderPass(context.getDevice());
    createFramebuffer(context.getDevice());
    createSampler(context.getDevice());
    createPipeline(context.getDevice(), globalLayout);
    LOG_INFO("ShadowMap initialized ({}x{})", m_resolution, m_resolution);
}

void ShadowMap::cleanup(VkDevice device, VmaAllocator allocator) {
    if (m_pipeline      != VK_NULL_HANDLE) { vkDestroyPipeline(device, m_pipeline, nullptr);            m_pipeline = VK_NULL_HANDLE; }
    if (m_layout        != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, m_layout, nullptr);        m_layout = VK_NULL_HANDLE; }
    if (m_framebuffer   != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, m_framebuffer, nullptr);      m_framebuffer = VK_NULL_HANDLE; }
    if (m_renderPass    != VK_NULL_HANDLE) { vkDestroyRenderPass(device, m_renderPass, nullptr);        m_renderPass = VK_NULL_HANDLE; }
    if (m_sampler       != VK_NULL_HANDLE) { vkDestroySampler(device, m_sampler, nullptr);              m_sampler = VK_NULL_HANDLE; }
    if (m_view          != VK_NULL_HANDLE) { vkDestroyImageView(device, m_view, nullptr);               m_view = VK_NULL_HANDLE; }
    if (m_image         != VK_NULL_HANDLE) { vmaDestroyImage(allocator, m_image, m_alloc);              m_image = VK_NULL_HANDLE; m_alloc = VK_NULL_HANDLE; }
}

void ShadowMap::beginRenderPass(VkCommandBuffer cmd) const {
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo info{};
    info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass        = m_renderPass;
    info.framebuffer       = m_framebuffer;
    info.renderArea.offset = {0, 0};
    info.renderArea.extent = {m_resolution, m_resolution};
    info.clearValueCount   = 1;
    info.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);

    // Dynamic viewport/scissor to the shadow extent (different from the main viewport).
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

void ShadowMap::endRenderPass(VkCommandBuffer cmd) const {
    vkCmdEndRenderPass(cmd);
}

void ShadowMap::createImage(VulkanContext& context) {
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.extent        = {m_resolution, m_resolution, 1};
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.format        = m_format;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(context.getAllocator(), &info, &allocInfo, &m_image, &m_alloc, nullptr) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shadow map image");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = m_format;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;
    if (vkCreateImageView(context.getDevice(), &viewInfo, nullptr, &m_view) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shadow map view");
}

void ShadowMap::createRenderPass(VkDevice device) {
    // Depth-only render pass. The image starts UNDEFINED each frame (we don't care
    // about previous contents — the load op clears it) and ends as DEPTH_READ_ONLY so
    // the main pass can sample it in mesh.frag without an extra explicit barrier.
    VkAttachmentDescription depth{};
    depth.format         = m_format;
    depth.samples        = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 0;
    subpass.pDepthStencilAttachment = &depthRef;

    // Sync: this frame's writes must wait for the previous frame's reads in the main
    // pass; subsequent samples in the main pass wait for our writes.
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments    = &depth;
    rp.subpassCount    = 1;
    rp.pSubpasses      = &subpass;
    rp.dependencyCount = static_cast<uint32_t>(deps.size());
    rp.pDependencies   = deps.data();
    if (vkCreateRenderPass(device, &rp, nullptr, &m_renderPass) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shadow render pass");
}

void ShadowMap::createFramebuffer(VkDevice device) {
    VkFramebufferCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass      = m_renderPass;
    info.attachmentCount = 1;
    info.pAttachments    = &m_view;
    info.width           = m_resolution;
    info.height          = m_resolution;
    info.layers          = 1;
    if (vkCreateFramebuffer(device, &info, nullptr, &m_framebuffer) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shadow framebuffer");
}

void ShadowMap::createSampler(VkDevice device) {
    // Hardware comparison sampler: linear filter + compareEnable=TRUE means a single
    // `texture(sampler2DShadow, vec3(uv, ref))` call returns a 4-tap bilinear PCF
    // compare result. That's roughly 9× cheaper than software 3×3 PCF for similar
    // quality, and exactly matches what the shader's sampleShadow now expects.
    // Clamp-to-border with FLOAT_OPAQUE_WHITE returns depth=1 (fully lit) off-map.
    VkSamplerCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter     = VK_FILTER_LINEAR;
    info.minFilter     = VK_FILTER_LINEAR;
    info.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.borderColor   = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    info.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.minLod        = 0.0f;
    info.maxLod        = 1.0f;
    info.compareEnable = VK_TRUE;
    info.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(device, &info, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shadow sampler");
}

// Read SPV file (mirrors Pipeline::readShaderFile to avoid coupling).
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

void ShadowMap::createPipeline(VkDevice device, VkDescriptorSetLayout globalLayout) {
    // shadow.vert reads lightSpace from the global UBO and model from push constants.
    // shadow.frag is empty (we only care about depth).
    auto vertCode = readSpv("shaders/shadow.vert.spv");
    auto fragCode = readSpv("shaders/shadow.frag.spv");
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
    vp.viewportCount = 1; vp.scissorCount = 1;   // dynamic

    // Front-face culling to reduce self-shadow acne (only back-facing triangles cast).
    // Plus a small depth bias to push shadow depth slightly away from the lit surface.
    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType            = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode      = VK_POLYGON_MODE_FILL;
    rast.lineWidth        = 1.0f;
    rast.cullMode         = VK_CULL_MODE_FRONT_BIT;
    rast.frontFace        = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.depthBiasEnable  = VK_TRUE;
    rast.depthBiasConstantFactor = 1.5f;
    rast.depthBiasSlopeFactor    = 1.75f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // No color attachments at all — the render pass has only depth.
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 0;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset     = 0;
    push.size       = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &globalLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &push;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_layout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shadow pipeline layout");

    std::array<VkDynamicState, 3> dyn = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS
    };
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
        throw std::runtime_error("Failed to create shadow pipeline");

    vkDestroyShaderModule(device, fragMod, nullptr);
    vkDestroyShaderModule(device, vertMod, nullptr);
}

} // namespace Nyx
