#pragma once

// GltfLoader.h — Load glTF 2.0 models (positions, normals, texcoords, indices)
//
// Uses cgltf (header-only, C99) for parsing.
// Extracts mesh geometry and base color texture URI.

#include "renderer/Vertex.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace Talos {

struct GltfMeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::string           baseColorTextureURI; // empty if none

    // PBR material params
    glm::vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor  = 0.0f;
    float roughnessFactor = 0.5f;
};

class GltfLoader {
public:
    // Load all meshes from a glTF file. Returns one GltfMeshData per primitive.
    static std::vector<GltfMeshData> load(const std::string& filepath);
};

} // namespace Talos
