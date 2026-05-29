#pragma once

// UniformTypes.h — UBO struct for global data + push constants for per-object data
//
// Set 0, binding 0: Global UBO (view/proj + lighting)
// Push constants: Per-object model + normalMatrix (128 bytes)

#include <glm/glm.hpp>

namespace Nyx {

constexpr int MAX_LIGHTS         = 8;
constexpr int MAX_POINT_SHADOWS  = 4;     // cube map slots for point-light shadows
constexpr int MAX_JOINTS = 128;  // per-skin joint-matrix count (must match mesh_skinned.vert)
                                 // 128 mat4 = 8 KB UBO, safely under Vulkan's 16 KB min guarantee

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
    float hasNormalMap     = 0.0f;  // 1 = sample set-1 binding 2 (else use geometric normal)
    float hasMetalRoughMap = 0.0f;  // 1 = sample set-1 binding 3 (else use metallic/roughness factors)
    float alphaCutoff      = 0.0f;  // >0 = alpha-masked (cutout) at this threshold; 0 = opaque
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

    // Analytic-sky IBL: a 3-stop gradient (top / horizon / ground) sampled by direction.
    // Drives both the procedural skybox and the in-shader image-based lighting (replaces
    // the flat ambient — metallics now reflect the sky and the diffuse hemisphere shifts
    // with surface normal). rgb = color, w = intensity multiplier.
    glm::vec4 skyTop;
    glm::vec4 skyHorizon;
    glm::vec4 skyGround;

    // Sun-shadow light-space matrix: transforms a world-space fragment position into
    // the shadow map's clip space. Built CPU-side each frame from the directional
    // sun light's direction (orthographic projection looking along the sun).
    glm::mat4 lightSpace;
};

// Per-object data pushed per draw call — 128 bytes
struct PushConstants {
    glm::mat4 model;        // 64 bytes
    glm::mat4 normalMatrix; // 64 bytes
};

} // namespace Nyx
