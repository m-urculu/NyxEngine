#include "renderer/Pipeline.h"
#include "renderer/VulkanContext.h"
#include "renderer/Vertex.h"
#include "renderer/UniformTypes.h"
#include "Logger.h"

#include <fstream>
#include <stdexcept>
#include <array>

namespace Nyx {

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC
// ════════════════════════════════════════════════════════════════════════════

void Pipeline::init(VulkanContext& context, VkExtent2D swapchainExtent, VkFormat swapchainFormat,
                     VkFormat hdrFormat, VkFormat depthFormat,
                     VkDescriptorSetLayout globalLayout,
                     VkDescriptorSetLayout materialLayout, VkDescriptorSetLayout jointLayout) {
    // Scene render pass writes HDR linear; composite RP tonemaps it into the sRGB swapchain.
    createRenderPass(context.getDevice(), hdrFormat, depthFormat);
    createCompositeRenderPass(context.getDevice(), swapchainFormat);
    createCompositePipeline(context.getDevice());
    // Opaque uses LESS_OR_EQUAL + no write. The depth pre-pass populated the buffer
    // with the same vertex shader (so values are bit-identical for opaque meshes), but
    // LESS_OR_EQUAL is a strictly safer test: it covers EQUAL exactly and absorbs any
    // sub-ulp precision drift if a future change introduces it (the source of an
    // earlier "regions of the floor go missing at high resolution" bug). Same
    // overdraw-elimination outcome as EQUAL.
    createGraphicsPipeline(context.getDevice(), "shaders/mesh.vert.spv", "shaders/mesh.frag.spv",
                           { globalLayout, materialLayout }, m_pipelineLayout, m_pipeline,
                           VK_CULL_MODE_BACK_BIT, VK_COMPARE_OP_LESS_OR_EQUAL, VK_FALSE);
    // Skinned and cutout retain LESS + write — they aren't covered by the pre-pass
    // (no skinned/cutout meshes in the default scene need it yet) and write their own
    // depth in the main pass.
    createGraphicsPipeline(context.getDevice(), "shaders/mesh_skinned.vert.spv", "shaders/mesh.frag.spv",
                           { globalLayout, materialLayout, jointLayout }, m_skinnedLayout, m_skinnedPipeline);
    VkPipelineLayout unusedLayout = VK_NULL_HANDLE;
    createGraphicsPipeline(context.getDevice(), "shaders/mesh.vert.spv", "shaders/mesh.frag.spv",
                           { globalLayout, materialLayout }, unusedLayout, m_cutoutPipeline,
                           VK_CULL_MODE_NONE);
    if (unusedLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(context.getDevice(), unusedLayout, nullptr);
    createSkyPipeline(context.getDevice(), globalLayout);
    createDepthPrePassPipeline(context.getDevice(), globalLayout);
    LOG_INFO("Graphics pipelines created (mesh + skinned + cutout + sky + depth-prepass)");
}

void Pipeline::cleanup(VkDevice device) {
    if (m_compositePipeline   != VK_NULL_HANDLE) { vkDestroyPipeline(device, m_compositePipeline, nullptr);                m_compositePipeline   = VK_NULL_HANDLE; }
    if (m_compositeLayout     != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, m_compositeLayout, nullptr);            m_compositeLayout     = VK_NULL_HANDLE; }
    if (m_compositeSetLayout  != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, m_compositeSetLayout, nullptr);    m_compositeSetLayout  = VK_NULL_HANDLE; }
    if (m_compositeRenderPass != VK_NULL_HANDLE) { vkDestroyRenderPass(device, m_compositeRenderPass, nullptr);            m_compositeRenderPass = VK_NULL_HANDLE; }
    if (m_depthPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_depthPipeline, nullptr);
        m_depthPipeline = VK_NULL_HANDLE;
    }
    if (m_depthLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_depthLayout, nullptr);
        m_depthLayout = VK_NULL_HANDLE;
    }
    if (m_skyPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_skyPipeline, nullptr);
        m_skyPipeline = VK_NULL_HANDLE;
    }
    if (m_skyLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_skyLayout, nullptr);
        m_skyLayout = VK_NULL_HANDLE;
    }
    if (m_cutoutPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_cutoutPipeline, nullptr);
        m_cutoutPipeline = VK_NULL_HANDLE;
    }
    if (m_skinnedPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_skinnedPipeline, nullptr);
        m_skinnedPipeline = VK_NULL_HANDLE;
    }
    if (m_skinnedLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_skinnedLayout, nullptr);
        m_skinnedLayout = VK_NULL_HANDLE;
    }
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

