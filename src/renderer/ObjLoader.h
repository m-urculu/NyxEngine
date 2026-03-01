#pragma once

// ObjLoader.h — Load OBJ models using tinyobjloader

#include "renderer/Vertex.h"
#include <vector>
#include <string>

namespace VulkanEngine {

class ObjLoader {
public:
    // Load an OBJ file. Performs vertex deduplication.
    // The default color is applied to vertices that don't have material colors.
    static bool load(const std::string& filepath,
                     std::vector<Vertex>& outVertices,
                     std::vector<uint32_t>& outIndices,
                     glm::vec3 defaultColor = {0.7f, 0.7f, 0.7f});
};

} // namespace VulkanEngine
