#pragma once

// UIPipeline.h — Vulkan pipeline for 2D UI overlay (title bar, buttons, etc.)
//
// Shares the existing render pass from the 3D pipeline (does NOT own it).
// Differs from the 3D pipeline: no depth test, alpha blending, no face culling,
// UIVertex format, push constant = vec2 screenSize.

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace Nyx {

class VulkanContext;

class UIPipeline {
public:
    void init(VulkanContext& context, VkRenderPass renderPass);
    void cleanup(VkDevice device);
    void recreate(VulkanContext& context, VkRenderPass renderPass);

    VkPipeline       getPipeline()       const { return m_pipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }

private:
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    void createGraphicsPipeline(VkDevice device, VkRenderPass renderPass);

    static std::vector<char> readShaderFile(const std::string& filepath);
    VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code);
};

} // namespace Nyx
