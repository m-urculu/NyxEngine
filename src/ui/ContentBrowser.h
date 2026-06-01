#pragma once

// ContentBrowser.h — Left-docked content browser. Shows the active project's
// files (assets / scenes / procgen) — never the engine source. Folders are
// collapsible: left-click a folder row to toggle its contents. Renders through
// the same UIPipeline as the title bar, using the shared PixelFont.

#include "ui/UIVertex.h"
#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <functional>
#include <set>

namespace Nyx {

class ContentBrowser {
public:
    static constexpr float DEFAULT_WIDTH   = 220.0f; // initial expanded panel width (px)
    static constexpr float MIN_WIDTH       = 150.0f;
    static constexpr float MAX_WIDTH       = 480.0f;
    static constexpr float COLLAPSED_WIDTH = 18.0f;  // thin rail width when collapsed
    static constexpr float TOP_OFFSET      = 0.0f;   // OS draws the title bar; content browser sits at y=0
    static constexpr float HEADER_H        = 22.0f;  // header band height
    static constexpr float ROW_H           = 12.0f;  // tree row height
    static constexpr float LIST_TOP        = TOP_OFFSET + HEADER_H; // content top (header covers overflow)
    static constexpr float RESIZE_GRAB     = 5.0f;   // px grab zone on the right edge

    void init(VmaAllocator allocator, GLFWwindow* window, const std::string& projectPath);
    void cleanup(VmaAllocator allocator);

    // Rebuild geometry; cursorActive enables hover highlight.
    void update(float windowWidth, float windowHeight, bool cursorActive);
    void draw(VkCommandBuffer cmd);

    // Left-press records a row for click/drag; the click action (toggle/open) and
    // any drag-drop move are resolved on release. Returns true if consumed.
    bool handleMouseButton(int button, int action);
    void handleRelease();                 // finish resize / drag-drop / row click
    bool handleScroll(double yoffset);    // returns true if the wheel was over the panel
    bool overResizeEdge() const;          // cursor is on the right edge (or actively resizing)

    // Right-click opens the context menu. Returns true if the cursor was over the
    // panel (so the caller can suppress camera look). Char/key feed inline rename.
    bool handleRightPress();
    void handleChar(unsigned int cp);
    bool handleKey(int key, int action, int mods);       // true if consumed
    bool capturesKeyboard() const { return m_renaming; }  // suppresses camera WASD while naming
    bool menuOpen() const { return m_menuOpen; }
    // Dismiss this panel's context menu (the app keeps only one menu open at a time).
    void closeContextMenu() { closeMenu(); }

    // Keyboard focus for the file tree: when focused, Ctrl+C/X/V/Z act on the
    // selected row. The Engine clears it when another panel/the viewport is clicked.
    void setFocused(bool f) { m_focused = f; }
    bool isFocused() const { return m_focused; }

    // Invoked with a path that was removed (deleted) so the editor can close its tab.
    void setPathRemovedCallback(std::function<void(const std::string&)> cb) { m_onPathRemoved = std::move(cb); }

    // Invoked for File-menu actions the engine owns: "save" / "saveas" / "exit" / "openproject".
    void setFileMenuCallback(std::function<void(const std::string&)> cb) { m_onFileMenu = std::move(cb); }

    // Invoked when the user asks to switch projects (New Project, Switch
    // Project) — the engine handles the save-current/clear/load-new flow.
    void setSwitchProjectCallback(std::function<void(const std::string&)> cb) { m_onSwitchProject = std::move(cb); }

    // Switch the browsed project root (e.g. after the engine's Open-folder dialog).
    void setProject(const std::string& path);

    void setVisible(bool v) { m_visible = v; }
    bool isVisible() const { return m_visible; }

    // True when the cursor is over a clickable element (FILE menu, new file /
    // folder buttons, expand toggle, or a tree row). Engine flips to the
    // pointer cursor when this is set.
    bool wantsPointerCursor() const { return m_overButton; }

    // Current docked width (full when expanded, the rail when collapsed) — used
    // to tile the bottom console to the right of the sidebar.
    float currentWidth() const { return m_expanded ? m_width : COLLAPSED_WIDTH; }

    // Persisted layout state — Engine writes these to editor.prefs on shutdown
    // and reads them back at startup so the collapse + width survive a restart.
    bool  isExpanded() const { return m_expanded; }
    float panelWidth() const { return m_width; }
    void  setExpanded(bool v) { m_expanded = v; }
    void  setPanelWidth(float w) { m_width = w; }

