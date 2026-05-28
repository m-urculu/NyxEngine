#pragma once

// Descriptors.h — Descriptor set layouts, pools, sets, and uniform buffers
//
// Set 0: Global UBO (view/proj + lighting) — one per frame in flight
// Set 1: Per-material (binding 0 = texture sampler, binding 1 = material UBO)

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <deque>
#include "renderer/Buffer.h"
#include "renderer/Renderer.h"

namespace Nyx {

class VulkanContext;
class Texture;
struct MaterialParams;

class Descriptors {
public:
    void init(VulkanContext& context);
    void cleanup(VkDevice device, VmaAllocator allocator);

    VkDescriptorSetLayout getGlobalLayout()   const { return m_globalLayout; }
    VkDescriptorSetLayout getMaterialLayout()  const { return m_materialLayout; }
    VkDescriptorSetLayout getJointLayout()     const { return m_jointLayout; }

    // A joint-matrix UBO + its descriptor set (set 2). The buffer is owned here
    // (stable address) and re-uploaded each frame by the skin runtime.
    struct JointAlloc { VkDescriptorSet set = VK_NULL_HANDLE; Buffer* buffer = nullptr; };
    JointAlloc allocateJointSet(VkDevice device, VmaAllocator allocator);

    // Backward-compatible alias
    VkDescriptorSetLayout getLayout() const { return m_globalLayout; }

    VkDescriptorSet getSet(uint32_t frameIndex) const { return m_globalSets[frameIndex]; }
    Buffer& getUniformBuffer(uint32_t frameIndex) { return m_uniformBuffers[frameIndex]; }

    // Bind the sun shadow map into set 0, binding 1 for all frames. Called once after
    // the ShadowMap is constructed; the image is sampled by mesh.frag for PCF compare.
    void setShadowMap(VkDevice device, VkImageView view, VkSampler sampler);

    // The textures bound into a material set. All must be non-null — pass the
    // ResourceCache default (white) texture for any map the material lacks.
    // Normal/metalRough are gated by MaterialParams flags; occlusion defaults to
    // white (=1.0 = no occlusion) when absent.
    struct MaterialMaps {
        Texture* baseColor  = nullptr;  // set 1, binding 0 (sRGB)
        Texture* normal     = nullptr;  // set 1, binding 2 (linear)
        Texture* metalRough = nullptr;  // set 1, binding 3 (linear)
        Texture* occlusion  = nullptr;  // set 1, binding 4 (linear)
    };

    // Allocate a material descriptor set: base-color + normal + metal-rough +
    // occlusion samplers, plus the material UBO (binding 1). The UBO is uploaded
    // into GPU-local memory via a staging copy (needs the context for the transfer).
    VkDescriptorSet allocateMaterialSet(VulkanContext& context,
                                        const MaterialMaps& maps, const MaterialParams& params);

    // Free ALL material sets + their UBOs (resets the material pool). Used when a
    // scene is cleared/reloaded so descriptors don't leak. Caller must vkDeviceWaitIdle
    // first — any set still referenced by an in-flight command buffer becomes invalid.
    void resetMaterials(VkDevice device, VmaAllocator allocator);

private:
    // Global (set 0)
    VkDescriptorSetLayout m_globalLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_globalSets;
    std::vector<Buffer>          m_uniformBuffers;

    // Material (set 1)
    VkDescriptorSetLayout m_materialLayout = VK_NULL_HANDLE;
    std::vector<Buffer>   m_materialUBOs; // one per allocated material set

    // Skinning (set 2): one joint-matrix UBO per skinned mesh. deque → stable
    // addresses so SkinComponent's Buffer* stays valid as more are allocated.
    VkDescriptorSetLayout m_jointLayout = VK_NULL_HANDLE;
    std::deque<Buffer>    m_jointUBOs;

    VkDescriptorPool m_pool         = VK_NULL_HANDLE;  // global sets (set 0)
    VkDescriptorPool m_materialPool = VK_NULL_HANDLE;  // material sets (set 1), resettable

    void createGlobalLayout(VkDevice device);
    void createMaterialLayout(VkDevice device);
    void createJointLayout(VkDevice device);
    void createPool(VkDevice device);
    void createUniformBuffers(VmaAllocator allocator);
    void createGlobalSets(VkDevice device);
};

} // namespace Nyx
