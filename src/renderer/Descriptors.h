#pragma once

// Descriptors.h — Descriptor set layout, pool, sets, and uniform buffers

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include "renderer/Buffer.h"
#include "renderer/Renderer.h"

namespace VulkanEngine {

class VulkanContext;

class Descriptors {
public:
    void init(VulkanContext& context);
    void cleanup(VkDevice device, VmaAllocator allocator);

    VkDescriptorSetLayout getLayout() const { return m_layout; }
    VkDescriptorSet getSet(uint32_t frameIndex) const { return m_sets[frameIndex]; }
    Buffer& getUniformBuffer(uint32_t frameIndex) { return m_uniformBuffers[frameIndex]; }

private:
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool   = VK_NULL_HANDLE;

    std::vector<VkDescriptorSet> m_sets;
    std::vector<Buffer>          m_uniformBuffers;

    void createLayout(VkDevice device);
    void createPool(VkDevice device);
    void createUniformBuffers(VmaAllocator allocator);
    void createSets(VkDevice device);
};

} // namespace VulkanEngine
