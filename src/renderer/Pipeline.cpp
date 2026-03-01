#include "renderer/Pipeline.h"
#include "Logger.h"

#include <fstream>
#include <stdexcept>

namespace VulkanEngine {

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC
// ════════════════════════════════════════════════════════════════════════════

void Pipeline::init(VkDevice device, VkExtent2D swapchainExtent, VkFormat swapchainFormat) {
    createRenderPass(device, swapchainFormat);
    createGraphicsPipeline(device, swapchainExtent);
    LOG_INFO("Graphics pipeline created");
}

void Pipeline::cleanup(VkDevice device) {
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
}

void Pipeline::recreate(VkDevice device, VkExtent2D swapchainExtent, VkFormat swapchainFormat) {
    cleanup(device);
    createRenderPass(device, swapchainFormat);
    createGraphicsPipeline(device, swapchainExtent);
    LOG_INFO("Graphics pipeline recreated");
}

// ════════════════════════════════════════════════════════════════════════════
// RENDER PASS
// ════════════════════════════════════════════════════════════════════════════
// A render pass describes the framebuffer attachments (color, depth, etc.)
// and how they should be handled (clear, store, etc.)

void Pipeline::createRenderPass(VkDevice device, VkFormat swapchainFormat) {
    // We have one color attachment — the swapchain image
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = swapchainFormat;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;   // No multisampling
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;   // Clear at start
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;  // Keep the result
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;  // Ready to present

    // Subpass: a phase of rendering that reads/writes a subset of attachments
    // We only have one subpass that writes to the color attachment
    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;  // Index into the attachment descriptions array
    colorAttachmentRef.layout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorAttachmentRef;

    // Subpass dependency: ensures the swapchain image is ready before we write to it
    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;  // Operations before the render pass
    dependency.dstSubpass    = 0;                     // Our subpass (index 0)
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments    = &colorAttachment;
    renderPassInfo.subpassCount    = 1;
    renderPassInfo.pSubpasses      = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies   = &dependency;

    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create render pass");
    }
}

// ════════════════════════════════════════════════════════════════════════════
// GRAPHICS PIPELINE
// ════════════════════════════════════════════════════════════════════════════
// This is one of the most complex Vulkan structures. It defines the entire
// rendering pipeline from vertex input to pixel output.

void Pipeline::createGraphicsPipeline(VkDevice device, VkExtent2D swapchainExtent) {
    // ── Load compiled shaders ──────────────────────────────────────────────
    auto vertCode = readShaderFile("shaders/triangle.vert.spv");
    auto fragCode = readShaderFile("shaders/triangle.frag.spv");

    VkShaderModule vertModule = createShaderModule(device, vertCode);
    VkShaderModule fragModule = createShaderModule(device, fragCode);

    // Vertex shader stage
    VkPipelineShaderStageCreateInfo vertStageInfo{};
    vertStageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStageInfo.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    vertStageInfo.module = vertModule;
    vertStageInfo.pName  = "main";  // Entry point function name in the shader

    // Fragment shader stage
    VkPipelineShaderStageCreateInfo fragStageInfo{};
    fragStageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStageInfo.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStageInfo.module = fragModule;
    fragStageInfo.pName  = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertStageInfo, fragStageInfo };

    // ── Vertex input ───────────────────────────────────────────────────────
    // We're hardcoding the triangle in the vertex shader, so no vertex input needed
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount   = 0;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;

    // ── Input assembly ─────────────────────────────────────────────────────
    // How to interpret the vertices: as a triangle list
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // ── Viewport and scissor ───────────────────────────────────────────────
    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(swapchainExtent.width);
    viewport.height   = static_cast<float>(swapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports    = &viewport;
    viewportState.scissorCount  = 1;
    viewportState.pScissors     = &scissor;

    // ── Rasterizer ─────────────────────────────────────────────────────────
    // Converts triangles into fragments (pixels)
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable        = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;  // Fill triangles (not wireframe)
    rasterizer.lineWidth               = 1.0f;
    rasterizer.cullMode                = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace               = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable         = VK_FALSE;

    // ── Multisampling ──────────────────────────────────────────────────────
    // Disabled for now (will enable MSAA in a future phase)
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable  = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // ── Color blending ─────────────────────────────────────────────────────
    // How to combine the fragment shader output with what's already in the framebuffer
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;  // No blending, just overwrite

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType             = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable     = VK_FALSE;
    colorBlending.attachmentCount   = 1;
    colorBlending.pAttachments      = &colorBlendAttachment;

    // ── Pipeline layout ────────────────────────────────────────────────────
    // Describes uniform values and push constants (none for now)
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pipeline layout");
    }

    // ── Create the pipeline ────────────────────────────────────────────────
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = shaderStages;
    pipelineInfo.pVertexInputState   = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = nullptr;   // No depth testing yet
    pipelineInfo.pColorBlendState    = &colorBlending;
    pipelineInfo.pDynamicState       = nullptr;   // No dynamic state
    pipelineInfo.layout              = m_pipelineLayout;
    pipelineInfo.renderPass          = m_renderPass;
    pipelineInfo.subpass             = 0;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    // Shader modules can be destroyed after pipeline creation
    vkDestroyShaderModule(device, fragModule, nullptr);
    vkDestroyShaderModule(device, vertModule, nullptr);
}

// ════════════════════════════════════════════════════════════════════════════
// SHADER HELPERS
// ════════════════════════════════════════════════════════════════════════════

std::vector<char> Pipeline::readShaderFile(const std::string& filepath) {
    // Open the file at the end (ate) in binary mode
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + filepath);
    }

    // The read position at the end tells us the file size
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);

    // Seek back to the beginning and read the entire file
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    file.close();

    LOG_INFO("Loaded shader: {} ({} bytes)", filepath, fileSize);
    return buffer;
}

VkShaderModule Pipeline::createShaderModule(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    // SPIR-V expects uint32_t alignment — reinterpret the char buffer
    createInfo.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }
    return shaderModule;
}

} // namespace VulkanEngine
