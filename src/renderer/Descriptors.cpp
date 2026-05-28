#include "renderer/Descriptors.h"
#include "renderer/VulkanContext.h"
#include "renderer/UniformTypes.h"
#include "renderer/Texture.h"
#include "Logger.h"

#include <stdexcept>
#include <array>

namespace Nyx {

void Descriptors::init(VulkanContext& context) {
    createGlobalLayout(context.getDevice());
    createMaterialLayout(context.getDevice());
    createJointLayout(context.getDevice());
    createPool(context.getDevice());
    createUniformBuffers(context.getAllocator());
    createGlobalSets(context.getDevice());
    LOG_INFO("Descriptors initialized");
}

void Descriptors::cleanup(VkDevice device, VmaAllocator allocator) {
    for (auto& buf : m_materialUBOs) {
        buf.cleanup(allocator);
    }
    m_materialUBOs.clear();

    for (auto& buf : m_jointUBOs) {
        buf.cleanup(allocator);
    }
    m_jointUBOs.clear();

    for (auto& buf : m_uniformBuffers) {
        buf.cleanup(allocator);
    }
    m_uniformBuffers.clear();

    if (m_materialPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_materialPool, nullptr);
        m_materialPool = VK_NULL_HANDLE;
    }
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    if (m_materialLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_materialLayout, nullptr);
        m_materialLayout = VK_NULL_HANDLE;
    }
    if (m_jointLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_jointLayout, nullptr);
        m_jointLayout = VK_NULL_HANDLE;
    }
    if (m_globalLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_globalLayout, nullptr);
        m_globalLayout = VK_NULL_HANDLE;
    }
}

void Descriptors::createGlobalLayout(VkDevice device) {
    // Binding 0 = the global UBO (view/proj/lights/sky/lightSpace). Binding 1 = the
    // sun shadow map sampler (fragment stage only — vertex shaders don't sample it).
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding            = 0;
    bindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount    = 1;
    bindings[0].stageFlags         = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding            = 1;
    bindings[1].descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount    = 1;
    bindings[1].stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_globalLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create global descriptor set layout");
    }
}

void Descriptors::createMaterialLayout(VkDevice device) {
    // Binding 0 = baseColor, 1 = material UBO, 2 = normal, 3 = metal-rough, 4 = occlusion.
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    auto sampler = [](uint32_t b) {
        VkDescriptorSetLayoutBinding x{};
        x.binding = b; x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        x.descriptorCount = 1; x.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        return x;
    };
    bindings[0] = sampler(0);                          // base color
    bindings[1].binding         = 1;                   // material UBO
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2] = sampler(2);                          // normal
    bindings[3] = sampler(3);                          // metallic-roughness
    bindings[4] = sampler(4);                          // occlusion

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_materialLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create material descriptor set layout");
    }
}

void Descriptors::createJointLayout(VkDevice device) {
    // Set 2, binding 0: joint-matrix UBO, read in the vertex stage by the skinned pipeline.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding            = 0;
    binding.descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount    = 1;
    binding.stageFlags         = VK_SHADER_STAGE_VERTEX_BIT;
    binding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &binding;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_jointLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create joint descriptor set layout");
    }
}

void Descriptors::createPool(VkDevice device) {
    // Global pool (set 0): one UBO + one shadow sampler per frame in flight.
    {
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = Renderer::MAX_FRAMES_IN_FLIGHT;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = Renderer::MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
        poolInfo.pPoolSizes    = sizes.data();
        poolInfo.maxSets       = Renderer::MAX_FRAMES_IN_FLIGHT;

        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create global descriptor pool");
        }
    }

    // Material pool (set 1): one sampler + one UBO per material. Kept separate so it
    // can be reset (freeing every material set) on scene clear/reload without
    // disturbing the global sets. Sized generously for many materials + reassignments.
    {
        constexpr uint32_t MAX_MATERIAL_SETS = 512;
        constexpr uint32_t MAX_JOINT_SETS    = 128;  // joint sets (set 2) share this pool
        constexpr uint32_t SAMPLERS_PER_MAT  = 4;    // baseColor + normal + metalRough + occlusion
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = MAX_MATERIAL_SETS * SAMPLERS_PER_MAT;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[1].descriptorCount = MAX_MATERIAL_SETS + MAX_JOINT_SETS;  // material UBO + joint UBO

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
        poolInfo.pPoolSizes    = sizes.data();
        poolInfo.maxSets       = MAX_MATERIAL_SETS + MAX_JOINT_SETS;

        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_materialPool) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create material descriptor pool");
        }
    }
}

void Descriptors::resetMaterials(VkDevice device, VmaAllocator allocator) {
    vkResetDescriptorPool(device, m_materialPool, 0);   // frees every material + joint set at once
    for (auto& buf : m_materialUBOs) buf.cleanup(allocator);
    m_materialUBOs.clear();
    for (auto& buf : m_jointUBOs) buf.cleanup(allocator);
    m_jointUBOs.clear();
}

