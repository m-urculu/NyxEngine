#include "renderer/Descriptors.h"
#include "renderer/VulkanContext.h"
#include "renderer/UniformTypes.h"
#include "renderer/Texture.h"
#include "Logger.h"

#include <stdexcept>
#include <array>

namespace Talos {

void Descriptors::init(VulkanContext& context) {
    createGlobalLayout(context.getDevice());
    createMaterialLayout(context.getDevice());
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

    for (auto& buf : m_uniformBuffers) {
        buf.cleanup(allocator);
    }
    m_uniformBuffers.clear();

    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    if (m_materialLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_materialLayout, nullptr);
        m_materialLayout = VK_NULL_HANDLE;
    }
    if (m_globalLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_globalLayout, nullptr);
        m_globalLayout = VK_NULL_HANDLE;
    }
}

void Descriptors::createGlobalLayout(VkDevice device) {
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding            = 0;
    uboBinding.descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount    = 1;
    uboBinding.stageFlags         = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    uboBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &uboBinding;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_globalLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create global descriptor set layout");
    }
}

void Descriptors::createMaterialLayout(VkDevice device) {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

    // Binding 0: combined image sampler (texture)
    bindings[0].binding            = 0;
    bindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount    = 1;
    bindings[0].stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[0].pImmutableSamplers = nullptr;

    // Binding 1: uniform buffer (material params)
    bindings[1].binding            = 1;
    bindings[1].descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount    = 1;
    bindings[1].stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_materialLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create material descriptor set layout");
    }
}

void Descriptors::createPool(VkDevice device) {
    // Pool sizes: UBOs for global sets + material UBOs, samplers for material sets
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = Renderer::MAX_FRAMES_IN_FLIGHT + 64; // global + material UBOs
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 64; // enough for many materials

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    poolInfo.maxSets       = Renderer::MAX_FRAMES_IN_FLIGHT + 64;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool");
    }
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

VkDescriptorSet Descriptors::allocateMaterialSet(VkDevice device, VmaAllocator allocator,
                                                  Texture& texture, const MaterialParams& params) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_materialLayout;

    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(device, &allocInfo, &set) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate material descriptor set");
    }

    // Create per-material UBO and upload params
    m_materialUBOs.emplace_back();
    Buffer& matUBO = m_materialUBOs.back();
    matUBO.init(allocator, sizeof(MaterialParams),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU);
    matUBO.uploadData(allocator, &params, sizeof(MaterialParams));

    // Descriptor writes: binding 0 = texture, binding 1 = material UBO
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView   = texture.getImageView();
    imageInfo.sampler     = texture.getSampler();

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = matUBO.getBuffer();
    bufferInfo.offset = 0;
    bufferInfo.range  = sizeof(MaterialParams);

    std::array<VkWriteDescriptorSet, 2> writes{};

    // Binding 0: texture sampler
    writes[0].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet           = set;
    writes[0].dstBinding       = 0;
    writes[0].dstArrayElement  = 0;
    writes[0].descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount  = 1;
    writes[0].pImageInfo       = &imageInfo;

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

} // namespace Talos
