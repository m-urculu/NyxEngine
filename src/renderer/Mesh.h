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

    // Möller-Trumbore ray vs every triangle in local space. Returns true if
    // the ray hits a face; outT is the ray parameter (matches the parameter
    // on the unnormalized world ray since invW is linear). Used by the
    // viewport click-pick after the cheap bbox reject.
    bool rayHit(const glm::vec3& ro, const glm::vec3& rd, float& outT) const;

private:
    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    uint32_t m_indexCount = 0;
    glm::vec3 m_min{0.0f};
    glm::vec3 m_max{0.0f};

    // CPU-side bind-pose triangle data retained for ray-picking. Skinned
    // meshes pick against bind pose (animated picking is a follow-up).
    std::vector<glm::vec3> m_pickPositions;
    std::vector<uint32_t>  m_pickIndices;
};

} // namespace Nyx
