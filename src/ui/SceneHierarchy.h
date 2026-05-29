#pragma once

// SceneHierarchy.h — Right-docked panel listing scene entities with multi-select
// (click / Ctrl-toggle / Shift-range / click-drag marquee), keyboard focus, and
// Delete/Copy/Cut/Paste commands the Engine acts on. Drives the Inspector via the
// "primary" (last-selected) entity. Renders through the shared UIPipeline+PixelFont.

#include "ui/UIVertex.h"
#include "renderer/Buffer.h"
#include "ecs/Entity.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <functional>

namespace Nyx {

class Registry;

class SceneHierarchy {
public:
    enum class Command { Delete, Copy, Cut, Paste, Duplicate };

    static constexpr float HEADER_H   = 20.0f;
    static constexpr float ROW_H      = 14.0f;
    static constexpr float MENU_ROW_H = 16.0f;

    void init(VmaAllocator allocator, GLFWwindow* window);
    void cleanup(VmaAllocator allocator);

    // Rebuild geometry. The panel occupies [x0, x0+width] x [top, top+height].
    void update(Registry& registry, float x0, float width, float top, float height, bool cursorActive);
    void draw(VkCommandBuffer cmd);

    bool handleMouseButton(int button, int action);   // press → record (resolved on release/drag)
    void handleRelease();                              // resolve click vs marquee
    bool handleRightPress();                           // open the context menu (true if over panel)
    bool handleScroll(double yoffset);
    bool handleKey(int key, int action, int mods);     // Del / Ctrl+C/X/V/A when focused
    bool menuOpen() const { return m_menuOpen; }

    // Primary (last-selected) entity → Inspector. Fires on any primary change.
    void setSelectCallback(std::function<void(Entity)> cb)  { m_onSelect  = std::move(cb); }
    void setCommandCallback(std::function<void(Command)> cb) { m_onCommand = std::move(cb); }

    // Fired when a row is double-clicked. Engine uses this to frame the
    // camera on the entity (same idea as Unity's "F" or Maya's "F" key).
    void setActivateCallback(std::function<void(Entity)> cb) { m_onActivate = std::move(cb); }
    // Click on the in-header ◀ collapse toggle → flip the right-dock state.
    void setCollapseCallback(std::function<void()> cb) { m_onToggleCollapse = std::move(cb); }

    // Engine-driven selection (spawn/paste select the new entities).
    void setSelection(const std::vector<Entity>& sel);
    const std::vector<Entity>& selection() const { return m_selected; }

    // Splice newly-created entities into the manual order right after the
    // lowest of their source entities. Used by Duplicate so a duped row lands
    // right below its source instead of at the bottom of the list.
    void insertAfterSources(const std::vector<Entity>& sources,
                            const std::vector<Entity>& fresh);

    void setFocused(bool f) { m_focused = f; }
    bool isFocused() const  { return m_focused; }

    void setVisible(bool v) { m_visible = v; }
    bool isVisible() const  { return m_visible; }

    // Engine sets this each frame so the right-dock resize edge gets a mint
    // highlight strip drawn on the panel's left side while hovered/dragged.
    void setLeftEdgeHighlight(bool h) { m_leftEdgeHighlight = h; }
    // When true, the panel renders as a thin vertical rail with a ▶ expand
    // chevron — same pattern the content browser uses when collapsed.
    void setCollapsedRail(bool c) { m_collapsedRail = c; }
    // True while the cursor is over the in-header collapse arrow (expanded
    // mode) or anywhere on the collapsed rail. Engine OR's it with the other
    // panels' wantsPointerCursor flags to swap to the hand cursor.
    bool wantsPointerCursor() const { return m_overButton; }

private:
    bool m_leftEdgeHighlight = false;
    bool m_collapsedRail     = false;
    bool m_overButton        = false;   // re-tested every update()
    enum class Kind { Mesh, Light, Environment, Other };
    struct Row { Entity entity; std::string label; Kind kind; };

