#pragma once

// TitleBar.h — Custom window title bar with minimize/maximize/close buttons,
// drag-to-move, and resizable edges/corners.

#include "ui/UIVertex.h"
#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>

namespace Talos {

class TitleBar {
public:
    static constexpr float BAR_HEIGHT    = 32.0f;
    static constexpr float BUTTON_WIDTH  = 46.0f;
    static constexpr float RESIZE_BORDER = 6.0f;
    static constexpr int   MIN_WIDTH     = 320;
    static constexpr int   MIN_HEIGHT    = 200;

    enum class HoverZone { None, Minimize, Maximize, Close, Bar };

    // Resize edge bitmask
    static constexpr uint8_t EDGE_LEFT   = 1;
    static constexpr uint8_t EDGE_RIGHT  = 2;
    static constexpr uint8_t EDGE_TOP    = 4;
    static constexpr uint8_t EDGE_BOTTOM = 8;

    void init(VmaAllocator allocator, GLFWwindow* window);
    void cleanup(VmaAllocator allocator);

    // Rebuild vertex/index data based on current hover state
    // cursorActive = false when cursor is captured (FPS mode) — disables hover detection
    void update(float windowWidth, float windowHeight, bool cursorActive);

    // Record draw commands into the given command buffer
    void draw(VkCommandBuffer cmd);

    // Input handling — returns true if the event was consumed
    bool handleMouseButton(int button, int action);

    // Call every frame when cursor is not captured to handle drag/resize
    void handleDragResize();

    // Update cursor shape based on mouse position (edges/corners)
    void updateCursorShape();

    // True when the title bar is consuming mouse input (dragging, resizing, hovering)
    bool consumesMouse() const;

    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const { return m_visible; }
    bool isResizing() const { return m_resizing; }

private:
    GLFWwindow*  m_window    = nullptr;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    bool         m_visible   = true;

    // Geometry buffers (CPU-visible, rebuilt each frame)
    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    bool   m_buffersInitialized = false;
    uint32_t m_indexCount = 0;

    std::vector<UIVertex> m_vertices;
    std::vector<uint32_t> m_indices;

    // Interaction state
    HoverZone m_hoverZone = HoverZone::None;
    bool      m_dragging  = false;
    bool      m_maximized = false;
    int       m_savedX = 0, m_savedY = 0, m_savedW = 0, m_savedH = 0;
    bool      m_resizing  = false;
    uint8_t   m_resizeEdges = 0;

    // Drag state
    double m_dragStartMouseX = 0.0;
    double m_dragStartMouseY = 0.0;
    int    m_dragStartWinX   = 0;
    int    m_dragStartWinY   = 0;

    // Resize state
    double m_resizeStartScreenX = 0.0;  // screen-space cursor at resize start
    double m_resizeStartScreenY = 0.0;
    int    m_resizeStartWinX   = 0;
    int    m_resizeStartWinY   = 0;
    int    m_resizeStartWinW   = 0;
    int    m_resizeStartWinH   = 0;

    // Current window dimensions (cached from update)
    float m_windowWidth  = 0.0f;
    float m_windowHeight = 0.0f;

    // Standard cursors (nullptr = system default arrow)
    GLFWcursor* m_cursorEW    = nullptr;
    GLFWcursor* m_cursorNS    = nullptr;
    GLFWcursor* m_cursorNWSE  = nullptr;
    GLFWcursor* m_cursorNESW  = nullptr;
    bool m_customCursorSet = false;

    // Helpers
    void addQuad(float x, float y, float w, float h, const glm::vec4& color);
    void addLine(float x0, float y0, float x1, float y1, float thickness, const glm::vec4& color);
    HoverZone hitTestButtons(double mx, double my) const;
    uint8_t   hitTestEdges(double mx, double my) const;
};

} // namespace Talos