void Descriptors::createUniformBuffers(VmaAllocator allocator) {
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);
    m_uniformBuffers.resize(Renderer::MAX_FRAMES_IN_FLIGHT);

    for (int i = 0; i < Renderer::MAX_FRAMES_IN_FLIGHT; i++) {
        m_uniformBuffers[i].init(
            allocator, bufferSize,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
    }
}

void Descriptors::createGlobalSets(VkDevice device) {
    std::vector<VkDescriptorSetLayout> layouts(Renderer::MAX_FRAMES_IN_FLIGHT, m_globalLayout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = Renderer::MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts        = layouts.data();

    m_globalSets.resize(Renderer::MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(device, &allocInfo, m_globalSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate global descriptor sets");
    }

    // Point each descriptor set at its uniform buffer
    for (int i = 0; i < Renderer::MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uniformBuffers[i].getBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range  = sizeof(UniformBufferObject);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet           = m_globalSets[i];
        descriptorWrite.dstBinding       = 0;
        descriptorWrite.dstArrayElement  = 0;
        descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount  = 1;
        descriptorWrite.pBufferInfo      = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }
}

void Descriptors::setShadowMap(VkDevice device, VkImageView view, VkSampler sampler) {
    // Write the shadow map sampler into binding 1 of every frame's global set. The
    // image is shared across frames (rendered fresh each frame); the descriptor itself
    // is stable. Layout transitions are handled at the render-pass level.
    for (int i = 0; i < Renderer::MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo info{};
        info.imageView   = view;
        info.sampler     = sampler;
        info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_globalSets[i];
        w.dstBinding      = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &info;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }
}

VkDescriptorSet Descriptors::allocateMaterialSet(VulkanContext& context,
                                                  const MaterialMaps& maps, const MaterialParams& params) {
    VkDevice device       = context.getDevice();
    VmaAllocator allocator = context.getAllocator();

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_materialPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_materialLayout;

    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(device, &allocInfo, &set) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate material descriptor set");
    }

    // Per-material UBO in GPU-local memory, filled via a staging copy (same path as
    // meshes). Avoids host-visible coherency issues seen with once-written UBOs.
    m_materialUBOs.emplace_back();
    Buffer& matUBO = m_materialUBOs.back();
    matUBO.init(allocator, sizeof(MaterialParams),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_MEMORY_USAGE_GPU_ONLY);
    Buffer staging;
    staging.init(allocator, sizeof(MaterialParams),
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    staging.uploadData(allocator, &params, sizeof(MaterialParams));
    Buffer::copyBuffer(context, staging.getBuffer(), matUBO.getBuffer(), sizeof(MaterialParams));
    staging.cleanup(allocator);

    auto imgInfo = [](Texture* t) {
        VkDescriptorImageInfo i{};
        i.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        i.imageView   = t->getImageView();
        i.sampler     = t->getSampler();
        return i;
    };
    VkDescriptorImageInfo baseInfo = imgInfo(maps.baseColor);
    VkDescriptorImageInfo nrmInfo  = imgInfo(maps.normal);
    VkDescriptorImageInfo mrInfo   = imgInfo(maps.metalRough);
    VkDescriptorImageInfo aoInfo   = imgInfo(maps.occlusion);

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = matUBO.getBuffer();
    bufferInfo.offset = 0;
    bufferInfo.range  = sizeof(MaterialParams);

    std::array<VkWriteDescriptorSet, 5> writes{};
    auto sampWrite = [&](int idx, uint32_t binding, const VkDescriptorImageInfo* info) {
        writes[idx].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[idx].dstSet          = set;
        writes[idx].dstBinding      = binding;
        writes[idx].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[idx].descriptorCount = 1;
        writes[idx].pImageInfo      = info;
    };
    sampWrite(0, 0, &baseInfo);   // base color
    sampWrite(2, 2, &nrmInfo);    // normal
    sampWrite(3, 3, &mrInfo);     // metallic-roughness
    sampWrite(4, 4, &aoInfo);     // occlusion

    // Binding 1: material UBO
    writes[1].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet           = set;
    writes[1].dstBinding       = 1;
    writes[1].dstArrayElement  = 0;
    writes[1].descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].descriptorCount  = 1;
    writes[1].pBufferInfo      = &bufferInfo;

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    return set;
}

Descriptors::JointAlloc Descriptors::allocateJointSet(VkDevice device, VmaAllocator allocator) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_materialPool;   // shares the resettable pool
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_jointLayout;

    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(device, &allocInfo, &set) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate joint descriptor set");
    }

    // One joint-matrix UBO per skinned mesh; deque keeps the address stable so
    // SkinComponent::jointBuffer remains valid as more skins are allocated.
    m_jointUBOs.emplace_back();
    Buffer& jointUBO = m_jointUBOs.back();
    jointUBO.init(allocator, sizeof(glm::mat4) * MAX_JOINTS,
                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                  VMA_MEMORY_USAGE_CPU_TO_GPU);

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = jointUBO.getBuffer();
    bufferInfo.offset = 0;
    bufferInfo.range  = sizeof(glm::mat4) * MAX_JOINTS;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 0;
    write.dstArrayElement = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo     = &bufferInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    return {set, &jointUBO};
}

} // namespace Nyx
