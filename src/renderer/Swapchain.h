#pragma once

// Swapchain.h — Manages the Vulkan swapchain
//
// The swapchain is a queue of images that are presented to the screen.
// Think of it like a filmstrip:
// - The GPU renders to one image while the display shows another
// - When a frame is done, the images swap (hence "swapchain")
// - Double buffering = 2 images, triple buffering = 3 images
//
// The swapchain must be recreated when the window is resized.

#include <vulkan/vulkan.h>
#include <vector>

namespace VulkanEngine {

class VulkanContext;

class Swapchain {
public:
    void init(VulkanContext& context, int windowWidth, int windowHeight);
    void cleanup(VkDevice device);

    // Recreate the swapchain (e.g., after window resize)
    void recreate(VulkanContext& context, int windowWidth, int windowHeight);

    // ── Accessors ──────────────────────────────────────────────────────────
    VkSwapchainKHR   getSwapchain()   const { return m_swapchain; }
    VkFormat         getImageFormat()  const { return m_imageFormat; }
    VkExtent2D       getExtent()      const { return m_extent; }
    const std::vector<VkImageView>& getImageViews() const { return m_imageViews; }

private:
    VkSwapchainKHR          m_swapchain  = VK_NULL_HANDLE;
    VkFormat                m_imageFormat;
    VkExtent2D              m_extent;
    std::vector<VkImage>    m_images;      // Owned by the swapchain, not us
    std::vector<VkImageView> m_imageViews;  // We create and destroy these

    // ── Helpers ────────────────────────────────────────────────────────────
    void createSwapchain(VulkanContext& context, int windowWidth, int windowHeight);
    void createImageViews(VkDevice device);

    // Choose optimal settings from what the device supports
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);
    VkPresentModeKHR   chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes);
    VkExtent2D         chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                                        int windowWidth, int windowHeight);
};

} // namespace VulkanEngine
