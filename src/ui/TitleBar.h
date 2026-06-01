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
#include <array>
#include <functional>
#include <string>

namespace Nyx {

class TitleBar {
public:
    // OS title bar is drawn outside the client area now, so the engine
    // content starts at y=0 — no in-engine title-bar offset needed.
    static constexpr float BAR_HEIGHT    = 0.0f;
    static constexpr float BUTTON_WIDTH  = 46.0f;
    static constexpr float RESIZE_BORDER = 6.0f;
    static constexpr int   MIN_WIDTH     = 320;
    static constexpr int   MIN_HEIGHT    = 200;
    static constexpr int   FPS_HISTORY       = 120;    // graph width in samples (px)
    static constexpr float FPS_GRAPH_SECONDS = 20.0f;  // time span the full graph represents

    enum class HoverZone { None, Minimize, Maximize, Close, Bar };

    // Resize edge bitmask
    static constexpr uint8_t EDGE_LEFT   = 1;
    static constexpr uint8_t EDGE_RIGHT  = 2;
    static constexpr uint8_t EDGE_TOP    = 4;
    static constexpr uint8_t EDGE_BOTTOM = 8;

    void init(VmaAllocator allocator, GLFWwindow* window);
    void cleanup(VmaAllocator allocator);

    // Rebuild vertex/index data based on current hover state.
    // cursorActive = false when cursor is captured (FPS mode) — disables hover detection.
    // dt = last frame's delta time (seconds), used to drive the FPS overlay.
    // frameIndex = the in-flight frame about to render; geometry is uploaded into
    // that frame's own buffer so it isn't overwritten while the previous frame is
    // still being read by the GPU (this was causing the FPS bars to tear/flash).
    // fpsRightInset shifts the right-aligned FPS overlay left by this many pixels so
    // it floats over the 3D viewport instead of under the right-hand dock.
    void update(float windowWidth, float windowHeight, bool cursorActive, float dt,
                uint32_t frameIndex = 0, float fpsRightInset = 0.0f, float leftInset = 0.0f,
                float topInset = 0.0f);

    // Record draw commands into the given command buffer. The caption (bar, logo,
    // buttons) and the FPS overlay are separate sub-draws so the renderer can put
    // the FPS graph BELOW the editor (above the 3D view) while the caption stays
    // on top: call drawFps() early in the UI pass and draw() last.
    void draw(VkCommandBuffer cmd);      // caption only
    void drawFps(VkCommandBuffer cmd);   // FPS overlay only

    // Input handling — returns true if the event was consumed
    bool handleMouseButton(int button, int action);

    // Call every frame when cursor is not captured to handle drag/resize
    void handleDragResize();

    // Cursor coordination: windowResizeCursor() returns the GLFW standard-cursor
    // shape for the window edge under the mouse (or 0 = none). The engine combines
    // it with the panels' resize edges and calls applyCursor() once per frame.
    // GLFW_HAND_CURSOR is also supported — used for hover over clickable UI.
    int  windowResizeCursor() const;
    void applyCursor(int glfwShape);   // 0 = default arrow

    // True when the cursor is over one of the title-bar buttons (min/max/close)
    // or the Play/Stop button — Engine uses this to switch to the hand cursor
    // over clickable chrome.
    bool wantsPointerCursor() const {
        return (m_hoverZone != HoverZone::None && m_hoverZone != HoverZone::Bar) || m_playHover;
    }

    // ── Play/Stop toolbar ───────────────────────────────────────────────────
    // A small button floating at the top-center of the 3D viewport. The editor
    // wires setOnPlayToggle() to launch / stop the standalone game process and
    // calls setPlayRunning() each frame so the icon reflects the child's state.
    // The button is built into the overlay (FPS) draw range so it stays visible
    // even though the in-engine caption is hidden when the OS draws the title
    // bar. When no toggle callback is set (the game/play process itself), the
    // button is not drawn or hit-tested.
    void setOnPlayToggle(std::function<void()> cb) { m_onPlayToggle = std::move(cb); }
    void setPlayRunning(bool running) { m_playRunning = running; }

    // True when the title bar is consuming mouse input (dragging, resizing, hovering)
    bool consumesMouse() const;

    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const { return m_visible; }
    // Hide just the caption (logo, "Nyx Engine" text, min/max/close buttons)
    // while keeping the FPS overlay rendering. Used when the OS draws the
    // title bar but we still want the FPS readout.
    void setCaptionVisible(bool v) { m_captionVisible = v; }
    bool isCaptionVisible() const { return m_captionVisible; }
    bool isResizing() const { return m_resizing; }

private:
    GLFWwindow*  m_window    = nullptr;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    bool         m_visible        = true;
    bool         m_captionVisible = true;

