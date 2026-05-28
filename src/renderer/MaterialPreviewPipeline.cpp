#include "renderer/MaterialPreviewPipeline.h"
#include "renderer/VulkanContext.h"
#include "Logger.h"

#include <fstream>
#include <stdexcept>
#include <array>

namespace Nyx {

VkVertexInputBindingDescription PreviewVertex::getBindingDescription() {
    VkVertexInputBindingDescription b{};
    b.binding = 0; b.stride = sizeof(PreviewVertex); b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return b;
}

std::array<VkVertexInputAttributeDescription, 3> PreviewVertex::getAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 3> a{};
    a[0].location = 0; a[0].binding = 0; a[0].format = VK_FORMAT_R32G32B32_SFLOAT; a[0].offset = offsetof(PreviewVertex, pos);
    a[1].location = 1; a[1].binding = 0; a[1].format = VK_FORMAT_R32G32B32_SFLOAT; a[1].offset = offsetof(PreviewVertex, normal);
    a[2].location = 2; a[2].binding = 0; a[2].format = VK_FORMAT_R32G32_SFLOAT;    a[2].offset = offsetof(PreviewVertex, uv);
    return a;
}

void MaterialPreviewPipeline::init(VulkanContext& context, VkRenderPass renderPass) {
    create(context.getDevice(), renderPass);
    LOG_INFO("Material preview pipeline created");
}

void MaterialPreviewPipeline::cleanup(VkDevice device) {
    if (m_pipeline)       { vkDestroyPipeline(device, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_setLayout)      { vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr); m_setLayout = VK_NULL_HANDLE; }
}

void MaterialPreviewPipeline::recreate(VulkanContext& context, VkRenderPass renderPass) {
    cleanup(context.getDevice());
    create(context.getDevice(), renderPass);
}

void MaterialPreviewPipeline::create(VkDevice device, VkRenderPass renderPass) {
    auto vertCode = readShaderFile("shaders/preview.vert.spv");
    auto fragCode = readShaderFile("shaders/preview.frag.spv");
    VkShaderModule vertModule = createShaderModule(device, vertCode);
    VkShaderModule fragModule = createShaderModule(device, fragCode);

    VkPipelineShaderStageCreateInfo vert{}, frag{};
    vert.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert.stage = VK_SHADER_STAGE_VERTEX_BIT;   vert.module = vertModule; vert.pName = "main";
    frag.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag.stage = VK_SHADER_STAGE_FRAGMENT_BIT; frag.module = fragModule; frag.pName = "main";
    VkPipelineShaderStageCreateInfo stages[] = { vert, frag };

    auto bindingDesc = PreviewVertex::getBindingDescription();
    auto attrDescs   = PreviewVertex::getAttributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bindingDesc;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    vi.pVertexAttributeDescriptions    = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    // Backface cull, CCW front (matches the engine's mesh pipeline + Y-flipped proj).
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    rs.cullMode = VK_CULL_MODE_BACK_BIT; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    // No depth test/write (single convex sphere; the swapchain depth holds the 3D
    // scene which we must not test against).
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE; ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1; dslInfo.pBindings = &samplerBinding;
    if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &m_setLayout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create preview descriptor set layout");

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0; pc.size = sizeof(PreviewPush);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1; plInfo.pSetLayouts = &m_setLayout;
    plInfo.pushConstantRangeCount = 1; plInfo.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create preview pipeline layout");

    std::array<VkDynamicState, 2> dyn = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = static_cast<uint32_t>(dyn.size());
    dynState.pDynamicStates = dyn.data();

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2; pi.pStages = stages;
    pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp; pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dynState;
    pi.layout = m_pipelineLayout; pi.renderPass = renderPass; pi.subpass = 0;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &m_pipeline) != VK_SUCCESS)
        throw std::runtime_error("Failed to create material preview pipeline");

    vkDestroyShaderModule(device, fragModule, nullptr);
    vkDestroyShaderModule(device, vertModule, nullptr);
}

std::vector<char> MaterialPreviewPipeline::readShaderFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Failed to open shader file: " + filepath);
    size_t sz = static_cast<size_t>(file.tellg());
    std::vector<char> buf(sz);
    file.seekg(0); file.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}

VkShaderModule MaterialPreviewPipeline::createShaderModule(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    if (vkCreateShaderModule(device, &info, nullptr, &m) != VK_SUCCESS)
        throw std::runtime_error("Failed to create preview shader module");
    return m;
}

} // namespace Nyx
