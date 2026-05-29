#pragma once

// PointShadowMap.h — Per-light cube map for omnidirectional point-light shadows.
// Stores linear distance from the light (R32_SFLOAT) in a 6-layer cube image.
// The Engine renders the scene six times per shadowed point light (one per face)
// using the depth-only pipeline below; mesh.frag samples the resulting cube with
// the world-space direction and compares against its own distance to the light.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

namespace Nyx {

class VulkanContext;

// One render-frame's worth of work for a single shadowed point light: which
// pool slot to write into, where the light is, its far radius (= LightComponent
// radius), and the per-face view*projection matrices. Populated by the Engine
// each frame, drained by the Renderer.
struct PointShadowJob {
    int       slot;
    glm::vec3 lightPos;
    float     farRadius;
    glm::mat4 viewProj[6];
};

class PointShadowMap {
public:
    static constexpr uint32_t DEFAULT_RESOLUTION = 512;

    // Re-uses the global descriptor set layout (set 0) and the standard mesh
    // vertex format so the same vertex buffers can feed the shadow pipeline.
    // `resolution` is the per-face cube map size; callers pick a tier per
    // light (typically 128 / 256 / 512 / 1024 / 2048).
    void init(VulkanContext& context, VkDescriptorSetLayout globalLayout,
              uint32_t resolution = DEFAULT_RESOLUTION);
    void cleanup(VkDevice device, VmaAllocator allocator);

    uint32_t getResolution() const { return m_resolution; }

    // Per-face render-pass control. `face` is 0..5, matching the cube map face
    // order (+X, -X, +Y, -Y, +Z, -Z).
    void beginFace(VkCommandBuffer cmd, uint32_t face) const;
    void endFace(VkCommandBuffer cmd) const;

    // First-time-only: run begin/end with no draws on every face so the image
    // ends up in SHADER_READ_ONLY_OPTIMAL even before any light writes to it.
    // Without this, sampling an unwritten slot in mesh.frag would be a layout
    // mismatch (UNDEFINED → SHADER_READ_ONLY) flagged by validation.
    void prime(VkCommandBuffer cmd) const;

    VkImageView      getCubeView()       const { return m_cubeView; }
    VkSampler        getSampler()        const { return m_sampler; }
    VkPipeline       getPipeline()       const { return m_pipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_layout; }

private:
    uint32_t        m_resolution = DEFAULT_RESOLUTION;
    VkFormat        m_format     = VK_FORMAT_R32_SFLOAT;

    VkImage         m_image      = VK_NULL_HANDLE;
    VmaAllocation   m_alloc      = VK_NULL_HANDLE;
    VkImageView     m_cubeView   = VK_NULL_HANDLE;          // 6-layer cube view, for sampling
    VkImageView     m_faceViews[6] = {};                    // single-layer 2D views for rendering
    VkFramebuffer   m_framebuffers[6] = {};

    VkRenderPass    m_renderPass = VK_NULL_HANDLE;
    VkSampler       m_sampler    = VK_NULL_HANDLE;

    VkPipelineLayout m_layout    = VK_NULL_HANDLE;
    VkPipeline       m_pipeline  = VK_NULL_HANDLE;

    void createImageAndViews(VulkanContext& context);
    void createSampler(VkDevice device);
    void createRenderPass(VkDevice device);
    void createFramebuffers(VkDevice device);
    void createPipeline(VkDevice device, VkDescriptorSetLayout globalLayout);
};

} // namespace Nyx
