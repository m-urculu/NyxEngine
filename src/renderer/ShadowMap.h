#pragma once

// ShadowMap.h — Sun shadow mapping. A single-cascade directional shadow map: an
// offscreen depth texture rendered from the sun's POV each frame, then sampled in
// mesh.frag with PCF to attenuate the directional light. The light-space matrix is
// supplied via the global UBO (`lightSpace`).

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace Nyx {

class VulkanContext;

class ShadowMap {
public:
    void init(VulkanContext& context, VkDescriptorSetLayout globalLayout, uint32_t resolution = 2048);
    void cleanup(VkDevice device, VmaAllocator allocator);

    // Begin the shadow render pass — call before drawing meshes from the sun's POV.
    // Cmd buffer must be in a recording state, outside any other render pass.
    void beginRenderPass(VkCommandBuffer cmd) const;
    void endRenderPass(VkCommandBuffer cmd) const;

    VkImageView      getView()           const { return m_view; }
    VkSampler        getSampler()        const { return m_sampler; }
    VkPipeline       getPipeline()       const { return m_pipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_layout; }
    uint32_t         getResolution()     const { return m_resolution; }

private:
    uint32_t        m_resolution    = 2048;
    VkFormat        m_format        = VK_FORMAT_D32_SFLOAT;

    VkImage         m_image         = VK_NULL_HANDLE;
    VmaAllocation   m_alloc         = VK_NULL_HANDLE;
    VkImageView     m_view          = VK_NULL_HANDLE;
    VkSampler       m_sampler       = VK_NULL_HANDLE;
    VkRenderPass    m_renderPass    = VK_NULL_HANDLE;
    VkFramebuffer   m_framebuffer   = VK_NULL_HANDLE;

    VkPipelineLayout m_layout       = VK_NULL_HANDLE;
    VkPipeline       m_pipeline     = VK_NULL_HANDLE;

    void createImage(VulkanContext& context);
    void createRenderPass(VkDevice device);
    void createFramebuffer(VkDevice device);
    void createSampler(VkDevice device);
    void createPipeline(VkDevice device, VkDescriptorSetLayout globalLayout);
};

} // namespace Nyx
