#include "renderer/Buffer.h"
#include "renderer/VulkanContext.h"
#include "Logger.h"

#include <stdexcept>
#include <cstring>

namespace VulkanEngine {

void Buffer::init(VmaAllocator allocator, VkDeviceSize size,
                  VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage) {
    m_size = size;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size        = size;
    bufferInfo.usage       = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = memoryUsage;

    if (vmaCreateBuffer(allocator, &bufferInfo, &allocCreateInfo,
                        &m_buffer, &m_allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }
}

void Buffer::cleanup(VmaAllocator allocator) {
    if (m_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator, m_buffer, m_allocation);
        m_buffer = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
    }
}

void Buffer::uploadData(VmaAllocator allocator, const void* data, VkDeviceSize size) {
    void* mapped = nullptr;
    vmaMapMemory(allocator, m_allocation, &mapped);
    std::memcpy(mapped, data, static_cast<size_t>(size));
    vmaUnmapMemory(allocator, m_allocation);
}

void Buffer::copyBuffer(VulkanContext& context, VkBuffer srcBuffer,
                         VkBuffer dstBuffer, VkDeviceSize size) {
    VkCommandBuffer commandBuffer = context.beginSingleTimeCommands();

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    context.endSingleTimeCommands(commandBuffer);
}

} // namespace VulkanEngine
