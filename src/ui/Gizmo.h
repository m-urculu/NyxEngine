#pragma once

// Gizmo.h — Screen-space translation gizmo overlay. The Engine projects the
// selected entity's origin and its three world-axis tips to screen pixels and
// feeds them here; the gizmo draws the X/Y/Z axis lines + arrowheads through the
// shared UIPipeline. Drawn in the UI pass just above the 3D view, below the panels.
// All hit-testing and drag math live in the Engine (it owns the camera + entity).

#include "ui/UIVertex.h"
#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>
#include <utility>

namespace Nyx {

class Gizmo {
public:
    void init(VmaAllocator allocator);
    void cleanup(VmaAllocator allocator);

    // Draws selection outlines (one bounding-box wireframe per selected object, as
    // screen-space edge pairs), the translation gizmo (when visible), and/or a
    // selection-marquee rectangle. origin + tips[3] are the gizmo center/axis tips;
    // hoverAxis (0/1/2) is highlighted; marqueeActive draws a box marqMin→marqMax.
    // lightIcons: screen-space {center, color} for each point light — drawn as a
    // small camera-facing "sun" glyph (always shown, regardless of `visible`).
    void update(bool visible, const glm::vec2& origin, const glm::vec2 tips[3], int hoverAxis,
                bool marqueeActive, const glm::vec2& marqMin, const glm::vec2& marqMax,
                const std::vector<std::pair<glm::vec2, glm::vec2>>& outlines,
                const std::vector<std::pair<glm::vec2, glm::vec4>>& lightIcons = {});
    void draw(VkCommandBuffer cmd);

private:
    VmaAllocator m_allocator = VK_NULL_HANDLE;

    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    bool     m_buffersInitialized = false;
    uint32_t m_indexCount = 0;
    std::vector<UIVertex> m_vertices;
    std::vector<uint32_t> m_indices;

    void addLine(const glm::vec2& a, const glm::vec2& b, float thickness, const glm::vec4& color);
    void addTri(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec4& color);
    void addQuad4(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec2& d, const glm::vec4& color);
    void addDisc(const glm::vec2& center, float radius, const glm::vec4& color);   // filled triangle-fan
    void addLightIcon(const glm::vec2& center, const glm::vec4& color);            // sun glyph for point lights
};

} // namespace Nyx
