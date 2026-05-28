#include "renderer/Swapchain.h"
#include "renderer/VulkanContext.h"
#include "Logger.h"

#include <algorithm>
#include <stdexcept>
#include <limits>

namespace Nyx {

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC
// ════════════════════════════════════════════════════════════════════════════

void Swapchain::init(VulkanContext& context, int windowWidth, int windowHeight) {
    createSwapchain(context, windowWidth, windowHeight);
    createImageViews(context.getDevice());
    m_depthBuffer.init(context, m_extent);
    LOG_INFO("Swapchain created: {}x{}, {} images",
             m_extent.width, m_extent.height, m_images.size());
}

void Swapchain::cleanup(VkDevice device, VmaAllocator allocator) {
    m_depthBuffer.cleanup(device, allocator);

    for (auto imageView : m_imageViews) {
        vkDestroyImageView(device, imageView, nullptr);
    }
    m_imageViews.clear();

    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

void Swapchain::recreate(VulkanContext& context, int windowWidth, int windowHeight) {
    vkDeviceWaitIdle(context.getDevice());

    // Keep old swapchain handle for smooth transition
    VkSwapchainKHR oldSwapchain = m_swapchain;
    m_swapchain = VK_NULL_HANDLE;

    // Clean up image views and depth buffer (but not the old swapchain yet)
    m_depthBuffer.cleanup(context.getDevice(), context.getAllocator());
    for (auto imageView : m_imageViews) {
        vkDestroyImageView(context.getDevice(), imageView, nullptr);
    }
    m_imageViews.clear();

    // Create new swapchain, passing old handle for seamless transition
    createSwapchain(context, windowWidth, windowHeight, oldSwapchain);

    // Now destroy the old swapchain
    if (oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(context.getDevice(), oldSwapchain, nullptr);
    }

    createImageViews(context.getDevice());
    m_depthBuffer.init(context, m_extent);

    LOG_INFO("Swapchain recreated: {}x{}", m_extent.width, m_extent.height);
}

// ════════════════════════════════════════════════════════════════════════════
// PRIVATE
// ════════════════════════════════════════════════════════════════════════════

void Swapchain::createSwapchain(VulkanContext& context, int windowWidth, int windowHeight,
                                 VkSwapchainKHR oldSwapchain) {
    SwapchainSupportDetails support = context.querySwapchainSupport(context.getPhysicalDevice());

    VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
    VkPresentModeKHR   presentMode   = chooseSwapPresentMode(support.presentModes);
    VkExtent2D         extent        = chooseSwapExtent(support.capabilities, windowWidth, windowHeight);

    uint32_t imageCount = support.capabilities.minImageCount + 1;
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
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    QueueFamilyIndices indices = context.findQueueFamilies(context.getPhysicalDevice());
    uint32_t queueFamilyIndices[] = { indices.graphicsFamily.value(), indices.presentFamily.value() };

    if (indices.graphicsFamily != indices.presentFamily) {
        createInfo.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices   = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform   = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode    = presentMode;
    createInfo.clipped        = VK_TRUE;
    createInfo.oldSwapchain   = oldSwapchain;

    if (vkCreateSwapchainKHR(context.getDevice(), &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create swapchain");
    }

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

        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

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
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats[0];
}

VkPresentModeKHR Swapchain::chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) {
    // Uncapped: present without waiting for vblank so the engine runs at its
    // natural rate. Prefer IMMEDIATE (no sync at all — may tear), then MAILBOX
    // (uncapped render, latest frame shown, no tearing). FIFO (V-Sync) is the
    // spec-guaranteed fallback.
    bool hasImmediate = false, hasMailbox = false;
    for (VkPresentModeKHR m : modes) {
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) hasImmediate = true;
        if (m == VK_PRESENT_MODE_MAILBOX_KHR)   hasMailbox   = true;
    }

    if (hasImmediate) {
        LOG_INFO("Present mode: IMMEDIATE (uncapped, V-Sync off)");
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (hasMailbox) {
        LOG_INFO("Present mode: MAILBOX (uncapped, no tearing)");
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    LOG_INFO("Present mode: FIFO (V-Sync) — uncapped modes unavailable");
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Swapchain::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                                        int windowWidth, int windowHeight) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    VkExtent2D actualExtent = {
        static_cast<uint32_t>(windowWidth),
        static_cast<uint32_t>(windowHeight)
    };

    actualExtent.width  = std::clamp(actualExtent.width,
                                     capabilities.minImageExtent.width,
                                     capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height,
                                     capabilities.minImageExtent.height,
                                     capabilities.maxImageExtent.height);
    return actualExtent;
}

} // namespace Nyx
