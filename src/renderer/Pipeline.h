#pragma once

// Pipeline.h — Graphics pipeline and render pass
//
// The graphics pipeline defines HOW things are rendered:
// - Vertex shader → processes each vertex (position, color)
// - Fragment shader → computes the color of each pixel
// - Rasterization → converts triangles to pixels
// - Color blending → combines new pixels with existing ones
//
// In Vulkan, the pipeline is almost entirely immutable once created.
// Changing ANY setting requires creating a new pipeline.
//
// The render pass defines WHAT we're rendering to (which images/attachments).

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace VulkanEngine {

class Pipeline {
public:
    void init(VkDevice device, VkExtent2D swapchainExtent, VkFormat swapchainFormat);
    void cleanup(VkDevice device);

    // Recreate after swapchain resize
    void recreate(VkDevice device, VkExtent2D swapchainExtent, VkFormat swapchainFormat);

    VkRenderPass     getRenderPass()     const { return m_renderPass; }
    VkPipeline       getPipeline()       const { return m_pipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }

private:
    VkRenderPass     m_renderPass     = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    void createRenderPass(VkDevice device, VkFormat swapchainFormat);
    void createGraphicsPipeline(VkDevice device, VkExtent2D swapchainExtent);

    // Read a compiled SPIR-V shader file into a byte buffer
    static std::vector<char> readShaderFile(const std::string& filepath);

    // Create a Vulkan shader module from SPIR-V bytecode
    VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code);
};

} // namespace VulkanEngine