void Pipeline::recreate(VulkanContext& context, VkExtent2D swapchainExtent, VkFormat swapchainFormat,
                         VkFormat hdrFormat, VkFormat depthFormat,
                         VkDescriptorSetLayout globalLayout,
                         VkDescriptorSetLayout materialLayout, VkDescriptorSetLayout jointLayout) {
    cleanup(context.getDevice());
    init(context, swapchainExtent, swapchainFormat, hdrFormat, depthFormat, globalLayout, materialLayout, jointLayout);
    LOG_INFO("Graphics pipelines recreated");
}

// ════════════════════════════════════════════════════════════════════════════
// RENDER PASS
// ════════════════════════════════════════════════════════════════════════════

void Pipeline::createRenderPass(VkDevice device, VkFormat hdrFormat, VkFormat depthFormat) {
    // Attachment 0: HDR color target. Sampled by the composite pass after rendering,
    // so finalLayout is SHADER_READ_ONLY (the subpass dependency to EXTERNAL below
    // handles the synchronization).
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = hdrFormat;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Attachment 1: Depth
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format         = depthFormat;
    depthAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                          | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // After we finish writing, the composite pass samples the HDR image in its
    // fragment shader — flush colour writes before that read.
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments    = attachments.data();
    renderPassInfo.subpassCount    = 1;
    renderPassInfo.pSubpasses      = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    renderPassInfo.pDependencies   = deps.data();

    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create render pass");
    }
}

// ════════════════════════════════════════════════════════════════════════════
// GRAPHICS PIPELINE
// ════════════════════════════════════════════════════════════════════════════

void Pipeline::createGraphicsPipeline(VkDevice device, const std::string& vertPath,
                                       const std::string& fragPath,
                                       const std::vector<VkDescriptorSetLayout>& setLayouts,
                                       VkPipelineLayout& outLayout, VkPipeline& outPipeline,
                                       VkCullModeFlags cullMode,
                                       VkCompareOp     depthCompareOp,
                                       VkBool32        depthWriteEnable) {
    // ── Load compiled shaders ──────────────────────────────────────────────
    auto vertCode = readShaderFile(vertPath);
    auto fragCode = readShaderFile(fragPath);

    VkShaderModule vertModule = createShaderModule(device, vertCode);
    VkShaderModule fragModule = createShaderModule(device, fragCode);

    VkPipelineShaderStageCreateInfo vertStageInfo{};
    vertStageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStageInfo.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    vertStageInfo.module = vertModule;
    vertStageInfo.pName  = "main";

    VkPipelineShaderStageCreateInfo fragStageInfo{};
    fragStageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStageInfo.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStageInfo.module = fragModule;
    fragStageInfo.pName  = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertStageInfo, fragStageInfo };

    // ── Vertex input — read from vertex buffers ───────────────────────────
    auto bindingDesc = Vertex::getBindingDescription();
    auto attrDescs   = Vertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount    = 1;
    vertexInputInfo.pVertexBindingDescriptions       = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount  = static_cast<uint32_t>(attrDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions     = attrDescs.data();

    // ── Input assembly ─────────────────────────────────────────────────────
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // ── Viewport and scissor (dynamic — set per frame in command buffer) ──
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports    = nullptr; // dynamic
    viewportState.scissorCount  = 1;
    viewportState.pScissors     = nullptr; // dynamic

    // ── Rasterizer — CCW front face for OBJ compatibility ─────────────────
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable        = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth               = 1.0f;
    rasterizer.cullMode                = cullMode;
    rasterizer.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable         = VK_FALSE;

    // ── Multisampling ──────────────────────────────────────────────────────
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable  = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // ── Color blending ─────────────────────────────────────────────────────
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType             = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable     = VK_FALSE;
    colorBlending.attachmentCount   = 1;
    colorBlending.pAttachments      = &colorBlendAttachment;

    // ── Depth stencil ──────────────────────────────────────────────────────
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable       = VK_TRUE;
    depthStencil.depthWriteEnable      = depthWriteEnable;
    depthStencil.depthCompareOp        = depthCompareOp;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable     = VK_FALSE;

    // ── Push constants (per-object model + normalMatrix) ────────────────
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset     = 0;
    pushConstantRange.size       = sizeof(PushConstants);

    // ── Pipeline layout (descriptor set layouts + push constants) ───────
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount         = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts            = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges    = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &outLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pipeline layout");
    }

    // ── Dynamic state (viewport + scissor set per frame) ──────────────────
    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates    = dynamicStates.data();

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
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = outLayout;
    pipelineInfo.renderPass          = m_renderPass;
    pipelineInfo.subpass             = 0;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    vkDestroyShaderModule(device, fragModule, nullptr);
    vkDestroyShaderModule(device, vertModule, nullptr);
}

