#pragma once

// DepthBuffer.h — VMA-backed depth image and view

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>

namespace VulkanEngine {

class VulkanContext;

class DepthBuffer {
public:
    void init(VulkanContext& context, VkExtent2D extent);
    void cleanup(VkDevice device, VmaAllocator allocator);

    VkImageView getImageView() const { return m_imageView; }
    VkFormat    getFormat()    const { return m_format; }

    static VkFormat findDepthFormat(VkPhysicalDevice physicalDevice);

private:
    VkImage       m_image      = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkImageView   m_imageView  = VK_NULL_HANDLE;
    VkFormat      m_format     = VK_FORMAT_UNDEFINED;

    static VkFormat findSupportedFormat(VkPhysicalDevice physicalDevice,
                                         const std::vector<VkFormat>& candidates,
                                         VkImageTiling tiling,
                                         VkFormatFeatureFlags features);
};

} // namespace VulkanEngine
