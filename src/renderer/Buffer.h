#pragma once

// Buffer.h — VMA-backed Vulkan buffer wrapper
//
// Wraps a VkBuffer + VmaAllocation pair. Supports:
// - GPU-only buffers (vertex/index buffers uploaded via staging)
// - CPU-visible buffers (uniform buffers mapped persistently)

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace VulkanEngine {

class VulkanContext;

class Buffer {
public:
    void init(VmaAllocator allocator, VkDeviceSize size,
              VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
    void cleanup(VmaAllocator allocator);

    // Upload data to a CPU-visible (mapped) buffer
    void uploadData(VmaAllocator allocator, const void* data, VkDeviceSize size);

    // Copy one buffer to another using the GPU
    static void copyBuffer(VulkanContext& context, VkBuffer srcBuffer,
                           VkBuffer dstBuffer, VkDeviceSize size);

    VkBuffer     getBuffer()     const { return m_buffer; }
    VkDeviceSize getSize()       const { return m_size; }

private:
    VkBuffer       m_buffer     = VK_NULL_HANDLE;
    VmaAllocation  m_allocation = VK_NULL_HANDLE;
    VkDeviceSize   m_size       = 0;
};

} // namespace VulkanEngine
