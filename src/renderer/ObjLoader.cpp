#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include "renderer/ObjLoader.h"
#include "Logger.h"

#include <unordered_map>
#include <stdexcept>

namespace VulkanEngine {

bool ObjLoader::load(const std::string& filepath,
                     std::vector<Vertex>& outVertices,
                     std::vector<uint32_t>& outIndices,
                     glm::vec3 defaultColor) {
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filepath.c_str())) {
        LOG_ERROR("Failed to load OBJ: {} {}", warn, err);
        return false;
    }

    if (!warn.empty()) {
        LOG_WARN("OBJ warning: {}", warn);
    }

    outVertices.clear();
    outIndices.clear();

    std::unordered_map<Vertex, uint32_t> uniqueVertices;

    for (const auto& shape : shapes) {
        for (const auto& index : shape.mesh.indices) {
            Vertex vertex{};

            // Position
            vertex.position = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };

            // Normal
            if (index.normal_index >= 0) {
                vertex.normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2]
                };
            } else {
                vertex.normal = {0.0f, 1.0f, 0.0f};
            }

            // Color — use vertex colors if available, otherwise default
            if (!attrib.colors.empty()) {
                vertex.color = {
                    attrib.colors[3 * index.vertex_index + 0],
                    attrib.colors[3 * index.vertex_index + 1],
                    attrib.colors[3 * index.vertex_index + 2]
                };
            } else {
                vertex.color = defaultColor;
            }

            // Vertex deduplication
            if (uniqueVertices.count(vertex) == 0) {
                uniqueVertices[vertex] = static_cast<uint32_t>(outVertices.size());
                outVertices.push_back(vertex);
            }
            outIndices.push_back(uniqueVertices[vertex]);
        }
    }

    LOG_INFO("Loaded OBJ: {} ({} unique vertices, {} indices)",
             filepath, outVertices.size(), outIndices.size());
    return true;
}

} // namespace VulkanEngine