    // 1px-quad text overflows small buffers; over-capacity geometry is dropped.
    static constexpr uint32_t VERT_CAP = 49152;
    static constexpr uint32_t IDX_CAP  = 73728;

    GLFWwindow*  m_window    = nullptr;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    bool         m_visible   = true;
    bool         m_focused   = false;

    std::vector<Row>    m_rows;
    std::vector<Entity> m_selected;                 // ordered; back() = primary
    Entity              m_anchor      = NULL_ENTITY; // Shift-range anchor
    Entity              m_lastPrimary = NULL_ENTITY; // for change detection
    float               m_scroll      = 0.0f;
    std::function<void(Entity)>  m_onSelect;
    std::function<void(Command)> m_onCommand;
    std::function<void(Entity)>  m_onActivate;
    std::function<void()>        m_onToggleCollapse;

    // Cached hit-rect of the in-header ◀ collapse button — recomputed each
    // update() so handleMouseButton can act on it.
    float m_collapseBtnX = 0.0f, m_collapseBtnY = 0.0f, m_collapseBtnW = 0.0f, m_collapseBtnH = 0.0f;

    // Double-click latch: same row pressed within DOUBLE_CLICK_S of the
    // previous press fires m_onActivate.
    int    m_lastClickRow  = -1;
    double m_lastClickTime = 0.0;
    static constexpr double DOUBLE_CLICK_S = 0.4;

    // Press / marquee state.
    bool   m_leftDown    = false;
    bool   m_marquee     = false;
    double m_pressX = 0.0, m_pressY = 0.0;
    double m_curX   = 0.0, m_curY   = 0.0;
    int    m_pressRow   = -1;
    bool   m_pressShift = false, m_pressCtrl = false;
    std::vector<Entity> m_preMarquee;               // base set for Ctrl+marquee union

    // Manual sibling order. Entities are kept in this vector in the order the
    // user wants them displayed; new entities get appended at the end. The
    // Environment singleton is not in here — buildRows always pins it to the
    // top regardless of order.
    std::vector<Entity> m_order;

    // Drag-to-reorder state. A press on a row that moves past the marquee
    // threshold starts a reorder drag instead of a rubber-band marquee.
    // m_reorderInsertIdx is the m_rows index the dragged row would land at.
    bool m_reorderDrag      = false;
    int  m_reorderInsertIdx = -1;

    // Right-click context menu.
    struct MenuItem { std::string label; Command cmd; };
    bool                  m_menuOpen = false;
    float                 m_menuX = 0.0f, m_menuY = 0.0f, m_menuW = 0.0f;
    std::vector<MenuItem> m_menuItems;
    void openMenu(double mx, double my);
    void closeMenu() { m_menuOpen = false; m_menuItems.clear(); }

    float m_x0 = 0.0f, m_w = 0.0f, m_top = 0.0f, m_h = 0.0f;

    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    bool     m_buffersInitialized = false;
    uint32_t m_indexCount = 0;
    std::vector<UIVertex> m_vertices;
    std::vector<uint32_t> m_indices;

    void buildRows(Registry& registry);   // also prunes stale selection
    bool isSelected(Entity e) const;
    int  rowOf(Entity e) const;            // row index of an entity, or -1
    int  rowAtY(double my) const;          // row under a y (panel space), or -1
    void selectSingle(Entity e);
    void toggle(Entity e);
    void selectRange(int rowA, int rowB);  // inclusive; clicked end becomes primary
    void notifyPrimary();                  // fire onSelect when the primary changes

    void  addQuad(float x, float y, float w, float h, const glm::vec4& color);
    void  addBall(float cx, float cy, float radius, const glm::vec4& color);
    void  addGlyph(float x, float y, char c, float s, const glm::vec4& color);
    void  addArrow(float cx, float cy, bool right, const glm::vec4& color);
    float addText(float x, float y, const std::string& text, float s, const glm::vec4& color, float maxX);
};

} // namespace Nyx
