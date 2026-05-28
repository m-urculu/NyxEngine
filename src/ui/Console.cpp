#include "ui/Console.h"
#include "ui/PixelFont.h"
#include "core/LogStore.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <cstring>

namespace Nyx {

void Console::init(VmaAllocator allocator, GLFWwindow* window) {
    m_allocator = allocator;
    m_window    = window;
    m_vertexBuffer.init(allocator, sizeof(UIVertex) * VERT_CAP,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_indexBuffer.init(allocator, sizeof(uint32_t) * VERT_CAP * 3 / 2,
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_buffersInitialized = true;

    // Built-in commands.
    registerCommand("help", "list available commands", [this](const std::vector<std::string>&) {
        print("commands:", {0.55f, 0.60f, 0.66f, 1.0f});
        for (const auto& c : m_commands) print("  " + c.name + "   " + c.help);
    });
    registerCommand("clear", "clear the console", [this](const std::vector<std::string>&) {
        m_cmdLines.clear(); m_cmdScroll = 0; m_cmdRev++;
    });
    print("Nyx dev console - type 'help'", {0.50f, 0.54f, 0.60f, 1.0f});
}

void Console::registerCommand(const std::string& name, const std::string& help, CmdFn fn) {
    m_commands.push_back({name, help, std::move(fn)});
}

void Console::print(const std::string& line, const glm::vec4& color) {
    m_cmdLines.push_back({line, color});
    if (m_cmdLines.size() > 500) m_cmdLines.erase(m_cmdLines.begin());
    m_cmdScroll = 0;        // jump to the newest output
    m_cmdRev++;
}

void Console::executeCommand(const std::string& raw) {
    size_t a = raw.find_first_not_of(" \t");
    if (a == std::string::npos) return;
    size_t b = raw.find_last_not_of(" \t");
    std::string line = raw.substr(a, b - a + 1);
    print("> " + line, {0.42f, 1.00f, 0.66f, 1.0f});          // echo (mint)
    m_history.push_back(line); m_histPos = -1;

    std::vector<std::string> tok; std::istringstream ss(line); std::string t;
    while (ss >> t) tok.push_back(t);
    if (tok.empty()) return;
    std::vector<std::string> args(tok.begin() + 1, tok.end());
    for (const auto& c : m_commands)
        if (c.name == tok[0]) { c.fn(args); return; }
    print("unknown command: " + tok[0] + "  (try 'help')", {0.96f, 0.42f, 0.40f, 1.0f});
}

void Console::handleChar(unsigned int cp) {
    if (!capturesKeyboard()) return;
    if (cp < 32 || cp > 126) return;
    m_input.push_back(static_cast<char>(cp)); m_cmdRev++;
}

void Console::handleKey(int key, int action, int /*mods*/) {
    if (!capturesKeyboard() || action == GLFW_RELEASE) return;
    switch (key) {
        case GLFW_KEY_BACKSPACE: if (!m_input.empty()) m_input.pop_back(); m_cmdRev++; break;
        case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER:
            if (!m_input.empty()) { executeCommand(m_input); m_input.clear(); } m_cmdRev++; break;
        case GLFW_KEY_ESCAPE: m_input.clear(); m_focused = false; m_cmdRev++; break;
        case GLFW_KEY_UP:
            if (!m_history.empty()) {
                if (m_histPos == -1) m_histPos = (int)m_history.size() - 1; else if (m_histPos > 0) m_histPos--;
                m_input = m_history[m_histPos]; m_cmdRev++;
            }
            break;
        case GLFW_KEY_DOWN:
            if (m_histPos != -1) {
                m_histPos++;
                if (m_histPos >= (int)m_history.size()) { m_histPos = -1; m_input.clear(); }
                else m_input = m_history[m_histPos];
                m_cmdRev++;
            }
            break;
        default: break;
    }
}

void Console::cleanup(VmaAllocator allocator) {
    if (m_buffersInitialized) {
        m_vertexBuffer.cleanup(allocator);
        m_indexBuffer.cleanup(allocator);
        m_buffersInitialized = false;
    }
}

// ─── Input ──────────────────────────────────────────────────────────────────

bool Console::handleMouseButton(int button, int action) {
    if (!m_visible) return false;
    if (button != GLFW_MOUSE_BUTTON_LEFT) return false;
    if (action == GLFW_RELEASE) { m_resizing = false; return false; }
    if (action != GLFW_PRESS) return false;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    float curH = m_expanded ? m_height : HEADER_H;
    float top  = m_winH - curH;

    // Resize: grab the top edge (expanded only).
    if (m_expanded && mx >= m_left && mx < m_winW && std::fabs(my - top) <= RESIZE_GRAB) {
        m_resizing = true;
        return true;
    }
    if (mx < m_left || mx >= m_winW || my < top) return false;   // outside the console

    if (my < top + HEADER_H) {                                   // header
        if (mx >= m_winW - TOGGLE_W) { m_expanded = !m_expanded; return true; }  // collapse toggle
        float tx = m_left + 6.0f;                                // Output / Console tabs
        const char* names[2] = { "OUTPUT", "CONSOLE" };
        for (int i = 0; i < 2; ++i) {
            float tw = static_cast<float>(std::strlen(names[i])) * PixelFont::ADVANCE + 12.0f;
            if (mx >= tx && mx < tx + tw) { m_tab = i; m_focused = (i == 1); m_cmdRev++; break; }
            tx += tw;
        }
        return true;
    }
    if (!m_expanded) { m_expanded = true; return true; }         // click the collapsed strip → expand
    m_focused = (m_tab == 1);                                    // body click focuses the command input
    m_cmdRev++;
    return true;   // consume clicks inside the console
}

void Console::handleRelease() { m_resizing = false; }

bool Console::handleScroll(double yoffset) {
    if (!m_visible || !m_expanded) return false;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    float top = m_winH - m_height;
    if (mx < m_left || mx >= m_winW || my < top) return false;   // not over the console
    if (m_tab == 1) {                                            // Console tab scrollback
        m_cmdScroll += static_cast<int>(std::lround(yoffset));
        if (m_cmdScroll < 0) m_cmdScroll = 0;                    // upper clamp done in update()
        m_cmdRev++;
        return true;
    }
    m_scrollLines += static_cast<int>(std::lround(yoffset));      // wheel up → older
    if (m_scrollLines < 0)            m_scrollLines = 0;
    if (m_scrollLines > m_maxScroll)  m_scrollLines = m_maxScroll;
    return true;
}

// ─── Update ─────────────────────────────────────────────────────────────────

void Console::update(float windowWidth, float windowHeight, float leftOffset) {
    m_winW = windowWidth; m_winH = windowHeight; m_left = leftOffset;
    if (!m_visible) { m_indexCount = 0; m_haveGeom = false; return; }

    // Keep the height valid for the current window. This makes the panel adapt to
    // window resizing: it can grow on a larger window and is clamped back down when
    // the window shrinks (otherwise the console would overflow off the top).
    const float maxH = maxHeightFor(windowHeight);

    // Apply an in-progress resize drag (drag the top edge up to grow).
    if (m_resizing) {
        bool down = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (!down) {
            m_resizing = false;
        } else {
            double mx = 0.0, my = 0.0;
            glfwGetCursorPos(m_window, &mx, &my);
            m_height = std::clamp(windowHeight - static_cast<float>(my), MIN_HEIGHT, maxH);
        }
    }
    m_height = std::clamp(m_height, MIN_HEIGHT, maxH);

    const float s     = PixelFont::SCALE;
    const float lineH = PixelFont::CELL_H * s + 3.0f;
    const float curH  = m_expanded ? m_height : HEADER_H;
    const float py    = windowHeight - curH;

    // Resize-edge hover (top edge) — drives the highlight and the resize cursor.
    double cmx = -1.0, cmy = -1.0;
    glfwGetCursorPos(m_window, &cmx, &cmy);
    bool edgeHover = m_resizing || (m_expanded && cmx >= m_left && cmx < m_winW
                                    && std::fabs(cmy - py) <= RESIZE_GRAB);

    // Button hover (Output/Console tabs + collapse toggle) — computed even when
    // the cached geometry is reused below so the pointer cursor still flicks on
    // and off as the mouse moves over the header.
    m_overButton = false;
    if (m_expanded && cmx >= m_left && cmx < m_winW && cmy >= py && cmy < py + HEADER_H) {
        const char* names[2] = { "OUTPUT", "CONSOLE" };
        float tx = m_left + 6.0f;
        for (int i = 0; i < 2; ++i) {
            float tw = static_cast<float>(std::strlen(names[i])) * PixelFont::ADVANCE + 12.0f;
            if (cmx >= tx && cmx < tx + tw) { m_overButton = true; break; }
            tx += tw;
        }
        if (!m_overButton && cmx >= m_winW - TOGGLE_W && cmx < m_winW) m_overButton = true;
    }

    // Visible line slots + scroll clamp (needs the current line count).
    const float contentTop = py + HEADER_H + 2.0f;
    const float contentBot = windowHeight - 3.0f;
    int visibleLines = m_expanded ? std::max(0, static_cast<int>(std::floor((contentBot - contentTop) / lineH))) : 0;
    int total = static_cast<int>(LogStore::count());
    m_maxScroll = std::max(0, total - visibleLines);
    if (m_scrollLines > m_maxScroll) m_scrollLines = m_maxScroll;
    if (m_scrollLines < 0)           m_scrollLines = 0;

    // Skip the rebuild when nothing relevant changed.
    uint64_t v = LogStore::version();
    if (m_haveGeom && v == m_lastVersion && windowWidth == m_lastW && windowHeight == m_lastH &&
        leftOffset == m_lastLeft && m_expanded == m_lastExpanded && m_height == m_lastHeight &&
        m_scrollLines == m_lastScroll && edgeHover == m_lastEdgeHover &&
        m_tab == m_lastTab && m_cmdRev == m_lastCmdRev && m_focused == m_lastFocused) {
        return;
    }
    m_lastVersion = v; m_lastW = windowWidth; m_lastH = windowHeight; m_lastLeft = leftOffset;
    m_lastExpanded = m_expanded; m_lastHeight = m_height; m_lastScroll = m_scrollLines;
    m_lastEdgeHover = edgeHover;
    m_lastTab = m_tab; m_lastCmdRev = m_cmdRev; m_lastFocused = m_focused;
    m_haveGeom = true;

    m_vertices.clear();
    m_indices.clear();

    const glm::vec4 panelBg  = {0.055f, 0.060f, 0.075f, 0.97f};
    const glm::vec4 headerBg = {0.10f,  0.105f, 0.130f, 1.0f};
    const glm::vec4 border   = {0.22f,  0.24f,  0.30f,  1.0f};
    const glm::vec4 labelCol = {0.50f,  0.54f,  0.60f,  1.0f};
    const glm::vec4 toggleCol= {0.78f,  0.80f,  0.86f,  1.0f};
    const glm::vec4 cInfo    = {0.78f,  0.80f,  0.86f,  1.0f};
    const glm::vec4 cWarn    = {0.96f,  0.80f,  0.32f,  1.0f};
    const glm::vec4 cErr     = {0.96f,  0.42f,  0.40f,  1.0f};
    const glm::vec4 cDim     = {0.48f,  0.50f,  0.56f,  1.0f};
    const glm::vec4 resizeCol= {0.42f,  1.00f,  0.66f,  1.0f};   // resize-edge highlight (mint)

    const float px = leftOffset;
    const float pw = windowWidth - leftOffset;
    if (pw < 1.0f) { upload(); return; }

    // Background + top border.
    addQuad(px, py, pw, curH, panelBg);
    addQuad(px, py, pw, 1.0f, border);

    // Header chrome + Output/Console tabs + collapse toggle. Emitted BEFORE the
    // text body so that if a very tall console exhausts the vertex budget, the
    // dropped geometry is old log lines (off-screen-ish) rather than the chrome.
    addQuad(px, py, pw, HEADER_H, headerBg);
    float hy = py + std::floor((HEADER_H - PixelFont::CELL_H) * 0.5f);
    {
        const char* names[2] = { "OUTPUT", "CONSOLE" };
        float tx = px + 6.0f;
        for (int i = 0; i < 2; ++i) {
            float tw = static_cast<float>(std::strlen(names[i])) * PixelFont::ADVANCE + 12.0f;
            bool act = (i == m_tab);
            if (act) addQuad(tx, py + HEADER_H - 2.0f, tw, 2.0f, resizeCol);   // active underline
            addText(tx + 6.0f, hy, names[i], s, act ? toggleCol : labelCol, tx + tw);
            tx += tw;
        }
    }
    addArrowV(px + pw - TOGGLE_W * 0.5f, py + HEADER_H * 0.5f, m_expanded, toggleCol);
    if (edgeHover) addQuad(px, py, pw, 2.0f, resizeCol);   // resize-edge highlight

    const float textX = px + 8.0f;
    const float maxX  = px + pw - 6.0f;

    if (m_expanded && m_tab == 0 && visibleLines > 0) {
        // OUTPUT tab — log lines, newest at the bottom slot.
        std::vector<LogLine> all;
        LogStore::snapshot(all, 500);
        int n = static_cast<int>(all.size());
        int topIdx = (n - 1 - m_scrollLines) - (visibleLines - 1);
        for (int r = 0; r < visibleLines; ++r) {
            int li = topIdx + r;
            if (li < 0 || li >= n) continue;
            const LogLine& ln = all[li];
            glm::vec4 col;
            switch (ln.level) {
                case 0: case 1: col = cDim;  break;
                case 3:         col = cWarn; break;
                case 4: case 5: col = cErr;  break;
                default:        col = cInfo; break;
            }
            addText(textX, contentTop + r * lineH, ln.text, s, col, maxX);
        }
    } else if (m_expanded) {
        // CONSOLE tab — command output above, an input line pinned to the bottom.
        const glm::vec4 promptCol = {0.42f, 1.00f, 0.66f, 1.0f};
        const glm::vec4 inputCol  = {0.92f, 0.94f, 0.98f, 1.0f};
        float inputY  = contentBot - lineH;
        int   outRows = std::max(0, static_cast<int>(std::floor((inputY - 2.0f - contentTop) / lineH)));
        int   n       = static_cast<int>(m_cmdLines.size());
        int   cmdMax  = std::max(0, n - outRows);
        if (m_cmdScroll > cmdMax) m_cmdScroll = cmdMax;
        int topIdx = (n - 1 - m_cmdScroll) - (outRows - 1);
        for (int r = 0; r < outRows; ++r) {
            int li = topIdx + r;
            if (li < 0 || li >= n) continue;
            addText(textX, contentTop + r * lineH, m_cmdLines[li].text, s, m_cmdLines[li].color, maxX);
        }
        // Input line + caret.
        addQuad(px, inputY - 2.0f, pw, 1.0f, border);
        float cx = addText(textX, inputY, "> " + m_input, s, inputCol, maxX);
        if (m_focused) addQuad(textX + cx, inputY, 1.0f, PixelFont::CELL_H * s, promptCol);
    }

    upload();
}

bool Console::overResizeEdge() const {
    if (!m_visible || !m_expanded) return false;
    if (m_resizing) return true;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    float top = m_winH - m_height;
    return mx >= m_left && mx < m_winW && std::fabs(my - top) <= RESIZE_GRAB;
}

void Console::upload() {
    if (!m_vertices.empty()) {
        m_vertexBuffer.uploadData(m_allocator, m_vertices.data(), m_vertices.size() * sizeof(UIVertex));
        m_indexBuffer.uploadData(m_allocator, m_indices.data(), m_indices.size() * sizeof(uint32_t));
        m_indexCount = static_cast<uint32_t>(m_indices.size());
    } else {
        m_indexCount = 0;
    }
}

void Console::draw(VkCommandBuffer cmd) {
    if (!m_visible || m_indexCount == 0) return;
    VkBuffer buffers[] = { m_vertexBuffer.getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

// ─── Geometry / text helpers ──────────────────────────────────────────────────

void Console::addQuad(float x, float y, float w, float h, const glm::vec4& color) {
    if (m_vertices.size() + 4 > VERT_CAP) return;
    const glm::vec2 z2{0.0f, 0.0f};
    const glm::vec4 z4{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t base = static_cast<uint32_t>(m_vertices.size());
    m_vertices.push_back({{x,     y},     color, z2, z4, z4});
    m_vertices.push_back({{x + w, y},     color, z2, z4, z4});
    m_vertices.push_back({{x + w, y + h}, color, z2, z4, z4});
    m_vertices.push_back({{x,     y + h}, color, z2, z4, z4});
    m_indices.push_back(base);     m_indices.push_back(base + 1); m_indices.push_back(base + 2);
    m_indices.push_back(base + 2); m_indices.push_back(base + 3); m_indices.push_back(base);
}

void Console::addGlyph(float x, float y, char c, float s, const glm::vec4& color) {
    const uint8_t* rows = PixelFont::glyphRows(c);
    if (!rows) return;
    for (int r = 0; r < PixelFont::CELL_H; ++r) {
        uint8_t bits = rows[r];
        for (int col = 0; col < PixelFont::CELL_W; ++col)
            if (bits & (1 << (PixelFont::CELL_W - 1 - col)))
                addQuad(x + col * s, y + r * s, s, s, color);
    }
}

float Console::addText(float x, float y, const std::string& text, float s, const glm::vec4& color, float maxX) {
    float cx = x;
    for (char c : text) {
        if (cx + PixelFont::CELL_W * s > maxX) break;
        addGlyph(cx, y, c, s, color);
        cx += PixelFont::ADVANCE * s;
    }
    return cx - x;
}

// Small triangle (5 wide, 3 tall): ▾ when down (expanded → collapse), ▴ when up (collapsed → expand).
void Console::addArrowV(float cx, float cy, bool down, const glm::vec4& color) {
    float top = cy - 1.0f;
    for (int r = 0; r < 3; ++r) {
        int w = down ? (5 - 2 * r) : (1 + 2 * r);
        float left = cx - static_cast<float>((w - 1) / 2);
        addQuad(left, top + r, static_cast<float>(w), 1.0f, color);
    }
}

} // namespace Nyx
