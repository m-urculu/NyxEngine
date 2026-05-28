#include "renderer/Mesh.h"
#include "renderer/VulkanContext.h"
#include "Logger.h"

#include <stdexcept>

namespace Nyx {

void Mesh::init(VulkanContext& context, const std::vector<Vertex>& vertices,
                const std::vector<uint32_t>& indices) {
    m_indexCount = static_cast<uint32_t>(indices.size());
    VmaAllocator allocator = context.getAllocator();

    // Local-space AABB for ray-picking.
    if (!vertices.empty()) {
        m_min = m_max = vertices[0].position;
        for (const Vertex& v : vertices) {
            m_min = glm::min(m_min, v.position);
            m_max = glm::max(m_max, v.position);
        }
    }

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

void Mesh::draw(VkCommandBuffer commandBuffer) const {
    VkBuffer buffers[] = { m_vertexBuffer.getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, 0, 0, 0);
}

} // namespace Nyx
