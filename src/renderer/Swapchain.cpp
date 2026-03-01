#include "renderer/Swapchain.h"
#include "renderer/VulkanContext.h"
#include "Logger.h"

#include <algorithm>
#include <stdexcept>
#include <limits>

namespace VulkanEngine {

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC
// ════════════════════════════════════════════════════════════════════════════

void Swapchain::init(VulkanContext& context, int windowWidth, int windowHeight) {
    createSwapchain(context, windowWidth, windowHeight);
    createImageViews(context.getDevice());
    LOG_INFO("Swapchain created: {}x{}, {} images",
             m_extent.width, m_extent.height, m_images.size());
}

void Swapchain::cleanup(VkDevice device) {
    // Destroy image views (we created them, so we must destroy them)
    for (auto imageView : m_imageViews) {
        vkDestroyImageView(device, imageView, nullptr);
    }
    m_imageViews.clear();

    // Destroy the swapchain itself (this also destroys the swapchain images)
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

void Swapchain::recreate(VulkanContext& context, int windowWidth, int windowHeight) {
    // Wait for the GPU to finish using the old swapchain
    vkDeviceWaitIdle(context.getDevice());

    cleanup(context.getDevice());
    createSwapchain(context, windowWidth, windowHeight);
    createImageViews(context.getDevice());

    LOG_INFO("Swapchain recreated: {}x{}", m_extent.width, m_extent.height);
}

// ════════════════════════════════════════════════════════════════════════════
// PRIVATE
// ════════════════════════════════════════════════════════════════════════════

void Swapchain::createSwapchain(VulkanContext& context, int windowWidth, int windowHeight) {
    SwapchainSupportDetails support = context.querySwapchainSupport(context.getPhysicalDevice());

    // Choose the best format, present mode, and resolution
    VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
    VkPresentModeKHR   presentMode   = chooseSwapPresentMode(support.presentModes);
    VkExtent2D         extent        = chooseSwapExtent(support.capabilities, windowWidth, windowHeight);

    // Request one more image than the minimum for smoother rendering
    uint32_t imageCount = support.capabilities.minImageCount + 1;
    // But don't exceed the maximum (0 means no limit)
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface          = context.getSurface();
    createInfo.minImageCount    = imageCount;
    createInfo.imageFormat      = surfaceFormat.format;
    createInfo.imageColorSpace  = surfaceFormat.colorSpace;
    createInfo.imageExtent      = extent;
    createInfo.imageArrayLayers = 1;  // Always 1 unless doing stereoscopic 3D
    createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    // Handle the case where graphics and present queues are different families
    QueueFamilyIndices indices = context.findQueueFamilies(context.getPhysicalDevice());
    uint32_t queueFamilyIndices[] = { indices.graphicsFamily.value(), indices.presentFamily.value() };

    if (indices.graphicsFamily != indices.presentFamily) {
        // Images can be used by multiple queue families without explicit transfers
        createInfo.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices   = queueFamilyIndices;
    } else {
        // Images are owned by one queue family at a time (better performance)
        createInfo.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform   = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode    = presentMode;
    createInfo.clipped        = VK_TRUE;   // Don't render pixels hidden by other windows
    createInfo.oldSwapchain   = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(context.getDevice(), &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create swapchain");
    }

    // Retrieve the swapchain images (Vulkan may create more than we requested)
    vkGetSwapchainImagesKHR(context.getDevice(), m_swapchain, &imageCount, nullptr);
    m_images.resize(imageCount);
    vkGetSwapchainImagesKHR(context.getDevice(), m_swapchain, &imageCount, m_images.data());

    m_imageFormat = surfaceFormat.format;
    m_extent = extent;
}

void Swapchain::createImageViews(VkDevice device) {
    m_imageViews.resize(m_images.size());

    for (size_t i = 0; i < m_images.size(); i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image    = m_images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format   = m_imageFormat;

        // No swizzling — use the default channel mapping
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

        // We're using these as color targets, with one mip level, one layer
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &m_imageViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create image view");
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// CHOOSING OPTIMAL SETTINGS
// ════════════════════════════════════════════════════════════════════════════

VkSurfaceFormatKHR Swapchain::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    // Prefer BGRA8 with sRGB color space (standard for most displays)
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    // Fall back to whatever is available
    return formats[0];
}

VkPresentModeKHR Swapchain::chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) {
    // Prefer MAILBOX (triple buffering — low latency, no tearing)
    for (const auto& mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            LOG_INFO("Present mode: Mailbox (triple buffering)");
            return mode;
        }
    }
    // FIFO is guaranteed to be available (essentially V-Sync)
    LOG_INFO("Present mode: FIFO (V-Sync)");
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Swapchain::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                                        int windowWidth, int windowHeight) {
    // If the current extent is not the special value 0xFFFFFFFF,
    // the window manager has already set the size for us
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    // Otherwise, pick the resolution that best matches the window size
    VkExtent2D actualExtent = {
        static_cast<uint32_t>(windowWidth),
        static_cast<uint32_t>(windowHeight)
    };

    // Clamp to the supported range
    actualExtent.width  = std::clamp(actualExtent.width,
                                     capabilities.minImageExtent.width,
                                     capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height,
                                     capabilities.minImageExtent.height,
                                     capabilities.maxImageExtent.height);
    return actualExtent;
}

} // namespace VulkanEngine
