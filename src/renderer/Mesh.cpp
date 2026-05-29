#include "renderer/Mesh.h"
#include "renderer/VulkanContext.h"
#include "Logger.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace Nyx {

void Mesh::init(VulkanContext& context, const std::vector<Vertex>& vertices,
                const std::vector<uint32_t>& indices) {
    m_indexCount = static_cast<uint32_t>(indices.size());
    VmaAllocator allocator = context.getAllocator();

    // Local-space AABB + retained positions/indices for per-triangle ray-picking.
    if (!vertices.empty()) {
        m_min = m_max = vertices[0].position;
        m_pickPositions.clear();
        m_pickPositions.reserve(vertices.size());
        for (const Vertex& v : vertices) {
            m_min = glm::min(m_min, v.position);
            m_max = glm::max(m_max, v.position);
            m_pickPositions.push_back(v.position);
        }
    }
    m_pickIndices = indices;

    // ── Vertex buffer (staging → GPU) ─────────────────────────────────────
    VkDeviceSize vertexSize = sizeof(Vertex) * vertices.size();

    Buffer stagingVB;
    stagingVB.init(allocator, vertexSize,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VMA_MEMORY_USAGE_CPU_ONLY);
    stagingVB.uploadData(allocator, vertices.data(), vertexSize);

    m_vertexBuffer.init(allocator, vertexSize,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY);

    Buffer::copyBuffer(context, stagingVB.getBuffer(), m_vertexBuffer.getBuffer(), vertexSize);
    stagingVB.cleanup(allocator);

    // ── Index buffer (staging → GPU) ──────────────────────────────────────
    VkDeviceSize indexSize = sizeof(uint32_t) * indices.size();

    Buffer stagingIB;
    stagingIB.init(allocator, indexSize,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VMA_MEMORY_USAGE_CPU_ONLY);
    stagingIB.uploadData(allocator, indices.data(), indexSize);

    m_indexBuffer.init(allocator, indexSize,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       VMA_MEMORY_USAGE_GPU_ONLY);

    Buffer::copyBuffer(context, stagingIB.getBuffer(), m_indexBuffer.getBuffer(), indexSize);
    stagingIB.cleanup(allocator);

    LOG_INFO("Mesh created: {} vertices, {} indices", vertices.size(), indices.size());
}

void Mesh::cleanup(VmaAllocator allocator) {
    m_indexBuffer.cleanup(allocator);
    m_vertexBuffer.cleanup(allocator);
}

bool Mesh::rayHit(const glm::vec3& ro, const glm::vec3& rd, float& outT) const {
    // Möller-Trumbore against every triangle. Linear; fine for typical
    // editor click-picks. If meshes grow huge this is where a BVH would go.
    const size_t n = m_pickIndices.size();
    if (n < 3 || m_pickPositions.empty()) return false;
    bool  hit     = false;
    float closest = std::numeric_limits<float>::infinity();
    for (size_t i = 0; i + 2 < n; i += 3) {
        const glm::vec3& a = m_pickPositions[m_pickIndices[i]];
        const glm::vec3& b = m_pickPositions[m_pickIndices[i + 1]];
        const glm::vec3& c = m_pickPositions[m_pickIndices[i + 2]];
        glm::vec3 e1 = b - a;
        glm::vec3 e2 = c - a;
        glm::vec3 p  = glm::cross(rd, e2);
        float det = glm::dot(e1, p);
        if (std::fabs(det) < 1e-8f) continue;         // ray parallel to triangle
        float invDet = 1.0f / det;
        glm::vec3 t = ro - a;
        float u = glm::dot(t, p) * invDet;
        if (u < 0.0f || u > 1.0f) continue;
        glm::vec3 q = glm::cross(t, e1);
        float v = glm::dot(rd, q) * invDet;
        if (v < 0.0f || u + v > 1.0f) continue;
        float td = glm::dot(e2, q) * invDet;
        if (td > 1e-5f && td < closest) { closest = td; hit = true; }
    }
    if (hit) outT = closest;
    return hit;
}

void Mesh::draw(VkCommandBuffer commandBuffer) const {
    VkBuffer buffers[] = { m_vertexBuffer.getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, 0, 0, 0);
}

} // namespace Nyx
