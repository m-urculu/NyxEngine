#pragma once

// Mesh.h — GPU mesh with vertex and index buffers
//
// Uses the staging buffer pattern: data is first uploaded to a CPU-visible
// staging buffer, then copied to a GPU-only buffer for optimal performance.

#include "renderer/Buffer.h"
#include "renderer/Vertex.h"
#include <glm/glm.hpp>
#include <vector>

namespace Nyx {

class VulkanContext;

class Mesh {
public:
    void init(VulkanContext& context, const std::vector<Vertex>& vertices,
              const std::vector<uint32_t>& indices);
    void cleanup(VmaAllocator allocator);

    void draw(VkCommandBuffer commandBuffer) const;

    uint32_t getIndexCount() const { return m_indexCount; }

    // Local-space axis-aligned bounds (computed from the vertices) — used for
    // viewport ray-picking.
    const glm::vec3& boundsMin() const { return m_min; }
    const glm::vec3& boundsMax() const { return m_max; }

private:
    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    uint32_t m_indexCount = 0;
    glm::vec3 m_min{0.0f};
    glm::vec3 m_max{0.0f};
};

} // namespace Nyx
