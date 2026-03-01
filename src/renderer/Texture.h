#pragma once

// Texture.h — VkImage + VkImageView + VkSampler for texturing
//
// Supports loading from file (via stb_image) and procedural creation.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <string>

namespace Talos {

class VulkanContext;

class Texture {
public:
    void loadFromFile(VulkanContext& context, const std::string& filepath);
    void createDefault(VulkanContext& context); // 1x1 white texture
    void cleanup(VkDevice device, VmaAllocator allocator);

    VkImageView getImageView() const { return m_imageView; }
    VkSampler   getSampler()   const { return m_sampler; }

private:
    VkImage       m_image      = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkImageView   m_imageView  = VK_NULL_HANDLE;
    VkSampler     m_sampler    = VK_NULL_HANDLE;

    void createImage(VulkanContext& context, uint32_t width, uint32_t height,
                     const void* pixels);
    void createImageView(VkDevice device);
    void createSampler(VkDevice device);

    static void transitionImageLayout(VulkanContext& context, VkImage image,
                                      VkImageLayout oldLayout, VkImageLayout newLayout);
};

} // namespace Talos
