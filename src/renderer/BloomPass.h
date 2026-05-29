#pragma once

// BloomPass.h — Mip-chain bloom (Call of Duty / Unreal style).
//
// Flow each frame, after the HDR scene RP finishes:
//   1. Downsample chain: mip 0 = brightpass-extract from HDR scene; mips 1..N-1
//      are progressively downsampled (13-tap with Karis average against fireflies).
//   2. Upsample chain: from coarsest mip, sample with a 3x3 tent filter and add
//      into the next-finer mip (blend = ONE,ONE). Final result lives in mip 0,
//      which the composite pass samples and adds to the HDR scene before tonemap.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>

namespace Nyx {

class VulkanContext;

class BloomPass {
public:
    static constexpr uint32_t MAX_MIPS = 6;

    void init(VulkanContext& context, VkImageView hdrView, VkSampler hdrSampler,
              VkExtent2D sceneExtent, uint32_t mipCount = 5);
    void cleanup(VkDevice device, VmaAllocator allocator);
    void resize(VulkanContext& context, VkImageView hdrView, VkExtent2D sceneExtent);

    // Run the full downsample + upsample chain. Caller must NOT be inside another
    // render pass when this is called. threshold/knee drive the brightpass; both
    // come from the active EnvironmentComponent.
    void render(VkCommandBuffer cmd, float threshold, float knee);

    // Bloom result for the composite pass to sample.
    VkImageView getResultView() const { return m_mipViews.empty() ? VK_NULL_HANDLE : m_mipViews[0]; }
    VkSampler   getSampler()    const { return m_sampler; }

private:
    VkFormat       m_format     = VK_FORMAT_B10G11R11_UFLOAT_PACK32;   // cheap HDR
    uint32_t       m_mipCount   = 0;
    VkExtent2D     m_extent{};                                          // mip 0 size (= scene/2)

    VkImage        m_image       = VK_NULL_HANDLE;
    VmaAllocation  m_alloc       = VK_NULL_HANDLE;
    std::vector<VkImageView>   m_mipViews;
    std::vector<VkFramebuffer> m_framebuffers;
    std::vector<VkExtent2D>    m_mipExtents;

    VkSampler             m_sampler            = VK_NULL_HANDLE;
    VkRenderPass          m_downsampleRP       = VK_NULL_HANDLE;       // LOAD_OP=DONT_CARE
    VkRenderPass          m_upsampleRP         = VK_NULL_HANDLE;       // LOAD_OP=LOAD + additive blend

    VkDescriptorSetLayout m_setLayout          = VK_NULL_HANDLE;       // 1 binding: source sampler
    VkPipelineLayout      m_pipelineLayout     = VK_NULL_HANDLE;       // 1 set + push constants (mode + texel size)
    VkPipeline            m_downsamplePipeline = VK_NULL_HANDLE;
    VkPipeline            m_upsamplePipeline   = VK_NULL_HANDLE;

    // Descriptor sets for SAMPLING. We need to read from: HDR scene (for mip 0 down),
    // then mips 0..N-2 (for further down), then mips 1..N-1 (for up). One set per
    // unique source view, allocated once at init. Layout: scene first, then mip 0..N-1.
    VkDescriptorPool             m_pool       = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_sourceSets;                          // [0]=scene, [i+1]=mip i

    void createImageAndViews(VulkanContext& context);
    void createSampler(VkDevice device);
    void createRenderPasses(VkDevice device);
    void createFramebuffers(VkDevice device);
    void createPipelineAndSets(VkDevice device);
    void writeSourceSets(VkDevice device, VkImageView hdrView);

    void destroy(VkDevice device, VmaAllocator allocator);
};

} // namespace Nyx
