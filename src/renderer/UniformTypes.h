#pragma once

// UniformTypes.h — UBO struct for global data + push constants for per-object data
//
// Set 0, binding 0: Global UBO (view/proj + lighting)
// Push constants: Per-object model + normalMatrix (128 bytes)

#include <glm/glm.hpp>

namespace Talos {

// Global data shared across all draws — set 0, binding 0
struct UniformBufferObject {
    // Matrices (128 bytes)
    glm::mat4 view;
    glm::mat4 projection;

    // Lighting (64 bytes)
    glm::vec4 lightDirection; // xyz = direction, w unused
    glm::vec4 lightColor;     // xyz = color, w = intensity
    glm::vec4 ambientColor;   // xyz = color, w = intensity
    glm::vec4 cameraPosition; // xyz = position, w unused
};

// Per-object data pushed per draw call — 128 bytes
struct PushConstants {
    glm::mat4 model;        // 64 bytes
    glm::mat4 normalMatrix; // 64 bytes
};

} // namespace Talos
