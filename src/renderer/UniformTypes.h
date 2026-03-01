#pragma once

// UniformTypes.h — UBO struct for MVP matrices + lighting data
//
// Single combined UBO at set 0, binding 0.
// Layout matches std140 alignment rules.

#include <glm/glm.hpp>

namespace VulkanEngine {

struct UniformBufferObject {
    // Matrices (4 x mat4 = 256 bytes)
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 normalMatrix;   // inverse transpose of model (as mat4 for alignment)

    // Lighting (4 x vec4 = 64 bytes)
    glm::vec4 lightDirection; // xyz = direction, w unused
    glm::vec4 lightColor;     // xyz = color, w = intensity
    glm::vec4 ambientColor;   // xyz = color, w = intensity
    glm::vec4 cameraPosition; // xyz = position, w unused
};

} // namespace VulkanEngine
