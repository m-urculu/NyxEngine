#pragma once

// Descriptors.h — Descriptor set layouts, pools, sets, and uniform buffers
//
// Set 0: Global UBO (view/proj + lighting) — one per frame in flight
// Set 1: Per-material (binding 0 = texture sampler, binding 1 = material UBO)

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include "renderer/Buffer.h"
#include "renderer/Renderer.h"

namespace Talos {

class VulkanContext;
class Texture;
struct MaterialParams;

class Descriptors {
public:
    void init(VulkanContext& context);
    void cleanup(VkDevice device, VmaAllocator allocator);

    VkDescriptorSetLayout getGlobalLayout()   const { return m_globalLayout; }
    VkDescriptorSetLayout getMaterialLayout()  const { return m_materialLayout; }

    // Backward-compatible alias
    VkDescriptorSetLayout getLayout() const { return m_globalLayout; }

    VkDescriptorSet getSet(uint32_t frameIndex) const { return m_globalSets[frameIndex]; }
    Buffer& getUniformBuffer(uint32_t frameIndex) { return m_uniformBuffers[frameIndex]; }

    // Allocate a material descriptor set bound to a texture + material UBO
    VkDescriptorSet allocateMaterialSet(VkDevice device, VmaAllocator allocator,
                                        Texture& texture, const MaterialParams& params);

private:
    // Global (set 0)
    VkDescriptorSetLayout m_globalLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_globalSets;
    std::vector<Buffer>          m_uniformBuffers;

    // Material (set 1)
    VkDescriptorSetLayout m_materialLayout = VK_NULL_HANDLE;
    std::vector<Buffer>   m_materialUBOs; // one per allocated material set

    // Shared pool for both global and material sets
    VkDescriptorPool m_pool = VK_NULL_HANDLE;

    void createGlobalLayout(VkDevice device);
    void createMaterialLayout(VkDevice device);
    void createPool(VkDevice device);
    void createUniformBuffers(VmaAllocator allocator);
    void createGlobalSets(VkDevice device);
};

} // namespace Talos
