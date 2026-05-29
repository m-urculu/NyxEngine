#pragma once

// Console.h — Bottom-docked log console. Renders recent log lines (color-coded
// by level) captured by LogStore, through the shared UIPipeline + PixelFont.
// Collapsible (header toggle), drag-resizable (top edge), and scrollable.

#include "ui/UIVertex.h"
#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include <algorithm>

namespace Nyx {

class Console {
public:
    static constexpr float DEFAULT_HEIGHT = 150.0f;
    static constexpr float MIN_HEIGHT     = 56.0f;
    static constexpr float TOP_RESERVE    = 32.0f;   // px kept above the console = title-bar height (TitleBar::BAR_HEIGHT)
    static constexpr float HEADER_H       = 20.0f;
    static constexpr float TOGGLE_W       = 22.0f;
    static constexpr float RESIZE_GRAB    = 5.0f;

    void init(VmaAllocator allocator, GLFWwindow* window);
    void cleanup(VmaAllocator allocator);

    // leftOffset = x where the panel starts (right edge of the left dock).
    void update(float windowWidth, float windowHeight, float leftOffset);
    void draw(VkCommandBuffer cmd);

    bool handleMouseButton(int button, int action);
    void handleRelease();
    bool handleScroll(double yoffset);
    bool overResizeEdge() const;          // cursor is on the top edge (or actively resizing)

    // True when the cursor is over a clickable header element (Output/Console
    // tabs, collapse toggle). Engine flips to the pointer cursor when this is set.
    bool wantsPointerCursor() const { return m_overButton; }

    void setVisible(bool v) { m_visible = v; }
    bool isVisible() const { return m_visible; }
    float currentHeight() const { return m_expanded ? m_height : HEADER_H; }

    // Persisted layout state — written to editor.prefs on shutdown.
    bool  isExpanded()  const { return m_expanded; }
    float panelHeight() const { return m_height; }
    void  setExpanded(bool v)     { m_expanded = v; }
    void  setPanelHeight(float h) { m_height   = h; }

    // ── Dev command console (the "Console" tab) ──────────────────────────────
    using CmdFn = std::function<void(const std::vector<std::string>& args)>;
    void registerCommand(const std::string& name, const std::string& help, CmdFn fn);
    void print(const std::string& line, const glm::vec4& color = {0.80f, 0.83f, 0.88f, 1.0f});

    // Keyboard — routed by Input when the Console tab has focus.
    void handleChar(unsigned int cp);
    void handleKey(int key, int action, int mods);
    bool capturesKeyboard() const { return m_focused && m_expanded && m_tab == 1; }
    void setFocused(bool f) { m_focused = f; }
    bool isFocused() const { return m_focused; }

    // Max console height for a given window: scales with the window so the panel
    // grows on larger windows (the old fixed 480px cap didn't adapt to resizing).
    static float maxHeightFor(float windowHeight) {
        return std::max(MIN_HEIGHT, windowHeight - TOP_RESERVE);
    }

private:
    // The pixel font emits one quad per lit glyph pixel, so a tall+wide console
    // is vertex-heavy. Sized to render a console filling a maximized 1080p window
    // (~80 lines of typical-length log text) without hitting the cap.
    static constexpr uint32_t VERT_CAP = 262144;

    VmaAllocator m_allocator = VK_NULL_HANDLE;
    GLFWwindow*  m_window    = nullptr;
    bool         m_visible   = true;

    bool  m_expanded    = true;
    float m_height      = DEFAULT_HEIGHT;
    int   m_scrollLines = 0;   // log scrollback (Output tab)
    int   m_maxScroll   = 0;
    bool  m_resizing    = false;

    // Tabs + command console state.
    struct Command { std::string name, help; CmdFn fn; };
    struct CmdLine { std::string text; glm::vec4 color; };
    int                  m_tab = 0;            // 0 = Output (log), 1 = Console (commands)
    std::vector<Command> m_commands;
    std::vector<CmdLine> m_cmdLines;           // command echo + results
    std::string          m_input;              // current command being typed
    std::vector<std::string> m_history;        // executed commands
    int                  m_histPos   = -1;     // -1 = editing a fresh line
    int                  m_cmdScroll = 0;      // console-tab scrollback
    bool                 m_focused   = false;
    uint64_t             m_cmdRev    = 0;       // bumps on any console-tab change → triggers rebuild
    void executeCommand(const std::string& line);

    // Current rect (set each update, used by the input handlers).
    float m_left = 0.0f, m_winW = 0.0f, m_winH = 0.0f;

    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    bool     m_buffersInitialized = false;
    uint32_t m_indexCount = 0;

    std::vector<UIVertex> m_vertices;
    std::vector<uint32_t> m_indices;

    // Rebuild only when something relevant changed.
    bool     m_haveGeom    = false;
    uint64_t m_lastVersion = 0;
    float    m_lastW = 0, m_lastH = 0, m_lastLeft = -1, m_lastHeight = 0;
    bool     m_lastExpanded = true;
    int      m_lastScroll = -1;
    bool     m_lastEdgeHover = false;
    int      m_lastTab = -1;
    uint64_t m_lastCmdRev = ~0ull;
    bool     m_lastFocused = false;
    bool     m_overButton  = false;   // cursor over a clickable header element this frame

    void  addQuad(float x, float y, float w, float h, const glm::vec4& color);
    void  addGlyph(float x, float y, char c, float s, const glm::vec4& color);
    float addText(float x, float y, const std::string& text, float s, const glm::vec4& color, float maxX);
    void  addArrowV(float cx, float cy, bool down, const glm::vec4& color);  // ▾ / ▴
    void  upload();
};

} // namespace Nyx
