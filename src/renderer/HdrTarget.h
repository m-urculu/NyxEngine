#pragma once

// HdrTarget.h — Offscreen HDR color attachment (R16G16B16A16_SFLOAT) used as the
// main scene render target. Meshes/sky now write linear HDR here instead of straight
// to the sRGB swapchain. A later composite pass samples this, applies tonemapping
// (and bloom), and writes the final LDR result to the swapchain.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace Nyx {

class VulkanContext;

class HdrTarget {
public:
    void init(VulkanContext& context, VkExtent2D extent);
    void cleanup(VkDevice device, VmaAllocator allocator);

    void resize(VulkanContext& context, VkExtent2D extent);

    VkImage     getImage()   const { return m_image; }
    VkImageView getView()    const { return m_view; }
    VkSampler   getSampler() const { return m_sampler; }
    VkFormat    getFormat()  const { return m_format; }
    VkExtent2D  getExtent()  const { return m_extent; }

private:
    VkFormat        m_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkExtent2D      m_extent{};

    VkImage         m_image      = VK_NULL_HANDLE;
    VmaAllocation   m_alloc      = VK_NULL_HANDLE;
    VkImageView     m_view       = VK_NULL_HANDLE;
    VkSampler       m_sampler    = VK_NULL_HANDLE;

    void create(VulkanContext& context, VkExtent2D extent);
    void destroy(VkDevice device, VmaAllocator allocator);
};

} // namespace Nyx
