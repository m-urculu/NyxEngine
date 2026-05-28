#pragma once

// MaterialPreviewPipeline.h — renders a single lit, textured sphere for previewing
// a material asset (the "texture on a model" view). Self-contained: a combined
// image sampler (albedo) + push-constant camera/material params. Backface-culled
// with NO depth test (one convex sphere needs neither a depth buffer nor sorting).
// Shares the 3D render pass.

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>
#include <string>
#include <vector>

namespace Nyx {

class VulkanContext;

struct PreviewVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
    static VkVertexInputBindingDescription getBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions();
};

struct PreviewPush {
    glm::mat4 viewProj;
    glm::vec4 baseColor;
    glm::vec4 params;     // x = metallic, y = roughness
};

class MaterialPreviewPipeline {
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