    // Folder expand-state persistence. Engine writes the set of currently-
    // open folder paths into editor.prefs at shutdown and re-applies them at
    // startup so the tree looks the same after a relaunch.
    std::set<std::string> expandedFolders() const;
    void setExpandedFolders(const std::set<std::string>& exp);

    // Invoked with the file path when a file row is clicked.
    void setFileOpenCallback(std::function<void(const std::string&)> cb) { m_onFileOpen = std::move(cb); }

    // Invoked when a file row is dragged and released OUTSIDE the panel (e.g. onto
    // the Inspector's material slot): (draggedPath, cursorX, cursorY).
    void setExternalDropCallback(std::function<void(const std::string&, double, double)> cb) {
        m_onExternalDrop = std::move(cb);
    }

    // Drag state + selection, queried by the Inspector (drop-hover highlight) and
    // the Engine (click-to-assign uses the current selection).
    bool               isDraggingFile() const { return m_dragActive && !m_pressPath.empty(); }
    const std::string& draggedPath()    const { return m_pressPath; }
    const std::string& selectedPath()   const { return m_selectedPath; }

private:
    struct Node {
        std::string name;
        std::string path;
        bool isDir    = false;
        bool expanded = true;            // folders default to expanded
        std::vector<Node> children;
    };
    struct Row { Node* node; int depth; };

    GLFWwindow*  m_window    = nullptr;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    bool         m_visible   = true;

    std::string m_projectPath;
    std::string m_projectName;

    Node             m_root;       // built once at init; never reallocated after
    std::vector<Row> m_rows;       // currently-visible rows, rebuilt each frame
    bool             m_expanded = true;            // file viewer expanded (collapse toggle)
    float            m_width    = DEFAULT_WIDTH;   // current panel width (drag-resizable)
    float            m_scroll   = 0.0f;            // vertical scroll offset (px)
    bool             m_resizing = false;           // dragging the right edge
    std::function<void(const std::string&)> m_onFileOpen;

    // Geometry cache — the tree's 1px-quad text is expensive to rebuild + re-upload
    // every frame. Skip it unless something that affects the rendered tree changed
    // (hover/interaction over the panel, layout/focus change, or a tree mutation).
    bool  m_haveGeom    = false;
    bool  m_geomDirty   = true;   // set by tree mutations that happen off-panel (scan)
    float m_cacheW = -1.0f, m_cacheH = -1.0f, m_cacheWidth = -1.0f;
    bool  m_cacheExpanded = true, m_cacheFocused = false;
    bool  m_cacheCursorOver = false;   // catches the "cursor left the panel" transition so the
                                       // hover/resize highlight gets redrawn-clean exactly once
    bool  m_overButton  = false;   // cursor over a clickable header button or tree row

    // ── Context menu ─────────────────────────────────────────────────────────
    static constexpr float MENU_ROW_H = 16.0f;
    enum Action { A_OPEN, A_NEWFOLDER, A_NEWFILE, A_NEWMATERIAL, A_CUT, A_COPY, A_PASTE, A_RENAME, A_DELETE, A_REFRESH,
                  A_NEWPROJECT, A_OPENPROJECT, A_SWITCHPROJECT, A_SAVE, A_SAVEAS, A_EXPORT, A_EXIT };
    struct MenuItem { std::string label; int action; std::string arg; };
    static constexpr float FILE_BTN_W   = 36.0f;   // "FILE" header button width (6 px pad each side)
    static constexpr float TOOL_BTN_W   = 18.0f;   // new-file / new-folder header buttons
    static constexpr float TOOL_GAP     = 6.0f;    // gap between the toolbar icons and the project name
    void  createInSelected(bool folder);           // new file/folder in the selected dir, then rename
    float projLabelWidth() const;                  // px width of the uppercased project name
    float toolButtonX(bool folder, float pw) const;  // left x of a toolbar icon (right side, left of name)
    bool                  m_menuOpen   = false;
    float                 m_menuX = 0.0f, m_menuY = 0.0f, m_menuW = 0.0f;
    std::string           m_menuTarget;            // path right-clicked (root = project path)
    bool                  m_menuTargetDir = false;
    std::vector<MenuItem> m_menuItems;

