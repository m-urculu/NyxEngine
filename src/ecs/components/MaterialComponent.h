#pragma once

// MaterialComponent.h — Per-entity material with texture descriptor set

#include <vulkan/vulkan.h>

namespace Talos {

class Texture;

struct MaterialComponent {
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    Texture* texture = nullptr; // non-owning
};

} // namespace Talos
