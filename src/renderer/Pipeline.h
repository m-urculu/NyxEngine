#pragma once

// Pipeline.h — Graphics pipeline and render pass

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace VulkanEngine {

class VulkanContext;

class Pipeline {
public:
    void init(VulkanContext& context, VkExtent2D swapchainExtent, VkFormat swapchainFormat,
              VkFormat depthFormat, VkDescriptorSetLayout descriptorSetLayout);
    void cleanup(VkDevice device);

    void recreate(VulkanContext& context, VkExtent2D swapchainExtent, VkFormat swapchainFormat,
                  VkFormat depthFormat, VkDescriptorSetLayout descriptorSetLayout);

    VkRenderPass     getRenderPass()     const { return m_renderPass; }
    VkPipeline       getPipeline()       const { return m_pipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }

private:
    VkRenderPass     m_renderPass     = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    void createRenderPass(VkDevice device, VkFormat swapchainFormat, VkFormat depthFormat);
    void createGraphicsPipeline(VkDevice device, VkExtent2D swapchainExtent,
                                VkDescriptorSetLayout descriptorSetLayout);

    static std::vector<char> readShaderFile(const std::string& filepath);
    VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code);
};

} // namespace VulkanEngine