// ════════════════════════════════════════════════════════════════════════════
// SHADER HELPERS
// ════════════════════════════════════════════════════════════════════════════

std::vector<char> Pipeline::readShaderFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + filepath);
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    file.close();

    LOG_INFO("Loaded shader: {} ({} bytes)", filepath, fileSize);
    return buffer;
}

// ════════════════════════════════════════════════════════════════════════════
// SKY PIPELINE
// ════════════════════════════════════════════════════════════════════════════
// Fullscreen procedural skybox. Unlike the mesh pipelines this has:
//   • no vertex input (3 verts generated from gl_VertexIndex)
//   • depth test LESS_EQUAL with depth-write OFF (so meshes overdraw it cleanly)
//   • cull disabled (the fullscreen triangle is single-sided to clip space anyway)
//   • only the global UBO bound; no material set, no push constants.
void Pipeline::createSkyPipeline(VkDevice device, VkDescriptorSetLayout globalLayout) {
    auto vertCode = readShaderFile("shaders/sky.vert.spv");
    auto fragCode = readShaderFile("shaders/sky.frag.spv");
    VkShaderModule vertModule = createShaderModule(device, vertCode);
    VkShaderModule fragModule = createShaderModule(device, fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // no bindings / attributes — the vertex shader fabricates the fullscreen triangle

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;   // dynamic

    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.lineWidth   = 1.0f;
    rast.cullMode    = VK_CULL_MODE_NONE;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAtt.blendEnable    = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1; blend.pAttachments = &blendAtt;

    // depth=1 (far plane) with LESS_EQUAL so the sky passes the cleared depth, and
    // depth-write OFF so subsequent mesh draws can punch through it.
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts    = &globalLayout;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_skyLayout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create sky pipeline layout");

    std::array<VkDynamicState, 2> dyn = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = static_cast<uint32_t>(dyn.size());
    dynState.pDynamicStates    = dyn.data();

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount          = 2; pi.pStages = stages;
    pi.pVertexInputState   = &vertexInput;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &rast;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = &depth;
    pi.pColorBlendState    = &blend;
    pi.pDynamicState       = &dynState;
    pi.layout              = m_skyLayout;
    pi.renderPass          = m_renderPass;
    pi.subpass             = 0;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &m_skyPipeline) != VK_SUCCESS)
        throw std::runtime_error("Failed to create sky pipeline");

    vkDestroyShaderModule(device, fragModule, nullptr);
    vkDestroyShaderModule(device, vertModule, nullptr);
}