    // Geometry buffers — one set per in-flight frame so re-uploading each frame
    // doesn't clobber a buffer the GPU is still reading (must match
    // Renderer::MAX_FRAMES_IN_FLIGHT).
    static constexpr int FRAMES_IN_FLIGHT = 2;
    Buffer   m_vertexBuffers[FRAMES_IN_FLIGHT];
    Buffer   m_indexBuffers[FRAMES_IN_FLIGHT];
    uint32_t m_indexCounts[FRAMES_IN_FLIGHT]    = {0, 0};  // total (caption + FPS)
    uint32_t m_capIndexCounts[FRAMES_IN_FLIGHT] = {0, 0};  // caption portion = [0, cap); FPS = [cap, total)
    int      m_drawFrame = 0;             // which set draw() should bind
    bool     m_buffersInitialized = false;

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
    float m_fpsRightInset = 0.0f;   // px to pull the FPS overlay left of the right dock
    float m_leftInset     = 0.0f;   // viewport left edge (content-browser width) for centering the play button
    float m_topInset      = 0.0f;   // px to push the play button + FPS overlay below the editor tab bar
    float m_windowHeight = 0.0f;

    // Play/Stop button state. Rect is recomputed each update() (centered over the
    // viewport); m_playHover drives the hover highlight + pointer cursor.
    std::function<void()> m_onPlayToggle;            // unset in the game/play process
    bool  m_playRunning = false;                     // child game process alive → show Stop
    bool  m_playHover   = false;
    float m_playX = 0.0f, m_playY = 0.0f, m_playW = 38.0f, m_playH = 24.0f;
    void  buildPlayButton();
    bool  hitTestPlay(double mx, double my) const;

    // FPS overlay state — ring buffer of recent per-frame FPS + a smoothed value
    // for the numeric readout.
    std::array<float, FPS_HISTORY> m_fpsHistory{};
    int   m_fpsSamples  = 0;     // how many slots are filled (ramps up to FPS_HISTORY)
    int   m_fpsHead     = 0;     // next write index
    float m_dtSmoothed    = 0.0f;  // EMA of frame TIME (seconds) — invert for true FPS
    float m_fpsSmoothed   = 0.0f;  // 1 / m_dtSmoothed, shown as the big number
    float m_fpsSampleAccum = 0.0f; // time accumulated toward the next graph sample

    // Standard cursors (nullptr = system default arrow)
    GLFWcursor* m_cursorEW    = nullptr;
    GLFWcursor* m_cursorNS    = nullptr;
    GLFWcursor* m_cursorNWSE  = nullptr;
    GLFWcursor* m_cursorNESW  = nullptr;
    GLFWcursor* m_cursorHand  = nullptr;     // pointer cursor over clickable UI
    GLFWcursor* m_appliedCursor = nullptr;   // last cursor handed to glfwSetCursor

    // Helpers
    void addQuad(float x, float y, float w, float h, const glm::vec4& color);
    // Solid (shape 0) triangle from three points — used for the play glyph.
    void addTriangle(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c,
                     const glm::vec4& color);
    // Emit one quad (centered at cx,cy, half-extents halfW/halfH including AA
    // padding) whose pixels are shaded by an SDF shape — see ui.frag for the
    // data0/data1 layout.
    void addShape(float cx, float cy, float halfW, float halfH,
                  const glm::vec4& color, const glm::vec4& data0, const glm::vec4& data1);
    HoverZone hitTestButtons(double mx, double my) const;
    uint8_t   hitTestEdges(double mx, double my) const;

    // Brand mark (left of the bar): a flat (matte) night disc with a 7-circle
    // Metatron cluster and an "N" traced through it (BL→TL→center→BR→TR).
    void addLogo(float cx, float cy, float r);

    // FPS overlay (top-right): a numeric readout + rolling history graph.
    // Text uses the shared PixelFont (see ui/PixelFont.h) at PixelFont::SCALE.
    void  buildFpsOverlay();
    void  addGlyph(float x, float y, char c, float s, const glm::vec4& color);
    float addText(float x, float y, const std::string& text, float s, const glm::vec4& color);
    float addNumber(float x, float y, int value, float s, const glm::vec4& color);
};

} // namespace Nyx
