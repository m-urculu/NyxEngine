#include "ui/TitleBar.h"
#include "Logger.h"

#include <cstring>
#include <algorithm>
#include <cmath>

namespace Talos {

// ════════════════════════════════════════════════════════════════════════════
// LIFECYCLE
// ════════════════════════════════════════════════════════════════════════════

void TitleBar::init(VmaAllocator allocator, GLFWwindow* window) {
    m_allocator = allocator;
    m_window    = window;

    // Create resize cursors (nullptr = system default arrow, no custom arrow needed)
    m_cursorEW   = glfwCreateStandardCursor(GLFW_RESIZE_EW_CURSOR);
    m_cursorNS   = glfwCreateStandardCursor(GLFW_RESIZE_NS_CURSOR);
    m_cursorNWSE = glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR);
    m_cursorNESW = glfwCreateStandardCursor(GLFW_RESIZE_NESW_CURSOR);

    // Allocate GPU buffers large enough for title bar geometry + software cursor
    constexpr VkDeviceSize vertexBufSize = sizeof(UIVertex) * 512;
    constexpr VkDeviceSize indexBufSize  = sizeof(uint32_t) * 768;

    m_vertexBuffer.init(allocator, vertexBufSize,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_indexBuffer.init(allocator, indexBufSize,
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_buffersInitialized = true;

    LOG_INFO("TitleBar initialized");
}

void TitleBar::cleanup(VmaAllocator allocator) {
    if (m_buffersInitialized) {
        m_vertexBuffer.cleanup(allocator);
        m_indexBuffer.cleanup(allocator);
        m_buffersInitialized = false;
    }

    if (m_cursorEW)   { glfwDestroyCursor(m_cursorEW);   m_cursorEW   = nullptr; }
    if (m_cursorNS)   { glfwDestroyCursor(m_cursorNS);   m_cursorNS   = nullptr; }
    if (m_cursorNWSE) { glfwDestroyCursor(m_cursorNWSE); m_cursorNWSE = nullptr; }
    if (m_cursorNESW) { glfwDestroyCursor(m_cursorNESW); m_cursorNESW = nullptr; }
}

// ════════════════════════════════════════════════════════════════════════════
// UPDATE — rebuild geometry each frame
// ════════════════════════════════════════════════════════════════════════════

void TitleBar::update(float windowWidth, float windowHeight, bool cursorActive) {
    m_windowWidth  = windowWidth;
    m_windowHeight = windowHeight;

    if (!m_visible) {
        m_indexCount = 0;
        return;
    }

    m_vertices.clear();
    m_indices.clear();

    // Get cursor position and detect hover only when active
    double mx = 0.0, my = 0.0;
    if (cursorActive) {
        glfwGetCursorPos(m_window, &mx, &my);
        m_hoverZone = hitTestButtons(mx, my);
    } else {
        m_hoverZone = HoverZone::None;
    }

    // Colors
    const glm::vec4 barColor      = {0.12f, 0.12f, 0.14f, 1.0f};
    const glm::vec4 hoverColor    = {0.40f, 0.40f, 0.45f, 1.0f};
    const glm::vec4 closeHover    = {0.90f, 0.15f, 0.15f, 1.0f};
    const glm::vec4 iconColor     = {0.85f, 0.85f, 0.85f, 1.0f};

    float w = windowWidth;

    // 1. Title bar background
    addQuad(0.0f, 0.0f, w, BAR_HEIGHT, barColor);

    // 2. Button backgrounds (right-aligned)
    float btnX = w - BUTTON_WIDTH * 3.0f;

    // Minimize button
    glm::vec4 minBg = (m_hoverZone == HoverZone::Minimize) ? hoverColor : barColor;
    addQuad(btnX, 0.0f, BUTTON_WIDTH, BAR_HEIGHT, minBg);

    // Maximize button
    glm::vec4 maxBg = (m_hoverZone == HoverZone::Maximize) ? hoverColor : barColor;
    addQuad(btnX + BUTTON_WIDTH, 0.0f, BUTTON_WIDTH, BAR_HEIGHT, maxBg);

    // Close button
    glm::vec4 closeBg = (m_hoverZone == HoverZone::Close) ? closeHover : barColor;
    addQuad(btnX + BUTTON_WIDTH * 2.0f, 0.0f, BUTTON_WIDTH, BAR_HEIGHT, closeBg);

    // 3. Button icons

    // Minimize icon: horizontal line (10px wide, 1.5px tall, centered)
    {
        float cx = btnX + BUTTON_WIDTH * 0.5f;
        float cy = BAR_HEIGHT * 0.5f;
        addQuad(cx - 5.0f, cy - 0.75f, 10.0f, 1.5f, iconColor);
    }

    // Maximize icon: square outline (10x10, 1.5px border, centered)
    {
        float cx = btnX + BUTTON_WIDTH * 1.5f;
        float cy = BAR_HEIGHT * 0.5f;
        float half = 5.0f;
        float t = 1.5f;
        addQuad(cx - half, cy - half, half * 2.0f, t, iconColor);
        addQuad(cx - half, cy + half - t, half * 2.0f, t, iconColor);
        addQuad(cx - half, cy - half, t, half * 2.0f, iconColor);
        addQuad(cx + half - t, cy - half, t, half * 2.0f, iconColor);
    }

    // Close icon: X from two properly rotated lines
    {
        float cx = btnX + BUTTON_WIDTH * 2.5f;
        float cy = BAR_HEIGHT * 0.5f;
        float half = 4.5f;
        float t = 1.5f;
        addLine(cx - half, cy - half, cx + half, cy + half, t, iconColor);
        addLine(cx + half, cy - half, cx - half, cy + half, t, iconColor);
    }


    // Upload to GPU
    if (!m_vertices.empty()) {
        m_vertexBuffer.uploadData(m_allocator, m_vertices.data(),
                                  m_vertices.size() * sizeof(UIVertex));
        m_indexBuffer.uploadData(m_allocator, m_indices.data(),
                                m_indices.size() * sizeof(uint32_t));
        m_indexCount = static_cast<uint32_t>(m_indices.size());
    } else {
        m_indexCount = 0;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// DRAW
// ════════════════════════════════════════════════════════════════════════════

void TitleBar::draw(VkCommandBuffer cmd) {
    if (!m_visible || m_indexCount == 0) return;

    VkBuffer buffers[] = { m_vertexBuffer.getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

// ════════════════════════════════════════════════════════════════════════════
// INPUT HANDLING
// ════════════════════════════════════════════════════════════════════════════

bool TitleBar::handleMouseButton(int button, int action) {
    if (!m_visible) return false;
    if (button != GLFW_MOUSE_BUTTON_LEFT) return false;

    double mx, my;
    glfwGetCursorPos(m_window, &mx, &my);

    if (action == GLFW_PRESS) {
        // Check buttons first
        HoverZone zone = hitTestButtons(mx, my);
        uint8_t edges = hitTestEdges(mx, my);

        if (zone == HoverZone::Close) {
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            return true;
        }
        if (zone == HoverZone::Minimize) {
            glfwIconifyWindow(m_window);
            return true;
        }
        if (zone == HoverZone::Maximize) {
            if (m_maximized) {
                glfwSetWindowPos(m_window, m_savedX, m_savedY);
                glfwSetWindowSize(m_window, m_savedW, m_savedH);
                m_maximized = false;
            } else {
                glfwGetWindowPos(m_window, &m_savedX, &m_savedY);
                glfwGetWindowSize(m_window, &m_savedW, &m_savedH);
                GLFWmonitor* monitor = glfwGetPrimaryMonitor();
                int waX, waY, waW, waH;
                glfwGetMonitorWorkarea(monitor, &waX, &waY, &waW, &waH);
                glfwSetWindowPos(m_window, waX, waY);
                glfwSetWindowSize(m_window, waW, waH);
                m_maximized = true;
            }
            return true;
        }

        // Check edge/corner resize
        if (edges != 0) {
            m_resizing = true;
            m_resizeEdges = edges;
            int winX, winY;
            glfwGetWindowPos(m_window, &winX, &winY);
            m_resizeStartScreenX = winX + mx;
            m_resizeStartScreenY = winY + my;
            m_resizeStartWinX = winX;
            m_resizeStartWinY = winY;
            glfwGetWindowSize(m_window, &m_resizeStartWinW, &m_resizeStartWinH);
            return true;
        }

        // Check title bar drag
        if (zone == HoverZone::Bar) {
            m_dragging = true;
            glfwGetCursorPos(m_window, &m_dragStartMouseX, &m_dragStartMouseY);
            glfwGetWindowPos(m_window, &m_dragStartWinX, &m_dragStartWinY);
            return true;
        }
    }

    if (action == GLFW_RELEASE) {
        if (m_dragging || m_resizing) {
            m_dragging = false;
            m_resizing = false;
            return true;
        }
    }

    return false;
}

void TitleBar::handleDragResize() {
    if (!m_visible) return;

    double mx, my;
    glfwGetCursorPos(m_window, &mx, &my);

    if (m_dragging) {
        // Use screen-space coordinates to avoid oscillation
        // (glfwSetWindowPos changes window pos, which shifts window-relative cursor)
        int winX, winY;
        glfwGetWindowPos(m_window, &winX, &winY);
        double screenX = winX + mx;
        double screenY = winY + my;
        double startScreenX = m_dragStartWinX + m_dragStartMouseX;
        double startScreenY = m_dragStartWinY + m_dragStartMouseY;
        int newX = m_dragStartWinX + static_cast<int>(screenX - startScreenX);
        int newY = m_dragStartWinY + static_cast<int>(screenY - startScreenY);
        glfwSetWindowPos(m_window, newX, newY);
    }

    if (m_resizing) {
        // Compute screen-space cursor using current window position
        int winX, winY;
        glfwGetWindowPos(m_window, &winX, &winY);
        double screenMX = winX + mx;
        double screenMY = winY + my;

        double dx = screenMX - m_resizeStartScreenX;
        double dy = screenMY - m_resizeStartScreenY;

        int newX = m_resizeStartWinX;
        int newY = m_resizeStartWinY;
        int newW = m_resizeStartWinW;
        int newH = m_resizeStartWinH;

        if (m_resizeEdges & EDGE_LEFT) {
            newX = m_resizeStartWinX + static_cast<int>(dx);
            newW = m_resizeStartWinW - static_cast<int>(dx);
        }
        if (m_resizeEdges & EDGE_RIGHT) {
            newW = m_resizeStartWinW + static_cast<int>(dx);
        }
        if (m_resizeEdges & EDGE_TOP) {
            newY = m_resizeStartWinY + static_cast<int>(dy);
            newH = m_resizeStartWinH - static_cast<int>(dy);
        }
        if (m_resizeEdges & EDGE_BOTTOM) {
            newH = m_resizeStartWinH + static_cast<int>(dy);
        }

        if (newW < MIN_WIDTH) {
            if (m_resizeEdges & EDGE_LEFT) newX -= (MIN_WIDTH - newW);
            newW = MIN_WIDTH;
        }
        if (newH < MIN_HEIGHT) {
            if (m_resizeEdges & EDGE_TOP) newY -= (MIN_HEIGHT - newH);
            newH = MIN_HEIGHT;
        }

        glfwSetWindowPos(m_window, newX, newY);
        glfwSetWindowSize(m_window, newW, newH);
    }
}

void TitleBar::updateCursorShape() {
    if (!m_visible) return;
    if (m_dragging || m_resizing) return;

    double mx, my;
    glfwGetCursorPos(m_window, &mx, &my);
    uint8_t edges = hitTestEdges(mx, my);

    GLFWcursor* desired = nullptr; // nullptr = system default arrow
    if (edges == (EDGE_LEFT | EDGE_TOP) || edges == (EDGE_RIGHT | EDGE_BOTTOM)) {
        desired = m_cursorNWSE;
    } else if (edges == (EDGE_RIGHT | EDGE_TOP) || edges == (EDGE_LEFT | EDGE_BOTTOM)) {
        desired = m_cursorNESW;
    } else if (edges & (EDGE_LEFT | EDGE_RIGHT)) {
        desired = m_cursorEW;
    } else if (edges & (EDGE_TOP | EDGE_BOTTOM)) {
        desired = m_cursorNS;
    }

    // Only call glfwSetCursor when transitioning to/from a resize cursor
    if (desired != nullptr && !m_customCursorSet) {
        glfwSetCursor(m_window, desired);
        m_customCursorSet = true;
    } else if (desired == nullptr && m_customCursorSet) {
        glfwSetCursor(m_window, nullptr); // restore system default
        m_customCursorSet = false;
    } else if (desired != nullptr && m_customCursorSet) {
        // Switching between resize shapes
        glfwSetCursor(m_window, desired);
    }
}

bool TitleBar::consumesMouse() const {
    if (!m_visible) return false;
    if (m_dragging || m_resizing) return true;

    double mx, my;
    glfwGetCursorPos(m_window, &mx, &my);

    if (hitTestEdges(mx, my) != 0) return true;

    return m_hoverZone != HoverZone::None;
}

// ════════════════════════════════════════════════════════════════════════════
// HELPERS
// ════════════════════════════════════════════════════════════════════════════

void TitleBar::addQuad(float x, float y, float w, float h, const glm::vec4& color) {
    uint32_t base = static_cast<uint32_t>(m_vertices.size());

    m_vertices.push_back({{x,     y},     color});
    m_vertices.push_back({{x + w, y},     color});
    m_vertices.push_back({{x + w, y + h}, color});
    m_vertices.push_back({{x,     y + h}, color});

    m_indices.push_back(base);
    m_indices.push_back(base + 1);
    m_indices.push_back(base + 2);
    m_indices.push_back(base + 2);
    m_indices.push_back(base + 3);
    m_indices.push_back(base);
}

void TitleBar::addLine(float x0, float y0, float x1, float y1, float thickness, const glm::vec4& color) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) return;

    // Perpendicular direction, scaled by half-thickness
    float nx = (-dy / len) * thickness * 0.5f;
    float ny = ( dx / len) * thickness * 0.5f;

    uint32_t base = static_cast<uint32_t>(m_vertices.size());

    m_vertices.push_back({{x0 + nx, y0 + ny}, color});
    m_vertices.push_back({{x1 + nx, y1 + ny}, color});
    m_vertices.push_back({{x1 - nx, y1 - ny}, color});
    m_vertices.push_back({{x0 - nx, y0 - ny}, color});

    m_indices.push_back(base);
    m_indices.push_back(base + 1);
    m_indices.push_back(base + 2);
    m_indices.push_back(base + 2);
    m_indices.push_back(base + 3);
    m_indices.push_back(base);
}

TitleBar::HoverZone TitleBar::hitTestButtons(double mx, double my) const {
    if (my < 0.0 || my > BAR_HEIGHT) return HoverZone::None;

    float btnX = m_windowWidth - BUTTON_WIDTH * 3.0f;

    if (mx >= btnX && mx < btnX + BUTTON_WIDTH) return HoverZone::Minimize;
    if (mx >= btnX + BUTTON_WIDTH && mx < btnX + BUTTON_WIDTH * 2.0f) return HoverZone::Maximize;
    if (mx >= btnX + BUTTON_WIDTH * 2.0f && mx < m_windowWidth) return HoverZone::Close;

    if (mx >= 0.0 && mx < m_windowWidth) return HoverZone::Bar;

    return HoverZone::None;
}

uint8_t TitleBar::hitTestEdges(double mx, double my) const {
    uint8_t edges = 0;
    float b = RESIZE_BORDER;

    if (mx < b)                  edges |= EDGE_LEFT;
    if (mx > m_windowWidth - b)  edges |= EDGE_RIGHT;
    if (my < b)                  edges |= EDGE_TOP;
    if (my > m_windowHeight - b) edges |= EDGE_BOTTOM;

    return edges;
}

} // namespace Talos
