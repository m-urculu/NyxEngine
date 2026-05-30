#pragma once

// MaterialComponent.h — Per-entity material with texture descriptor set and PBR params

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>

namespace Nyx {

class Texture;
class Buffer;

struct MaterialComponent {
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    Texture* texture     = nullptr;     // non-owning
    Buffer*  materialUBO = nullptr;     // non-owning; non-null only for host-visible (re-uploadable) sets

    // PBR-ish parameters
    glm::vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic  = 0.0f;
    float roughness = 0.5f;
    float alphaCutoff = 0.0f;   // >0 = alpha-masked: render two-sided via the cutout pipeline
    float subsurface = 0.0f;    // 0 = off; >0 = wrap-diffuse + back-translucency (skin/wax/foliage)

    // Display name of the albedo source, shown in the editor Inspector
    // (e.g. "logo.png" / "stone.mat"); empty means the default white texture.
    std::string albedoName;

    // Loadable path of the albedo texture (project-relative), persisted in scenes
    // so the texture can be reloaded. Empty means the default white texture.
    std::string albedoPath;

    // Loadable paths of the PBR data maps (linear), persisted so the full material
    // survives scene save/load and undo/redo. Empty means the map is absent (the
    // shader falls back to the default white texture + the metallic/roughness factors).
    std::string normalPath;
    std::string metalRoughPath;
    std::string occlusionPath;
};

} // namespace Nyx
