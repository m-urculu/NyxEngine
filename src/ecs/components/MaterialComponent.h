#pragma once

// MaterialComponent.h — Per-entity material with texture descriptor set and PBR params

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace Talos {

class Texture;

struct MaterialComponent {
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    Texture* texture = nullptr; // non-owning

    // PBR-ish parameters
    glm::vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic  = 0.0f;
    float roughness = 0.5f;
};

} // namespace Talos
