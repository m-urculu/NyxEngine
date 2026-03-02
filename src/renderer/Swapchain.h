#pragma once

// Swapchain.h — Manages the Vulkan swapchain and depth buffer

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include "renderer/DepthBuffer.h"

namespace Talos {

class VulkanContext;

class Swapchain {
public:
    void init(VulkanContext& context, int windowWidth, int windowHeight);
    void cleanup(VkDevice device, VmaAllocator allocator);

    void recreate(VulkanContext& context, int windowWidth, int windowHeight);

    VkSwapchainKHR   getSwapchain()   const { return m_swapchain; }
    VkFormat         getImageFormat()  const { return m_imageFormat; }
    VkExtent2D       getExtent()      const { return m_extent; }
    const std::vector<VkImageView>& getImageViews() const { return m_imageViews; }

    DepthBuffer&       getDepthBuffer()       { return m_depthBuffer; }
    const DepthBuffer& getDepthBuffer() const { return m_depthBuffer; }

private:
    VkSwapchainKHR          m_swapchain  = VK_NULL_HANDLE;
    VkFormat                m_imageFormat;
    VkExtent2D              m_extent;
    std::vector<VkImage>    m_images;
    std::vector<VkImageView> m_imageViews;
    DepthBuffer             m_depthBuffer;

    void createSwapchain(VulkanContext& context, int windowWidth, int windowHeight,
                         VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
    void createImageViews(VkDevice device);

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);
    VkPresentModeKHR   chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes);
    VkExtent2D         chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                                        int windowWidth, int windowHeight);
};

} // namespace Talos