    // ── Selection / focus ─────────────────────────────────────────────────────
    std::string m_selectedPath;        // last-clicked row; target for keyboard ops
    bool        m_focused = false;      // file tree has keyboard focus (Ctrl shortcuts)
    std::function<void(const std::string&)> m_onPathRemoved;  // -> editor closes the tab
    std::function<void(const std::string&)> m_onFileMenu;     // -> engine (save/saveas/exit)
    std::function<void(const std::string&)> m_onSwitchProject;// -> engine (full project switch flow)
    std::function<void(const std::string&, double, double)> m_onExternalDrop; // -> drop outside panel

    // ── Cut / copy clipboard ─────────────────────────────────────────────────
    std::string m_clipPath;
    bool        m_clipCut = false;

    // ── Undo stack for file operations (move/rename/create/delete) ─────────────
    enum class UndoKind { MoveBack, RemoveCreated, RestoreDeleted };
    struct UndoOp { UndoKind kind; std::string a; std::string b; };
    std::vector<UndoOp> m_undo;
    void doUndo();

    // ── Inline rename ────────────────────────────────────────────────────────
    bool        m_renaming = false;
    std::string m_renamePath;          // node being renamed
    std::string m_renameText;          // editable buffer (real case)
    bool        m_renameSelected = false;  // whole name selected → first keystroke replaces it

    // ── Drag & drop ──────────────────────────────────────────────────────────
    bool        m_leftDown   = false;  // a row press is pending (click or drag)
    bool        m_dragActive = false;  // moved past the threshold → dragging
    int         m_pressRow   = -1;
    std::string m_pressPath;
    double      m_pressX = 0.0, m_pressY = 0.0;
    std::string m_dropTarget;          // valid destination dir while dragging
    int         m_dropRow = -1;        // folder row highlighted as the drop target

    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    bool     m_buffersInitialized = false;
    uint32_t m_indexCount = 0;

    std::vector<UIVertex> m_vertices;
    std::vector<uint32_t> m_indices;

    float m_windowWidth  = 0.0f;
    float m_windowHeight = 0.0f;

    void scan();
    void scanDir(Node& parent);
    void flatten();
    void flattenNode(Node& n, int depth);

    // Tree mutation — all filesystem ops rescan and restore expanded state by path.
    void   refresh();
    void   collectExpanded(const Node& n, std::set<std::string>& out) const;
    void   applyExpanded(Node& n, const std::set<std::string>& exp);
    Node*  findByPath(Node& n, const std::string& path);
    static std::string parentOf(const std::string& path);
    static bool        isSubPath(const std::string& parent, const std::string& child);
    std::string uniqueChildPath(const std::string& dir, const std::string& name) const;
    void   moveInto(const std::string& src, const std::string& destDir, bool copy);

    // Context menu (right-click), File menu (header button), inline rename.
    void openMenu(const std::string& target, bool isDir, float x, float y);
    void openFileMenu(float x, float y);                  // project File menu
    void closeMenu() { m_menuOpen = false; m_menuItems.clear(); }
    void doAction(int action, const std::string& arg = "");
    void beginRename(const std::string& path);
    void commitRename();
    void cancelRename();

    // File-type icons: 3D assets keep the shaded ball (engine signature shape);
    // other types get a flat 8x8 pixel pictograph in a category color.
    enum class FileCat { Object, Material, Image, Script, Audio, Doc, Data, Other };
    static FileCat fileCategory(const std::string& ext);
    void  addFileIcon(float iconX, float rowY, FileCat cat, const glm::vec4& color);
    void  addIconBitmap(float ix, float iy, const uint8_t rows[8], const glm::vec4& color);

    // Geometry / text helpers (solid quads = shape 0; ball = shape 4).
    void  addQuad(float x, float y, float w, float h, const glm::vec4& color);
    void  addBall(float cx, float cy, float radius, const glm::vec4& color);
    void  addChevron(float x, float rowY, bool expanded, const glm::vec4& color);
    void  addFolderIcon(float iconX, float iconY, bool open, const glm::vec4& color);
    void  addArrow(float cx, float cy, bool right, const glm::vec4& color);  // ▶ / ◀
    void  addPlus(float cx, float cy, float r, const glm::vec4& color);      // small + badge

    // Collapse/expand toggle hit region (right edge of the expanded header band).
    static constexpr float TOGGLE_W = 22.0f;
    void  addGlyph(float x, float y, char c, float s, const glm::vec4& color);
    float addText(float x, float y, const std::string& text, float s, const glm::vec4& color, float maxX);
};

} // namespace Nyx
