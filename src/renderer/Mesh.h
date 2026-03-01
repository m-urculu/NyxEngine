#pragma once

// Mesh.h — GPU mesh with vertex and index buffers
//
// Uses the staging buffer pattern: data is first uploaded to a CPU-visible
// staging buffer, then copied to a GPU-only buffer for optimal performance.

#include "renderer/Buffer.h"
#include "renderer/Vertex.h"
#include <vector>

namespace VulkanEngine {

class VulkanContext;

class Mesh {
public:
    void init(VulkanContext& context, const std::vector<Vertex>& vertices,
              const std::vector<uint32_t>& indices);
    void cleanup(VmaAllocator allocator);

    void draw(VkCommandBuffer commandBuffer) const;

    uint32_t getIndexCount() const { return m_indexCount; }

private:
    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    uint32_t m_indexCount = 0;
};

} // namespace VulkanEngine
