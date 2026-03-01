#pragma once

// UniformTypes.h — UBO struct for global data + push constants for per-object data
//
// Set 0, binding 0: Global UBO (view/proj + lighting)
// Push constants: Per-object model + normalMatrix (128 bytes)

#include <glm/glm.hpp>

namespace Talos {

constexpr int MAX_LIGHTS = 8;

// Per-light data in the global UBO (48 bytes, std140-aligned)
struct GpuLightData {
    glm::vec4 positionAndType;   // xyz=pos/dir, w=0(dir)/1(point)
    glm::vec4 colorAndIntensity; // xyz=color, w=intensity
    glm::vec4 params;            // x=radius, y/z/w=reserved
};

// Per-material params — set 1, binding 1 (32 bytes, std140-aligned)
struct MaterialParams {
    glm::vec4 baseColorFactor; // rgba
    float metallic;
    float roughness;
    float _pad[2];
};

// Global data shared across all draws — set 0, binding 0
struct UniformBufferObject {
    // Matrices (128 bytes)
    glm::mat4 view;
    glm::mat4 projection;

    // Legacy lighting fields kept for layout — ambient + camera
    glm::vec4 ambientColor;   // xyz = color, w = intensity
    glm::vec4 cameraPosition; // xyz = position, w unused

    // Multi-light array (8 * 48 = 384 bytes)
    GpuLightData lights[MAX_LIGHTS];

    // Light count (16 bytes, std140 ivec4)
    glm::ivec4 lightCountAndPad; // x = active count
};

// Per-object data pushed per draw call — 128 bytes
struct PushConstants {
    glm::mat4 model;        // 64 bytes
    glm::mat4 normalMatrix; // 64 bytes
};

} // namespace Talos
