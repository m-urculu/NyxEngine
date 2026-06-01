#include "ui/Gizmo.h"

#include <cmath>

namespace Nyx {

void Gizmo::init(VmaAllocator allocator) {
    m_allocator = allocator;
    constexpr VkDeviceSize vertexBufSize = sizeof(UIVertex) * 16384;  // gizmo + marquee + selection outlines
    constexpr VkDeviceSize indexBufSize  = sizeof(uint32_t) * 24576;
    m_vertexBuffer.init(allocator, vertexBufSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_indexBuffer.init(allocator, indexBufSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_buffersInitialized = true;
}

void Gizmo::cleanup(VmaAllocator allocator) {
    if (m_buffersInitialized) {
        m_vertexBuffer.cleanup(allocator);
        m_indexBuffer.cleanup(allocator);
        m_buffersInitialized = false;
    }
}

void Gizmo::update(bool visible, const glm::vec2& origin, const glm::vec2 tips[3], int hoverAxis,
                   bool marqueeActive, const glm::vec2& marqMin, const glm::vec2& marqMax,
                   const std::vector<std::pair<glm::vec2, glm::vec2>>& outlines,
                   const std::vector<std::pair<glm::vec2, glm::vec4>>& lightIcons) {
    m_vertices.clear();
    m_indices.clear();

    // Point-light icons — a camera-facing sun glyph in the light's colour. Always
    // shown (independent of `visible`, which gates the translation gizmo).
    for (const auto& li : lightIcons) addLightIcon(li.first, li.second);

    // Selection outlines — a bounding-box wireframe per selected object (orange).
    {
        const glm::vec4 selCol = {1.0f, 0.58f, 0.18f, 0.95f};
        for (const auto& e : outlines) addLine(e.first, e.second, 1.5f, selCol);
    }

    // Selection-marquee rectangle (drawn under the gizmo).
    if (marqueeActive) {
        const glm::vec4 fill = {0.42f, 1.00f, 0.66f, 0.12f};
        const glm::vec4 lineC = {0.42f, 1.00f, 0.66f, 0.85f};
        glm::vec2 a{marqMin.x, marqMin.y}, b{marqMax.x, marqMin.y},
                  c{marqMax.x, marqMax.y}, d{marqMin.x, marqMax.y};
        addQuad4(a, b, c, d, fill);
        addLine(a, b, 1.5f, lineC); addLine(b, c, 1.5f, lineC);
        addLine(c, d, 1.5f, lineC); addLine(d, a, 1.5f, lineC);
    }

    if (!visible) {
        if (!m_vertices.empty()) {
            m_vertexBuffer.uploadData(m_allocator, m_vertices.data(), m_vertices.size() * sizeof(UIVertex));
            m_indexBuffer.uploadData(m_allocator, m_indices.data(), m_indices.size() * sizeof(uint32_t));
            m_indexCount = (uint32_t)m_indices.size();
        } else m_indexCount = 0;
        return;
    }

    const glm::vec4 axisCol[3] = {
        {0.92f, 0.28f, 0.30f, 1.0f},   // X red
        {0.40f, 0.85f, 0.38f, 1.0f},   // Y green
        {0.36f, 0.56f, 0.96f, 1.0f},   // Z blue
    };
    const glm::vec4 hot = {1.0f, 0.86f, 0.25f, 1.0f};   // hovered/active axis → amber

    for (int a = 0; a < 3; ++a) {
        glm::vec4 c = (a == hoverAxis) ? hot : axisCol[a];
        glm::vec2 d = tips[a] - origin;
        float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 1.0f) continue;
        glm::vec2 dir = d / len;

        // Shaft stops short so the arrowhead sits at the tip.
        const float head = 11.0f;
        glm::vec2 shaftEnd = tips[a] - dir * head;
        addLine(origin, shaftEnd, 3.0f, c);

        // Arrowhead: triangle pointing along the axis.
        glm::vec2 n = {-dir.y, dir.x};
        glm::vec2 base = tips[a] - dir * head;
        addTri(tips[a], base + n * 5.0f, base - n * 5.0f, c);
    }

    // Center handle.
    addLine(origin + glm::vec2(-3, 0), origin + glm::vec2(3, 0), 6.0f, {0.85f, 0.85f, 0.9f, 1.0f});

    if (!m_vertices.empty()) {
        m_vertexBuffer.uploadData(m_allocator, m_vertices.data(), m_vertices.size() * sizeof(UIVertex));
        m_indexBuffer.uploadData(m_allocator, m_indices.data(), m_indices.size() * sizeof(uint32_t));
        m_indexCount = (uint32_t)m_indices.size();
    } else {
        m_indexCount = 0;
    }
}

void Gizmo::draw(VkCommandBuffer cmd) {
    if (m_indexCount == 0) return;
    VkBuffer buffers[] = { m_vertexBuffer.getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

// A thick line as a quad (solid shape). UIVertex: pos, color, local, data0, data1.
void Gizmo::addLine(const glm::vec2& a, const glm::vec2& b, float thickness, const glm::vec4& color) {
    glm::vec2 d = b - a;
    float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 0.001f) return;
    d /= len;
    glm::vec2 n = glm::vec2(-d.y, d.x) * (thickness * 0.5f);
    const glm::vec2 z2{0.0f, 0.0f};
    const glm::vec4 z4{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t base = (uint32_t)m_vertices.size();
    m_vertices.push_back({a + n, color, z2, z4, z4});
    m_vertices.push_back({b + n, color, z2, z4, z4});
    m_vertices.push_back({b - n, color, z2, z4, z4});
    m_vertices.push_back({a - n, color, z2, z4, z4});
    m_indices.push_back(base);     m_indices.push_back(base + 1); m_indices.push_back(base + 2);
    m_indices.push_back(base + 2); m_indices.push_back(base + 3); m_indices.push_back(base);
}

void Gizmo::addTri(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec4& color) {
    const glm::vec2 z2{0.0f, 0.0f};
    const glm::vec4 z4{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t base = (uint32_t)m_vertices.size();
    m_vertices.push_back({a, color, z2, z4, z4});
    m_vertices.push_back({b, color, z2, z4, z4});
    m_vertices.push_back({c, color, z2, z4, z4});
    m_indices.push_back(base); m_indices.push_back(base + 1); m_indices.push_back(base + 2);
}

void Gizmo::addQuad4(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec2& d, const glm::vec4& color) {
    const glm::vec2 z2{0.0f, 0.0f};
    const glm::vec4 z4{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t base = (uint32_t)m_vertices.size();
    m_vertices.push_back({a, color, z2, z4, z4});
    m_vertices.push_back({b, color, z2, z4, z4});
    m_vertices.push_back({c, color, z2, z4, z4});
    m_vertices.push_back({d, color, z2, z4, z4});
    m_indices.push_back(base);     m_indices.push_back(base + 1); m_indices.push_back(base + 2);
    m_indices.push_back(base + 2); m_indices.push_back(base + 3); m_indices.push_back(base);
}

void Gizmo::addDisc(const glm::vec2& center, float radius, const glm::vec4& color) {
    constexpr int SEG = 12;
    constexpr float TAU = 6.28318530718f;
    for (int k = 0; k < SEG; ++k) {
        float a0 = (float)k       / SEG * TAU;
        float a1 = (float)(k + 1) / SEG * TAU;
        addTri(center,
               center + glm::vec2(std::cos(a0), std::sin(a0)) * radius,
               center + glm::vec2(std::cos(a1), std::sin(a1)) * radius, color);
    }
}

void Gizmo::addLightIcon(const glm::vec2& center, const glm::vec4& color) {
    // Dark halo for contrast against bright sky, a colored core, and short rays —
    // reads as a little sun / point-light marker. Camera-facing by construction
    // (it's drawn in screen space at the light's projected position).
    const glm::vec4 dark = {0.0f, 0.0f, 0.0f, 0.55f};
    addDisc(center, 7.0f, dark);
    addDisc(center, 4.5f, color);
    constexpr float TAU = 6.28318530718f;
    for (int k = 0; k < 8; ++k) {
        float ang = (float)k / 8.0f * TAU;
        glm::vec2 d{std::cos(ang), std::sin(ang)};
        addLine(center + d * 6.0f, center + d * 10.0f, 2.6f, dark);   // dark underlay
        addLine(center + d * 6.0f, center + d * 10.0f, 1.4f, color);  // colored ray
    }
}

} // namespace Nyx
