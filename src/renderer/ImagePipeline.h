#pragma once

// ImagePipeline.h — textured-quad pipeline for the asset preview. Unlike the UI
// pipeline it binds a combined image sampler (one descriptor set per previewed
// image) so it can show actual pixels: a flat image (mode 0) or a lit textured
// ball (mode 1). Shares the 3D render pass.

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>
#include <string>
#include <vector>
#include <cstdint>

namespace Nyx {

class VulkanContext;

struct ImageVertex {
    glm::vec2 pos;   // pixel coordinates
    glm::vec2 uv;
    static VkVertexInputBindingDescription getBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 2> getAttributeDescriptions();
};

struct ImagePush {
    glm::vec2 screenSize;
    glm::vec2 ballCenter;   // mode 1
    float     ballRadius;   // mode 1
    int32_t   mode;         // 0 = flat image, 1 = shaded textured ball
};

class ImagePipeline {
public:
    void init(VulkanContext& context, VkRenderPass renderPass);
    void cleanup(VkDevice device);
    void recreate(VulkanContext& context, VkRenderPass renderPass);

    VkPipeline            getPipeline() const { return m_pipeline; }
    VkPipelineLayout      getPipelineLayout() const { return m_pipelineLayout; }
    VkDescriptorSetLayout getDescriptorSetLayout() const { return m_setLayout; }

private:
    VkDescriptorSetLayout m_setLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline       = VK_NULL_HANDLE;

    void create(VkDevice device, VkRenderPass renderPass);
    static std::vector<char> readShaderFile(const std::string& filepath);
    VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code);
};

} // namespace Nyx
