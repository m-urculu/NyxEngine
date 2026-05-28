#include "renderer/Buffer.h"
#include "renderer/VulkanContext.h"
#include "Logger.h"

#include <stdexcept>
#include <cstring>

namespace Nyx {

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
    // Never memcpy past the allocation. A caller uploading over-capacity data (e.g.
    // a UI panel whose 1px-quad text overflows its buffer) would otherwise corrupt
    // memory and crash in memcpy. Clamp to the buffer size instead.
    if (size > m_size) size = m_size;
    void* mapped = nullptr;
    if (vmaMapMemory(allocator, m_allocation, &mapped) != VK_SUCCESS || !mapped) return;
    std::memcpy(mapped, data, static_cast<size_t>(size));
    // Flush so the write is visible to the GPU on NON-coherent host memory (no-op
    // when already HOST_COHERENT).
    vmaFlushAllocation(allocator, m_allocation, 0, VK_WHOLE_SIZE);
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

} // namespace Nyx