// ════════════════════════════════════════════════════════════════════════════
// DEPTH PRE-PASS PIPELINE
// ════════════════════════════════════════════════════════════════════════════
// Z pre-pass for opaque meshes: depth_only.vert + empty depth_only.frag, color writes
// disabled (mask=0). Establishes the depth buffer so the heavy PBR shader runs at
// most once per pixel in the main pass (which uses depth EQUAL + no write). Cuts
// overdraw on regions where many primitives stack up (head behind helmet, etc.).
void Pipeline::createDepthPrePassPipeline(VkDevice device, VkDescriptorSetLayout globalLayout) {
    // CRITICAL: reuse mesh.vert so gl_Position is bit-identical to the main pass.
    // Using a separate depth_only.vert (which computes the same math but in different
    // surrounding context) yields slightly different .z values after compilation, and
    // the main pass's EQUAL depth test then fails on whole regions of geometry. The
    // fragment shader is still the empty depth_only.frag — varyings get dropped.
    auto vertCode = readShaderFile("shaders/mesh.vert.spv");
    auto fragCode = readShaderFile("shaders/depth_only.frag.spv");
    VkShaderModule vertModule = createShaderModule(device, vertCode);
    VkShaderModule fragModule = createShaderModule(device, fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule; stages[1].pName = "main";

    // Same vertex input as the mesh pipelines (Vertex layout). The pre-pass shader
    // only reads location 0 (position); unused attributes are ignored.
    auto bindingDesc = Vertex::getBindingDescription();
    auto attrDescs   = Vertex::getAttributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    vertexInput.pVertexAttributeDescriptions    = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;   // dynamic

    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.lineWidth   = 1.0f;
    rast.cullMode    = VK_CULL_MODE_BACK_BIT;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Color writes OFF — depth is the only output.
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = 0;
    blendAtt.blendEnable    = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1; blend.pAttachments = &blendAtt;

    // Standard depth: test LESS, write enabled (this IS where opaque depth is born).
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp   = VK_COMPARE_OP_LESS;

    // Same push constants as the mesh pipeline — depth_only.vert reads `pc.model`.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &globalLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_depthLayout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create depth pre-pass pipeline layout");

    std::array<VkDynamicState, 2> dyn = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = static_cast<uint32_t>(dyn.size());
    dynState.pDynamicStates    = dyn.data();

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount          = 2; pi.pStages = stages;
    pi.pVertexInputState   = &vertexInput;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &rast;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = &depth;
    pi.pColorBlendState    = &blend;
    pi.pDynamicState       = &dynState;
    pi.layout              = m_depthLayout;
    pi.renderPass          = m_renderPass;
    pi.subpass             = 0;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &m_depthPipeline) != VK_SUCCESS)
        throw std::runtime_error("Failed to create depth pre-pass pipeline");

    vkDestroyShaderModule(device, fragModule, nullptr);
    vkDestroyShaderModule(device, vertModule, nullptr);
}

// ════════════════════════════════════════════════════════════════════════════
// COMPOSITE: HDR scene → tonemap → swapchain (LDR sRGB)
// ════════════════════════════════════════════════════════════════════════════
// Render pass targets the swapchain (one color attachment, no depth — the composite
// fullscreen tri doesn't need it; UI rendering that follows in this same pass uses
// VK_COMPARE_OP_ALWAYS so depth isn't required either).
void Pipeline::createCompositeRenderPass(VkDevice device, VkFormat swapchainFormat) {
    VkAttachmentDescription color{};
    color.format         = swapchainFormat;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments    = &color;
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = 1;
    info.pDependencies   = &dep;
    if (vkCreateRenderPass(device, &info, nullptr, &m_compositeRenderPass) != VK_SUCCESS)
        throw std::runtime_error("Failed to create composite render pass");
}

void Pipeline::createCompositePipeline(VkDevice device) {
    // Set layout: binding 0 = HDR scene sampler, binding 1 = bloom result sampler.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < 2; i++) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = static_cast<uint32_t>(bindings.size());
    dsl.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &dsl, nullptr, &m_compositeSetLayout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create composite set layout");

    auto vertCode = readShaderFile("shaders/composite.vert.spv");
    auto fragCode = readShaderFile("shaders/composite.frag.spv");
    VkShaderModule vertMod = createShaderModule(device, vertCode);
    VkShaderModule fragMod = createShaderModule(device, fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vertMod; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragMod; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};   // no vertex input — fullscreen tri from gl_VertexIndex
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.lineWidth   = 1.0f;
    rast.cullMode    = VK_CULL_MODE_NONE;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    att.blendEnable    = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &att;

    // No depth attachment in composite RP → depth state is informational only.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_ALWAYS;

    // 4 floats: bloomStrength, exposure, tonemap mode, pad. Drives composite.frag.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(float) * 4;

    VkPipelineLayoutCreateInfo li{};
    li.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount         = 1;
    li.pSetLayouts            = &m_compositeSetLayout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(device, &li, nullptr, &m_compositeLayout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create composite pipeline layout");

    std::array<VkDynamicState, 2> dyn = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = static_cast<uint32_t>(dyn.size());
    dynState.pDynamicStates    = dyn.data();

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount          = 2; pi.pStages = stages;
    pi.pVertexInputState   = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &rast;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = &ds;
    pi.pColorBlendState    = &cb;
    pi.pDynamicState       = &dynState;
    pi.layout              = m_compositeLayout;
    pi.renderPass          = m_compositeRenderPass;
    pi.subpass             = 0;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &m_compositePipeline) != VK_SUCCESS)
        throw std::runtime_error("Failed to create composite pipeline");

    vkDestroyShaderModule(device, fragMod, nullptr);
    vkDestroyShaderModule(device, vertMod, nullptr);
}

VkShaderModule Pipeline::createShaderModule(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }
    return shaderModule;
}

} // namespace Nyx
