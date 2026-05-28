#include "renderer/HdrTarget.h"
#include "renderer/VulkanContext.h"
#include "Logger.h"

#include <stdexcept>

namespace Nyx {

void HdrTarget::init(VulkanContext& context, VkExtent2D extent) {
    create(context, extent);
    LOG_INFO("HdrTarget created ({}x{} R16G16B16A16_SFLOAT)", extent.width, extent.height);
}

void HdrTarget::cleanup(VkDevice device, VmaAllocator allocator) {
    destroy(device, allocator);
}

void HdrTarget::resize(VulkanContext& context, VkExtent2D extent) {
    destroy(context.getDevice(), context.getAllocator());
    create(context, extent);
}

void HdrTarget::create(VulkanContext& context, VkExtent2D extent) {
    m_extent = extent;

    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.extent        = {extent.width, extent.height, 1};
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.format        = m_format;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(context.getAllocator(), &info, &alloc, &m_image, &m_alloc, nullptr) != VK_SUCCESS)
        throw std::runtime_error("Failed to create HDR image");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = m_format;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;
    if (vkCreateImageView(context.getDevice(), &viewInfo, nullptr, &m_view) != VK_SUCCESS)
        throw std::runtime_error("Failed to create HDR view");

    VkSamplerCreateInfo sampler{};
    sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter    = VK_FILTER_LINEAR;
    sampler.minFilter    = VK_FILTER_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.minLod       = 0.0f;
    sampler.maxLod       = 0.0f;
    if (vkCreateSampler(context.getDevice(), &sampler, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("Failed to create HDR sampler");
}

void HdrTarget::destroy(VkDevice device, VmaAllocator allocator) {
    if (m_sampler != VK_NULL_HANDLE) { vkDestroySampler(device, m_sampler, nullptr);  m_sampler = VK_NULL_HANDLE; }
    if (m_view    != VK_NULL_HANDLE) { vkDestroyImageView(device, m_view, nullptr);   m_view    = VK_NULL_HANDLE; }
    if (m_image   != VK_NULL_HANDLE) { vmaDestroyImage(allocator, m_image, m_alloc);  m_image   = VK_NULL_HANDLE; m_alloc = VK_NULL_HANDLE; }
}

} // namespace Nyx
