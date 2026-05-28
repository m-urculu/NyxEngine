#pragma once

// Texture.h — VkImage + VkImageView + VkSampler for texturing
//
// Supports loading from file (via stb_image) and procedural creation.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <string>

namespace Nyx {

class VulkanContext;

class Texture {
public:
    // srgb=true for color (base color) textures; false for data maps (normal,
    // metallic-roughness, occlusion) which must be sampled in linear space.
    void loadFromFile(VulkanContext& context, const std::string& filepath, bool srgb = true);
    void createDefault(VulkanContext& context); // 1x1 white texture
    void cleanup(VkDevice device, VmaAllocator allocator);

    VkImageView getImageView() const { return m_imageView; }
    VkSampler   getSampler()   const { return m_sampler; }
    uint32_t    getWidth()     const { return m_width; }
    uint32_t    getHeight()    const { return m_height; }

private:
    VkImage       m_image      = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkImageView   m_imageView  = VK_NULL_HANDLE;
    VkSampler     m_sampler    = VK_NULL_HANDLE;
    VkFormat      m_format     = VK_FORMAT_R8G8B8A8_SRGB;
    uint32_t      m_width = 0, m_height = 0;
    uint32_t      m_mipLevels  = 1;   // full mip chain — see Texture.cpp::createImage

    void createImage(VulkanContext& context, uint32_t width, uint32_t height,
                     const void* pixels);
    void createImageView(VkDevice device);
    void createSampler(VkDevice device);
    // Fills mips 1..N-1 by blit-chaining from mip 0 (which was uploaded from staging).
    void generateMipmaps(VulkanContext& context);

    static void transitionImageLayout(VulkanContext& context, VkImage image,
                                      VkImageLayout oldLayout, VkImageLayout newLayout);
};

} // namespace Nyx
