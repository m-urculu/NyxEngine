#pragma once

// Pipeline.h — Graphics pipeline and render pass

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace Nyx {

class VulkanContext;

class Pipeline {
public:
    void init(VulkanContext& context, VkExtent2D swapchainExtent, VkFormat swapchainFormat,
              VkFormat hdrFormat, VkFormat depthFormat,
              VkDescriptorSetLayout globalLayout,
              VkDescriptorSetLayout materialLayout, VkDescriptorSetLayout jointLayout);
    void cleanup(VkDevice device);

    void recreate(VulkanContext& context, VkExtent2D swapchainExtent, VkFormat swapchainFormat,
                  VkFormat hdrFormat, VkFormat depthFormat,
                  VkDescriptorSetLayout globalLayout,
                  VkDescriptorSetLayout materialLayout, VkDescriptorSetLayout jointLayout);

    VkRenderPass     getRenderPass()           const { return m_renderPass; }            // scene HDR render pass
    VkRenderPass     getCompositeRenderPass()  const { return m_compositeRenderPass; }   // tonemaps HDR → swapchain
    VkPipeline       getPipeline()             const { return m_pipeline; }
    VkPipelineLayout getPipelineLayout()       const { return m_pipelineLayout; }

    // Composite pipeline: fullscreen tri samples the HDR scene (and later, bloom),
    // applies ACES tonemap, writes LDR to the swapchain.
    VkPipeline            getCompositePipeline()       const { return m_compositePipeline; }
    VkPipelineLayout      getCompositePipelineLayout() const { return m_compositeLayout; }
    VkDescriptorSetLayout getCompositeSetLayout()      const { return m_compositeSetLayout; }

    // Skinned variant: mesh_skinned.vert, 3 set layouts {global, material, joint}.
    VkPipeline       getSkinnedPipeline()       const { return m_skinnedPipeline; }
    VkPipelineLayout getSkinnedPipelineLayout() const { return m_skinnedLayout; }

    // Alpha-cutout variant: same shaders/layout as the opaque pipeline but with
    // back-face culling DISABLED (two-sided) — for hair cards / foliage / fences
    // whose fragments are dropped by the shader's alpha test.
    VkPipeline       getCutoutPipeline()       const { return m_cutoutPipeline; }
    VkPipelineLayout getCutoutPipelineLayout() const { return m_pipelineLayout; }  // shares opaque layout

    // Procedural skybox: fullscreen triangle, no vertex input, samples the analytic
    // sky gradient from the global UBO. Drawn first each frame; depth-write OFF and
    // test LESS_EQUAL so opaque meshes overdraw it where they exist.
    VkPipeline       getSkyPipeline()       const { return m_skyPipeline; }
    VkPipelineLayout getSkyPipelineLayout() const { return m_skyLayout; }

    // Depth pre-pass: depth-only render of opaque meshes (color writes OFF) to
    // establish the depth buffer. The opaque main pipeline then runs with depth
    // test EQUAL + write OFF, so each pixel's expensive PBR shading runs exactly
    // once — eliminating overdraw on regions like the helmet/head where multiple
    // primitives stack up.
    VkPipeline       getDepthPrePassPipeline()       const { return m_depthPipeline; }
    VkPipelineLayout getDepthPrePassPipelineLayout() const { return m_depthLayout; }

private:
    VkRenderPass     m_renderPass     = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;
    VkPipelineLayout m_skinnedLayout  = VK_NULL_HANDLE;
    VkPipeline       m_skinnedPipeline = VK_NULL_HANDLE;
    VkPipeline       m_cutoutPipeline  = VK_NULL_HANDLE;
    VkPipelineLayout m_skyLayout       = VK_NULL_HANDLE;
    VkPipeline       m_skyPipeline     = VK_NULL_HANDLE;
    VkPipelineLayout m_depthLayout     = VK_NULL_HANDLE;
    VkPipeline       m_depthPipeline   = VK_NULL_HANDLE;

    // Composite (scene HDR + bloom → swapchain) — created from a separate render pass
    // because the swapchain attachment format is sRGB while the scene RP writes HDR.
    VkRenderPass          m_compositeRenderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_compositeSetLayout  = VK_NULL_HANDLE;
    VkPipelineLayout      m_compositeLayout     = VK_NULL_HANDLE;
    VkPipeline            m_compositePipeline   = VK_NULL_HANDLE;

    void createRenderPass(VkDevice device, VkFormat hdrFormat, VkFormat depthFormat);
    void createCompositeRenderPass(VkDevice device, VkFormat swapchainFormat);
    void createCompositePipeline(VkDevice device);
    void createGraphicsPipeline(VkDevice device, const std::string& vertPath,
                                const std::string& fragPath,
                                const std::vector<VkDescriptorSetLayout>& setLayouts,
                                VkPipelineLayout& outLayout, VkPipeline& outPipeline,
                                VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT,
                                VkCompareOp     depthCompareOp    = VK_COMPARE_OP_LESS,
                                VkBool32        depthWriteEnable  = VK_TRUE);
    void createSkyPipeline(VkDevice device, VkDescriptorSetLayout globalLayout);
    void createDepthPrePassPipeline(VkDevice device, VkDescriptorSetLayout globalLayout);

    static std::vector<char> readShaderFile(const std::string& filepath);
    VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code);
};

} // namespace Nyx
