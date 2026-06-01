#include "ui/TitleBar.h"
#include "ui/PixelFont.h"
#include "Window.h"
#include "Logger.h"

#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>

#ifdef _WIN32
  #define GLFW_EXPOSE_NATIVE_WIN32
  #include <GLFW/glfw3native.h>
  #include <windows.h>
#endif

namespace Nyx {

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
    m_cursorHand = glfwCreateStandardCursor(GLFW_HAND_CURSOR);

    // Allocate GPU buffers large enough for title bar geometry + the FPS overlay
    // (panel, pixel-font digits, and ~120 graph columns).
    constexpr VkDeviceSize vertexBufSize = sizeof(UIVertex) * 4096;
    constexpr VkDeviceSize indexBufSize  = sizeof(uint32_t) * 6144;

    for (int i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        m_vertexBuffers[i].init(allocator, vertexBufSize,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        m_indexBuffers[i].init(allocator, indexBufSize,
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    }
    m_buffersInitialized = true;

    LOG_INFO("TitleBar initialized");
}

void TitleBar::cleanup(VmaAllocator allocator) {
    if (m_buffersInitialized) {
        for (int i = 0; i < FRAMES_IN_FLIGHT; ++i) {
            m_vertexBuffers[i].cleanup(allocator);
            m_indexBuffers[i].cleanup(allocator);
        }
        m_buffersInitialized = false;
    }

    if (m_cursorEW)   { glfwDestroyCursor(m_cursorEW);   m_cursorEW   = nullptr; }
    if (m_cursorNS)   { glfwDestroyCursor(m_cursorNS);   m_cursorNS   = nullptr; }
    if (m_cursorNWSE) { glfwDestroyCursor(m_cursorNWSE); m_cursorNWSE = nullptr; }
    if (m_cursorNESW) { glfwDestroyCursor(m_cursorNESW); m_cursorNESW = nullptr; }
    if (m_cursorHand) { glfwDestroyCursor(m_cursorHand); m_cursorHand = nullptr; }
}

// ════════════════════════════════════════════════════════════════════════════
// UPDATE — rebuild geometry each frame
// ════════════════════════════════════════════════════════════════════════════

void TitleBar::update(float windowWidth, float windowHeight, bool cursorActive, float dt,
                      uint32_t frameIndex, float fpsRightInset, float leftInset, float topInset) {
    m_windowWidth   = windowWidth;
    m_windowHeight  = windowHeight;
    m_fpsRightInset = fpsRightInset;
    m_leftInset     = leftInset;
    m_topInset      = topInset;

    // Track FPS every frame (even when the bar is hidden) so the graph stays
    // continuous. We smooth the frame TIME and invert it — averaging 1/dt
    // directly is biased high under jitter (mean(1/dt) >> 1/mean(dt)), which made
    // the readout spike into the hundreds while the true rate was ~60.
    if (dt > 0.0001f) {
        m_dtSmoothed = (m_dtSmoothed <= 0.0f) ? dt : m_dtSmoothed * 0.9f + dt * 0.1f;
        m_fpsSmoothed = 1.0f / m_dtSmoothed;

        // Push to the graph on a fixed time cadence (not once per frame), so the
        // history spans FPS_GRAPH_SECONDS regardless of how high the frame rate is.
        constexpr float interval = FPS_GRAPH_SECONDS / static_cast<float>(FPS_HISTORY);
        m_fpsSampleAccum += dt;
        if (m_fpsSampleAccum >= interval) {
            m_fpsSampleAccum -= interval;
            m_fpsHistory[m_fpsHead] = m_fpsSmoothed;
            m_fpsHead = (m_fpsHead + 1) % FPS_HISTORY;
            if (m_fpsSamples < FPS_HISTORY) m_fpsSamples++;
        }
    }

    if (!m_visible) {
        m_drawFrame = static_cast<int>(frameIndex) % FRAMES_IN_FLIGHT;
        m_indexCounts[m_drawFrame] = 0;
        return;
    }

    m_vertices.clear();
    m_indices.clear();

    if (m_captionVisible) {
    // Get cursor position and detect hover only when active
    double mx = 0.0, my = 0.0;
    if (cursorActive) {
        glfwGetCursorPos(m_window, &mx, &my);
        m_hoverZone = hitTestButtons(mx, my);
    } else {
        m_hoverZone = HoverZone::None;
    }

    // Colors — authored in sRGB (ui.frag converts to linear for the sRGB swapchain).
    const glm::vec4 barColor   = {0.07f, 0.07f, 0.08f, 1.0f};  // dark banner
    const glm::vec4 hoverColor = {0.20f, 0.20f, 0.23f, 1.0f};  // subtle button hover
    const glm::vec4 closeHover = {0.77f, 0.17f, 0.11f, 1.0f};  // Windows close red (#C42B1C)
    const glm::vec4 iconColor  = {0.82f, 0.82f, 0.85f, 1.0f};
    const glm::vec4 closeIcon  = (m_hoverZone == HoverZone::Close)
                               ? glm::vec4{1.0f, 1.0f, 1.0f, 1.0f}   // white on red
                               : iconColor;

    float w = windowWidth;

    // 1. Title bar background
    addQuad(0.0f, 0.0f, w, BAR_HEIGHT, barColor);

    // 1b. Brand: logo mark + "Nyx Engine" on the left.
    {
        const float r   = 11.0f;
        const float lcx = 12.0f + r;
        const float lcy = std::floor(BAR_HEIGHT * 0.5f);
        addLogo(lcx, lcy, r);

        const glm::vec4 nyxCol{0.62f, 1.00f, 0.82f, 1.0f};   // mint, matches the mark
        const glm::vec4 engCol{0.80f, 0.83f, 0.89f, 1.0f};   // light grey
        const float s   = PixelFont::SCALE;
        const float adv = PixelFont::ADVANCE * s;
        const float tx  = lcx + r + 9.0f;
        const float ty  = std::floor(BAR_HEIGHT * 0.5f - PixelFont::CELL_H * s * 0.5f);
        addText(tx, ty, "Nyx", s, nyxCol);
        addText(tx + 4.0f * adv, ty, "Engine", s, engCol);   // 3 glyphs + 1 space
    }

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

    // 3. Button icons — SDF glyphs snapped to the pixel grid. Centers sit on the
    //    .5 pixel-center grid and stroke/extents are whole pixels, so each 1px
    //    axis-aligned stroke covers exactly one pixel row/column — crisp, with no
    //    soft 2px smear. (The diagonal close "X" keeps a tight 1px edge; a
    //    diagonal can't be both pixel-aligned and free of stair-stepping.)
    //    Layout codes live in data1.x: 2 = rounded-box outline, 3 = capsule.
    constexpr float STROKE = 1.0f;   // 1px stroke
    constexpr float SR     = STROKE * 0.5f;
    constexpr float PAD    = 2.0f;   // transparent AA padding (doesn't affect crispness)
    auto snap = [](float v) { return std::floor(v) + 0.5f; };   // to pixel-center grid
    const float cy = snap(BAR_HEIGHT * 0.5f);

    // Minimize: round-capped horizontal line, exactly 1px tall.
    {
        float cx = snap(btnX + BUTTON_WIDTH * 0.5f);
        float halfLen = 4.0f;
        addShape(cx, cy, halfLen + SR + PAD, SR + PAD, iconColor,
                 {-halfLen, 0.0f, halfLen, 0.0f}, {3.0f, SR, 0.0f, 0.0f});
    }

    // Maximize / restore: a single square when windowed; two stacked squares
    // (the OS "restore" glyph) when maximized. Integer extents + .5 center keep
    // every edge on a whole pixel.
    {
        float cx = snap(btnX + BUTTON_WIDTH * 1.5f);
        const glm::vec4 outline{2.0f, 0.0f, 0.0f, 0.0f}; // rounded-box outline
        const glm::vec4 capsule{3.0f, SR, 0.0f, 0.0f};   // round-capped line
        bool maxd = Nyx::isCustomMaximized(m_window);

        if (!maxd) {
            float half = 4.0f, radius = 1.0f;
            addShape(cx, cy, half + SR + PAD, half + SR + PAD, iconColor,
                     {half, half, radius, SR}, outline);
        } else {
            float h = 3.0f, d = 2.0f, radius = 1.0f;   // integer offset keeps edges crisp
            // Front square (offset down-left), full outline.
            addShape(snap(cx - d), snap(cy + d), h + SR + PAD, h + SR + PAD, iconColor,
                     {h, h, radius, SR}, outline);
            // Back square (up-right): only its top and right edges peek out.
            float reach = d + h + SR + PAD;
            addShape(cx, cy, reach, reach, iconColor,
                     {d - h, -d - h, d + h, -d - h}, capsule);   // top edge
            addShape(cx, cy, reach, reach, iconColor,
                     {d + h, -d - h, d + h, -d + h}, capsule);   // right edge
        }
    }

    // Close: two diagonal round-capped lines forming an X.
    {
        float cx = snap(btnX + BUTTON_WIDTH * 2.5f);
        float arm = 3.0f;
        glm::vec4 data1{3.0f, SR, 0.0f, 0.0f};          // capsule, radius SR
        float h = arm + SR + PAD;
        addShape(cx, cy, h, h, closeIcon, {-arm, -arm,  arm,  arm}, data1);
        addShape(cx, cy, h, h, closeIcon, { arm, -arm, -arm,  arm}, data1);
    }

    } // end if (m_captionVisible)

    // Everything above is the caption (bar, brand, buttons) — drawn on top.
    uint32_t capIndices = static_cast<uint32_t>(m_indices.size());

    // 4. FPS overlay (top-right, below the bar) — appended after the caption so it
    //    can be drawn as a separate, lower layer (under the editor, over the 3D view).
    buildFpsOverlay();

    // 5. Play/Stop button (top-center of the viewport). Lives in this same
    //    overlay range so it draws via drawFps() regardless of caption state.
    if (m_onPlayToggle) {
        double mx = 0.0, my = 0.0;
        if (cursorActive) glfwGetCursorPos(m_window, &mx, &my);
        // Centre between the content-browser (left) and the right dock (right).
        float left   = m_leftInset;
        float right  = m_windowWidth - m_fpsRightInset;
        float cx     = std::floor((left + right) * 0.5f - m_playW * 0.5f);
        m_playX = cx;
        m_playY = BAR_HEIGHT + m_topInset + 8.0f;   // below the editor tab bar when one is shown
        m_playHover = cursorActive && hitTestPlay(mx, my);
        buildPlayButton();
    } else {
        m_playHover = false;
    }

    // Upload into this in-flight frame's own buffer set (avoids tearing the FPS
    // graph when the GPU is still reading the previous frame's geometry).
    int f = static_cast<int>(frameIndex) % FRAMES_IN_FLIGHT;
    m_drawFrame = f;
    if (!m_vertices.empty()) {
        m_vertexBuffers[f].uploadData(m_allocator, m_vertices.data(),
                                      m_vertices.size() * sizeof(UIVertex));
        m_indexBuffers[f].uploadData(m_allocator, m_indices.data(),
                                     m_indices.size() * sizeof(uint32_t));
        m_indexCounts[f]    = static_cast<uint32_t>(m_indices.size());
        m_capIndexCounts[f] = capIndices;
    } else {
        m_indexCounts[f] = 0;
        m_capIndexCounts[f] = 0;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// DRAW
// ════════════════════════════════════════════════════════════════════════════

void TitleBar::draw(VkCommandBuffer cmd) {
    // Caption sub-range: indices [0, capCount).
    uint32_t cap = m_capIndexCounts[m_drawFrame];
    if (!m_visible || cap == 0) return;
    VkBuffer buffers[] = { m_vertexBuffers[m_drawFrame].getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffers[m_drawFrame].getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, cap, 1, 0, 0, 0);
}

void TitleBar::drawFps(VkCommandBuffer cmd) {
    // FPS sub-range: indices [capCount, total).
    uint32_t cap   = m_capIndexCounts[m_drawFrame];
    uint32_t total = m_indexCounts[m_drawFrame];
    if (!m_visible || total <= cap) return;
    VkBuffer buffers[] = { m_vertexBuffers[m_drawFrame].getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffers[m_drawFrame].getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, total - cap, 1, cap, 0, 0);
}

// ════════════════════════════════════════════════════════════════════════════
// INPUT HANDLING
// ════════════════════════════════════════════════════════════════════════════

bool TitleBar::handleMouseButton(int button, int action) {
    if (!m_visible) return false;
    if (button != GLFW_MOUSE_BUTTON_LEFT) return false;

    double mx, my;
    glfwGetCursorPos(m_window, &mx, &my);

    // Play/Stop button — handled before the caption guard, since the in-engine
    // caption is hidden whenever the OS draws the title bar (so m_captionVisible
    // is false during normal editing) but the play toolbar still floats over the
    // viewport and must stay clickable.
    if (action == GLFW_PRESS && m_onPlayToggle && hitTestPlay(mx, my)) {
        m_onPlayToggle();
        return true;
    }

    if (!m_captionVisible) return false;

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
            // Custom borderless maximize — SetWindowPos to work area directly,
            // bypassing the OS maximize state machine. See Window.cpp.
            Nyx::toggleCustomMaximize(m_window);
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
#ifdef _WIN32
            // Drag-from-custom-maximized: exit custom max first (which restores
            // to the saved windowed rect), then hand off the drag.
            if (Nyx::isCustomMaximized(m_window)) {
                Nyx::toggleCustomMaximize(m_window);
                HWND hwnd = glfwGetWin32Window(m_window);
                ReleaseCapture();
                SendMessage(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
                return true;
            }
#endif
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

int TitleBar::windowResizeCursor() const {
    // Window-edge resize is now handled natively (WM_NCHITTEST), so the OS shows
    // the correct resize cursor on the borders. Nothing for us to do here.
    return 0;
}

void TitleBar::applyCursor(int glfwShape) {
    GLFWcursor* c = nullptr;   // 0 / default → system arrow
    switch (glfwShape) {
        case GLFW_RESIZE_EW_CURSOR:   c = m_cursorEW;   break;
        case GLFW_RESIZE_NS_CURSOR:   c = m_cursorNS;   break;
        case GLFW_RESIZE_NWSE_CURSOR: c = m_cursorNWSE; break;
        case GLFW_RESIZE_NESW_CURSOR: c = m_cursorNESW; break;
        case GLFW_HAND_CURSOR:        c = m_cursorHand; break;
        default: break;
    }
    if (c != m_appliedCursor) {        // only touch the cursor on a transition
        glfwSetCursor(m_window, c);
        m_appliedCursor = c;
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
    // Solid fill (shape 0): local/data are unused by the shader for this case.
    const glm::vec2 zero2{0.0f, 0.0f};
    const glm::vec4 zero4{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t base = static_cast<uint32_t>(m_vertices.size());

    m_vertices.push_back({{x,     y},     color, zero2, zero4, zero4});
    m_vertices.push_back({{x + w, y},     color, zero2, zero4, zero4});
    m_vertices.push_back({{x + w, y + h}, color, zero2, zero4, zero4});
    m_vertices.push_back({{x,     y + h}, color, zero2, zero4, zero4});

    m_indices.push_back(base);
    m_indices.push_back(base + 1);
    m_indices.push_back(base + 2);
    m_indices.push_back(base + 2);
    m_indices.push_back(base + 3);
    m_indices.push_back(base);
}

void TitleBar::addTriangle(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c,
                           const glm::vec4& color) {
    // Solid fill (shape 0): local/data unused by the shader for this case.
    const glm::vec2 zero2{0.0f, 0.0f};
    const glm::vec4 zero4{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t base = static_cast<uint32_t>(m_vertices.size());
    m_vertices.push_back({{a.x, a.y}, color, zero2, zero4, zero4});
    m_vertices.push_back({{b.x, b.y}, color, zero2, zero4, zero4});
    m_vertices.push_back({{c.x, c.y}, color, zero2, zero4, zero4});
    m_indices.push_back(base);
    m_indices.push_back(base + 1);
    m_indices.push_back(base + 2);
}

void TitleBar::addShape(float cx, float cy, float halfW, float halfH,
                        const glm::vec4& color, const glm::vec4& data0,
                        const glm::vec4& data1) {
    // One quad centered at (cx,cy). Each vertex carries its offset from the
    // center in `local`, so the fragment shader receives per-pixel coordinates
    // to evaluate the SDF. halfW/halfH include a few px of AA padding.
    const glm::vec2 off[4] = {
        {-halfW, -halfH}, {halfW, -halfH}, {halfW, halfH}, {-halfW, halfH}
    };
    uint32_t base = static_cast<uint32_t>(m_vertices.size());

    for (const glm::vec2& o : off) {
        m_vertices.push_back({{cx + o.x, cy + o.y}, color, o, data0, data1});
    }

    m_indices.push_back(base);
    m_indices.push_back(base + 1);
    m_indices.push_back(base + 2);
    m_indices.push_back(base + 2);
    m_indices.push_back(base + 3);
    m_indices.push_back(base);
}

void TitleBar::addLogo(float cx, float cy, float r) {
    // Flat night disc (matte — no shine). A filled rounded rect whose corner
    // radius equals its half-extent is a circle (shape 1), so no specular term.
    const glm::vec4 disc{0.10f, 0.10f, 0.22f, 1.0f};
    addShape(cx, cy, r + 1.0f, r + 1.0f, disc, {r, r, r, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f});

    // Metatron node cluster (7 circles) with an "N" traced through it: pointy-top
    // hexagon + center. The two left circles stack into the N's left bar, the two
    // right into the right bar, the TL→BR diagonal runs through center; top &
    // bottom circles stay as Metatron nodes.
    const glm::vec4 mint{0.62f, 1.00f, 0.82f, 1.0f};
    const float rHex = r * 0.60f, c = 0.8660254f;
    const glm::vec2 M{cx, cy};
    const glm::vec2 TOP{cx, cy - rHex},               BOT{cx, cy + rHex};
    const glm::vec2 TR{cx + c * rHex, cy - 0.5f * rHex}, BR{cx + c * rHex, cy + 0.5f * rHex};
    const glm::vec2 BL{cx - c * rHex, cy + 0.5f * rHex}, TL{cx - c * rHex, cy - 0.5f * rHex};

    // "N" strokes (capsules in logo-local space), drawn under the balls.
    const float half = r + 1.0f;
    auto line = [&](const glm::vec2& a, const glm::vec2& b) {
        addShape(cx, cy, half, half, mint,
                 {a.x - cx, a.y - cy, b.x - cx, b.y - cy}, {3.0f, 0.9f, 0.0f, 0.0f});
    };
    line(BL, TL);   // left vertical
    line(TL, BR);   // diagonal (through the center circle)
    line(BR, TR);   // right vertical

    // All 7 balls on top.
    const float dr = std::max(1.8f, r * 0.17f);
    auto ball = [&](const glm::vec2& p) {
        addShape(p.x, p.y, dr + 1.0f, dr + 1.0f, mint, {dr, dr, dr, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f});
    };
    for (const glm::vec2& p : {M, TOP, TR, BR, BOT, BL, TL}) ball(p);
}

// ────────────────────────────────────────────────────────────────────────────
// FPS OVERLAY
// ────────────────────────────────────────────────────────────────────────────

void TitleBar::addGlyph(float x, float y, char c, float s, const glm::vec4& color) {
    const uint8_t* rows = PixelFont::glyphRows(c);
    if (!rows) return;
    for (int r = 0; r < PixelFont::CELL_H; ++r) {
        uint8_t bits = rows[r];
        for (int col = 0; col < PixelFont::CELL_W; ++col)
            if (bits & (1 << (PixelFont::CELL_W - 1 - col)))
                addQuad(x + col * s, y + r * s, s, s, color);
    }
}

float TitleBar::addText(float x, float y, const std::string& text, float s, const glm::vec4& color) {
    float cx = x;
    for (char c : text) {
        addGlyph(cx, y, c, s, color);
        cx += PixelFont::ADVANCE * s;
    }
    return cx - x - s;
}

float TitleBar::addNumber(float x, float y, int value, float s, const glm::vec4& color) {
    if (value < 0)    value = 0;
    if (value > 9999) value = 9999;
    return addText(x, y, std::to_string(value), s, color);
}

void TitleBar::buildFpsOverlay() {
    constexpr float pad   = 6.0f;
    const float     s     = PixelFont::SCALE;               // unified UI text scale
    const float     charW = PixelFont::ADVANCE * s;
    const float     textH = PixelFont::CELL_H * s;
    constexpr float gap   = 4.0f;
    constexpr float Gw    = static_cast<float>(FPS_HISTORY); // 120 columns @ 1px
    constexpr float Gh    = 22.0f;                           // graph height
    constexpr float PW    = Gw + 2.0f * pad;
    const float     PH    = pad + textH + gap + Gh + pad;

    // Right-aligned to the 3D viewport (inset past the right-hand dock) so the
    // overlay floats over the scene rather than bleeding under the dock panels.
    float px = std::floor(m_windowWidth - m_fpsRightInset - 12.0f - PW);
    float py = BAR_HEIGHT + m_topInset + 8.0f;              // below the bar (+ editor tab bar if shown)

    // Colors authored in sRGB (ui.frag converts to linear).
    const glm::vec4 panelBg  = {0.04f, 0.05f, 0.06f, 0.62f};
    const glm::vec4 fillCol  = {0.20f, 0.85f, 0.50f, 0.42f};
    const glm::vec4 lineCol  = {0.42f, 1.00f, 0.66f, 1.00f};
    const glm::vec4 textCol  = {0.95f, 0.96f, 0.98f, 1.00f};
    const glm::vec4 labelCol = {0.55f, 0.60f, 0.66f, 1.00f};

    // Panel background — rounded rect (SDF fill); quad padded 1px so corner AA
    // isn't clipped.
    {
        float hw = PW * 0.5f, hh = PH * 0.5f;
        addShape(px + hw, py + hh, hw + 1.0f, hh + 1.0f, panelBg,
                 {hw, hh, 6.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f});
    }

    // Numeric readout + "FPS" label — same font and size as everything else.
    int fpsInt = static_cast<int>(m_fpsSmoothed + 0.5f);
    addNumber(px + pad, py + pad, fpsInt, s, textCol);
    {
        float lblW = 3.0f * charW - s;                       // "FPS" = 3 glyphs
        addText(px + PW - pad - lblW, py + pad, "FPS", s, labelCol);
    }

    // History graph — oldest on the left, newest on the right.
    float gx      = px + pad;
    float gBottom = py + pad + textH + gap + Gh;

    float maxV = 60.0f;                                      // never scale below 60
    for (int i = 0; i < m_fpsSamples; ++i) maxV = std::max(maxV, m_fpsHistory[i]);
    // 10% headroom, rounded to a tidy 30-fps step, so a steady line sits below
    // the top edge rather than clipping against it.
    float niceMax = std::ceil(maxV * 1.1f / 30.0f) * 30.0f;

    for (int col = 0; col < FPS_HISTORY; ++col) {
        int age = FPS_HISTORY - 1 - col;                    // 0 = newest (rightmost)
        if (age >= m_fpsSamples) continue;                  // history not full yet
        int idx = ((m_fpsHead - 1 - age) % FPS_HISTORY + FPS_HISTORY) % FPS_HISTORY;
        float h = std::floor((m_fpsHistory[idx] / niceMax) * Gh + 0.5f);
        if (h < 1.0f) h = 1.0f;
        if (h > Gh)   h = Gh;
        float xcol = gx + static_cast<float>(col);
        addQuad(xcol, gBottom - h, 1.0f, h,    fillCol);    // filled column
        addQuad(xcol, gBottom - h, 1.0f, 1.0f, lineCol);    // bright cap = the "line"
    }
}

// ────────────────────────────────────────────────────────────────────────────
// PLAY / STOP BUTTON
// ────────────────────────────────────────────────────────────────────────────

void TitleBar::buildPlayButton() {
    const float x = m_playX, y = m_playY, w = m_playW, h = m_playH;
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float cx = std::floor(x + hw) + 0.5f;   // pixel-centered for crisp icon
    const float cy = std::floor(y + hh) + 0.5f;

    // Panel background — flat square-cornered fill, matching the engine's
    // min/max/close buttons (no rounded corners). Brightens on hover. Authored
    // in sRGB (ui.frag converts to linear).
    const glm::vec4 bg      = m_playHover ? glm::vec4{0.22f, 0.23f, 0.27f, 0.94f}
                                          : glm::vec4{0.11f, 0.12f, 0.14f, 0.86f};
    addQuad(x, y, w, h, bg);

    if (!m_playRunning) {
        // Green right-pointing play triangle.
        const glm::vec4 green{0.42f, 0.92f, 0.56f, 1.0f};
        addTriangle({cx - 4.0f, cy - 6.0f}, {cx - 4.0f, cy + 6.0f}, {cx + 6.0f, cy}, green);
    } else {
        // Red stop square.
        const glm::vec4 red{0.96f, 0.45f, 0.42f, 1.0f};
        addQuad(cx - 5.0f, cy - 5.0f, 10.0f, 10.0f, red);
    }
}

bool TitleBar::hitTestPlay(double mx, double my) const {
    if (!m_onPlayToggle) return false;
    return mx >= m_playX && mx < m_playX + m_playW &&
           my >= m_playY && my < m_playY + m_playH;
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

} // namespace Nyx
