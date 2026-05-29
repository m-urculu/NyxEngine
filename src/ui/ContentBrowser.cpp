#include "ui/ContentBrowser.h"
#include "ui/PixelFont.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace Nyx {

// ════════════════════════════════════════════════════════════════════════════
// LIFECYCLE
// ════════════════════════════════════════════════════════════════════════════

void ContentBrowser::init(VmaAllocator allocator, GLFWwindow* window, const std::string& projectPath) {
    m_allocator   = allocator;
    m_window      = window;
    m_projectPath = projectPath;
    m_projectName = fs::path(projectPath).filename().string();
    if (m_projectName.empty()) m_projectName = projectPath;

    // Sized for a deep, fully-expanded asset tree — the 1px-quad font means each
    // filename row is hundreds of quads, so a folder with many files (e.g. PBR
    // texture sets) adds up fast. Over-capacity geometry is also clamped in
    // Buffer::uploadData so it can never overrun this allocation.
    constexpr VkDeviceSize vertexBufSize = sizeof(UIVertex) * 131072;
    constexpr VkDeviceSize indexBufSize  = sizeof(uint32_t) * 196608;
    m_vertexBuffer.init(allocator, vertexBufSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_indexBuffer.init(allocator, indexBufSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_buffersInitialized = true;

    scan();
    LOG_INFO("ContentBrowser initialized: project '{}'", m_projectName);
}

void ContentBrowser::cleanup(VmaAllocator allocator) {
    if (m_buffersInitialized) {
        m_vertexBuffer.cleanup(allocator);
        m_indexBuffer.cleanup(allocator);
        m_buffersInitialized = false;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// FILESYSTEM SCAN — build the tree once (folders default expanded)
// ════════════════════════════════════════════════════════════════════════════

void ContentBrowser::scanDir(Node& parent) {
    std::vector<std::string> dirs, files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(parent.path, ec)) {
        std::string name = e.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;   // hide dotfiles
        if (e.is_directory(ec)) dirs.push_back(name);
        else                    files.push_back(name);
    }
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());

    for (const std::string& d : dirs) {
        Node child;
        child.name = d; child.path = parent.path + "/" + d; child.isDir = true; child.expanded = true;
        parent.children.push_back(std::move(child));
    }
    for (const std::string& f : files) {
        Node child;
        child.name = f; child.path = parent.path + "/" + f; child.isDir = false;
        parent.children.push_back(std::move(child));
    }
    // Recurse (parent.children is no longer modified past this point, so refs are stable).
    for (Node& c : parent.children) {
        if (c.isDir) scanDir(c);
    }
}

void ContentBrowser::scan() {
    m_root = Node{};
    m_root.name = m_projectName;
    m_root.path = m_projectPath;
    m_root.isDir = true;
    std::error_code ec;
    if (fs::exists(m_projectPath, ec)) scanDir(m_root);
    m_geomDirty = true;   // tree changed → force a geometry rebuild next update
}

void ContentBrowser::flattenNode(Node& n, int depth) {
    m_rows.push_back({&n, depth});
    if (n.isDir && n.expanded) {
        for (Node& c : n.children) flattenNode(c, depth + 1);
    }
}

void ContentBrowser::flatten() {
    m_rows.clear();
    for (Node& c : m_root.children) flattenNode(c, 0);
}

// ════════════════════════════════════════════════════════════════════════════
// INPUT — left press records a row (click vs drag resolved on release),
//         right press opens the context menu, char/key drive inline rename
// ════════════════════════════════════════════════════════════════════════════

bool ContentBrowser::handleMouseButton(int button, int action) {
    if (!m_visible) return false;
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return false;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);

    // An open menu eats the next click: select an item, or dismiss on a click off it.
    if (m_menuOpen) {
        float mh = m_menuItems.size() * MENU_ROW_H;
        if (mx >= m_menuX && mx < m_menuX + m_menuW && my >= m_menuY && my < m_menuY + mh) {
            int idx = static_cast<int>(std::floor((my - m_menuY) / MENU_ROW_H));
            if (idx >= 0 && idx < static_cast<int>(m_menuItems.size())) {
                int         act = m_menuItems[idx].action;
                std::string arg = m_menuItems[idx].arg;
                closeMenu();
                doAction(act, arg);
            }
        } else {
            closeMenu();
        }
        return true;
    }

    // A click anywhere commits an in-progress rename.
    if (m_renaming) { commitRename(); return true; }

    // Resize: grab the right edge (expanded only; full height, checked before the
    // toggle so the last few px win while the centered arrow stays clickable).
    if (m_expanded && my >= TOP_OFFSET && std::fabs(mx - m_width) <= RESIZE_GRAB) {
        m_resizing = true;
        return true;
    }

    float pw = m_expanded ? m_width : COLLAPSED_WIDTH;
    if (mx < 0.0 || mx >= pw || my < TOP_OFFSET) return false;   // outside the panel

    if (!m_expanded) { m_expanded = true; return true; }         // rail → expand

    // Header band: FILE menu (left), New File / New Folder icons + collapse (right).
    if (my < TOP_OFFSET + HEADER_H) {
        const float fileX = toolButtonX(false, pw), folderX = toolButtonX(true, pw);
        if (mx < FILE_BTN_W)                                  { openFileMenu(2.0f, TOP_OFFSET + HEADER_H); return true; }
        if (mx >= fileX   && mx < fileX   + TOOL_BTN_W)       { createInSelected(false); return true; }
        if (mx >= folderX && mx < folderX + TOOL_BTN_W)       { createInSelected(true);  return true; }
        if (mx >= m_width - TOGGLE_W)                         { m_expanded = false; return true; }
        return true;   // consume other header clicks
    }

    // Tree row: remember it; the toggle/open or a drag-move happens on release.
    if (my >= LIST_TOP) {
        int idx = static_cast<int>(std::floor((my - LIST_TOP + m_scroll) / ROW_H));
        if (idx >= 0 && idx < static_cast<int>(m_rows.size())) {
            m_leftDown   = true;
            m_dragActive = false;
            m_pressRow   = idx;
            m_pressPath  = m_rows[idx].node->path;
            m_pressX = mx; m_pressY = my;
        }
    }
    return true;   // click was inside the panel — consume it
}

void ContentBrowser::handleRelease() {
    if (m_resizing) { m_resizing = false; return; }

    if (m_dragActive) {
        // Released over a folder inside the panel → move; released outside the
        // panel → hand the dragged path to whoever registered (e.g. the Inspector).
        double mx = 0.0, my = 0.0;
        glfwGetCursorPos(m_window, &mx, &my);
        bool outside = mx < 0.0 || mx >= currentWidth();
        if (!m_dropTarget.empty()) {
            moveInto(m_pressPath, m_dropTarget, /*copy=*/false);
        } else if (outside && m_onExternalDrop && !m_pressPath.empty()) {
            m_onExternalDrop(m_pressPath, mx, my);
        }
    } else if (m_leftDown && !m_pressPath.empty()) {
        // No drag → it was a plain click: select the row, then toggle/open.
        m_selectedPath = m_pressPath;
        Node* n = findByPath(m_root, m_pressPath);
        if (n) {
            if (n->isDir && !n->children.empty()) n->expanded = !n->expanded;
            else if (!n->isDir && m_onFileOpen)   m_onFileOpen(n->path);
        }
    }
    m_leftDown = false; m_dragActive = false; m_pressRow = -1;
    m_pressPath.clear(); m_dropTarget.clear(); m_dropRow = -1;
}

// ── Right-click context menu ────────────────────────────────────────────────

bool ContentBrowser::handleRightPress() {
    if (!m_visible) return false;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);

    float pw = m_expanded ? m_width : COLLAPSED_WIDTH;
    if (mx < 0.0 || mx >= pw || my < TOP_OFFSET) { closeMenu(); return false; }  // not over panel

    if (m_renaming) commitRename();

    // Resolve the target row (empty area / collapsed rail → the project root).
    std::string target = m_root.path;
    bool isDir = true;
    if (m_expanded && my >= LIST_TOP) {
        int idx = static_cast<int>(std::floor((my - LIST_TOP + m_scroll) / ROW_H));
        if (idx >= 0 && idx < static_cast<int>(m_rows.size())) {
            target = m_rows[idx].node->path;
            isDir  = m_rows[idx].node->isDir;
            m_selectedPath = target;     // right-click selects the row (for keyboard ops)
        }
    }
    openMenu(target, isDir, static_cast<float>(mx), static_cast<float>(my));
    return true;
}

void ContentBrowser::openMenu(const std::string& target, bool isDir, float x, float y) {
    m_menuTarget    = target;
    m_menuTargetDir = isDir;
    m_menuItems.clear();

    bool isRoot = (target == m_root.path);
    if (isDir) {
        if (!isRoot) m_menuItems.push_back({"Open", A_OPEN});
        m_menuItems.push_back({"New Folder",   A_NEWFOLDER});
        m_menuItems.push_back({"New File",     A_NEWFILE});
        m_menuItems.push_back({"New Material", A_NEWMATERIAL});
        if (!m_clipPath.empty()) m_menuItems.push_back({"Paste", A_PASTE});
        if (!isRoot) {
            m_menuItems.push_back({"Cut",    A_CUT});
            m_menuItems.push_back({"Copy",   A_COPY});
            m_menuItems.push_back({"Rename", A_RENAME});
            m_menuItems.push_back({"Delete", A_DELETE});
        }
        m_menuItems.push_back({"Refresh", A_REFRESH});
    } else {
        m_menuItems.push_back({"Open",   A_OPEN});
        m_menuItems.push_back({"Cut",    A_CUT});
        m_menuItems.push_back({"Copy",   A_COPY});
        if (!m_clipPath.empty()) m_menuItems.push_back({"Paste", A_PASTE});  // into this file's folder
        m_menuItems.push_back({"Rename", A_RENAME});
        m_menuItems.push_back({"Delete", A_DELETE});
    }

    // Width from the longest label; clamp the box inside the window.
    size_t longest = 0;
    for (const auto& it : m_menuItems) longest = std::max(longest, it.label.size());
    m_menuW = longest * PixelFont::ADVANCE * PixelFont::SCALE + 18.0f;
    float mh = m_menuItems.size() * MENU_ROW_H;
    m_menuX = std::min(x, std::max(0.0f, m_windowWidth  - m_menuW));
    m_menuY = std::min(y, std::max(0.0f, m_windowHeight - mh));
    m_menuOpen = true;
}

void ContentBrowser::doAction(int action, const std::string& arg) {
    std::error_code ec;
    // Destination dir for create/paste: the folder itself, else its parent.
    std::string destDir = m_menuTargetDir ? m_menuTarget : parentOf(m_menuTarget);

    switch (action) {
        case A_OPEN: {
            Node* n = findByPath(m_root, m_menuTarget);
            if (n) {
                if (n->isDir) n->expanded = !n->expanded;
                else if (m_onFileOpen) m_onFileOpen(n->path);
            }
            break;
        }
        case A_NEWFOLDER: {
            std::string np = uniqueChildPath(destDir, "New folder");
            fs::create_directory(np, ec);
            m_undo.push_back({UndoKind::RemoveCreated, np, ""});
            refresh();
            if (Node* d = findByPath(m_root, destDir)) d->expanded = true;
            beginRename(np);
            break;
        }
        case A_NEWFILE: {
            std::string np = uniqueChildPath(destDir, "New file.txt");
            { std::ofstream f(np); }                 // create empty file
            m_undo.push_back({UndoKind::RemoveCreated, np, ""});
            refresh();
            if (Node* d = findByPath(m_root, destDir)) d->expanded = true;
            beginRename(np);
            break;
        }
        case A_NEWMATERIAL: {
            std::string np = uniqueChildPath(destDir, "New material.mat");
            {
                std::ofstream f(np);
                f << "# Nyx material\n"
                     "baseColor 0.8 0.8 0.85 1.0\n"
                     "metallic 0.0\n"
                     "roughness 0.5\n"
                     "albedo \n";
            }
            m_undo.push_back({UndoKind::RemoveCreated, np, ""});
            refresh();
            if (Node* d = findByPath(m_root, destDir)) d->expanded = true;
            beginRename(np);
            break;
        }
        case A_CUT:  m_clipPath = m_menuTarget; m_clipCut = true;  break;
        case A_COPY: m_clipPath = m_menuTarget; m_clipCut = false; break;
        case A_PASTE:
            if (!m_clipPath.empty() && !destDir.empty()) {
                moveInto(m_clipPath, destDir, /*copy=*/!m_clipCut);
                if (m_clipCut) m_clipPath.clear();
            }
            break;
        case A_RENAME: beginRename(m_menuTarget); break;
        case A_DELETE: {
            // Move to a temp "trash" so the delete is undoable (Ctrl+Z); fall back
            // to a permanent delete only if that move fails.
            fs::path trashDir = fs::temp_directory_path() / "NyxTrash";
            fs::create_directories(trashDir, ec);
            std::string trashPath = uniqueChildPath(trashDir.generic_string(),
                                                    fs::path(m_menuTarget).filename().string());
            ec.clear();
            fs::rename(m_menuTarget, trashPath, ec);
            if (!ec) m_undo.push_back({UndoKind::RestoreDeleted, trashPath, m_menuTarget});
            else { ec.clear(); fs::remove_all(m_menuTarget, ec); }   // permanent fallback
            if (m_onPathRemoved) m_onPathRemoved(m_menuTarget);      // close the editor tab
            if (m_clipPath == m_menuTarget)     m_clipPath.clear();
            if (m_selectedPath == m_menuTarget) m_selectedPath.clear();
            refresh();
            break;
        }
        case A_REFRESH: refresh(); break;

        // ── File menu (project) actions ──────────────────────────────────────
        case A_NEWPROJECT: {
            std::string root = parentOf(m_projectPath);          // the "projects" dir
            if (root.empty()) root = "projects";
            std::string np = uniqueChildPath(root, "New project");
            fs::create_directories(np + "/assets",  ec);
            fs::create_directories(np + "/scenes",  ec);
            fs::create_directories(np + "/procgen", ec);
            setProject(np);
            break;
        }
        case A_OPENPROJECT:   if (m_onFileMenu) m_onFileMenu("openproject"); break;  // engine opens a folder dialog
        case A_SWITCHPROJECT: if (!arg.empty()) setProject(arg); break;
        case A_SAVE:      if (m_onFileMenu) m_onFileMenu("save");      break;
        case A_SAVEALL:   if (m_onFileMenu) m_onFileMenu("saveall");   break;
        case A_SAVESCENE: if (m_onFileMenu) m_onFileMenu("savescene"); break;
        case A_EXIT:      if (m_onFileMenu) m_onFileMenu("exit");      break;
    }
}

void ContentBrowser::openFileMenu(float x, float y) {
    m_menuTarget = m_root.path; m_menuTargetDir = true;
    m_menuItems.clear();
    m_menuItems.push_back({"New Project",  A_NEWPROJECT});
    m_menuItems.push_back({"Open Project", A_OPENPROJECT});
    m_menuItems.push_back({"Save",         A_SAVE});
    m_menuItems.push_back({"Save All",     A_SAVEALL});
    m_menuItems.push_back({"Save Scene",   A_SAVESCENE});
    m_menuItems.push_back({"Refresh",      A_REFRESH});
    m_menuItems.push_back({"Exit",         A_EXIT});
    size_t longest = 0; for (const auto& it : m_menuItems) longest = std::max(longest, it.label.size());
    m_menuW = longest * PixelFont::ADVANCE * PixelFont::SCALE + 18.0f;
    float mh = m_menuItems.size() * MENU_ROW_H;
    m_menuX = std::min(x, std::max(0.0f, m_windowWidth  - m_menuW));
    m_menuY = std::min(y, std::max(0.0f, m_windowHeight - mh));
    m_menuOpen = true;
}

void ContentBrowser::createInSelected(bool folder) {
    if (!m_expanded) m_expanded = true;
    std::string dir = m_root.path;
    if (!m_selectedPath.empty()) {
        Node* n = findByPath(m_root, m_selectedPath);
        dir = (n && n->isDir) ? m_selectedPath : parentOf(m_selectedPath);
    }
    if (dir.empty()) dir = m_root.path;
    m_menuTarget = dir; m_menuTargetDir = true;          // doAction creates in destDir
    doAction(folder ? A_NEWFOLDER : A_NEWFILE);          // creates + begins inline rename
}

// Width of the uppercased project name as drawn in the header (matches addText's advance).
float ContentBrowser::projLabelWidth() const {
    if (m_projectName.empty()) return 0.0f;
    return (float)m_projectName.size() * PixelFont::ADVANCE * PixelFont::SCALE - PixelFont::SCALE;
}

// Toolbar icons sit just left of the project name: [New File][New Folder] then the name.
float ContentBrowser::toolButtonX(bool folder, float pw) const {
    float projLeft = pw - TOGGLE_W - projLabelWidth();
    float folderX  = projLeft - TOOL_GAP - TOOL_BTN_W;   // New Folder = closest to the name
    return folder ? folderX : folderX - TOOL_BTN_W;      // New File to its left
}

void ContentBrowser::setProject(const std::string& path) {
    m_projectPath = fs::path(path).generic_string();   // normalize to '/' so path math stays consistent
    m_projectName = fs::path(m_projectPath).filename().string();
    if (m_projectName.empty()) m_projectName = path;
    m_selectedPath.clear();
    m_scroll = 0.0f;
    scan();
    LOG_INFO("ContentBrowser: project '{}'", m_projectName);
}

// ── Inline rename ────────────────────────────────────────────────────────────

void ContentBrowser::beginRename(const std::string& path) {
    m_renaming        = true;
    m_renamePath      = path;
    m_renameText      = fs::path(path).filename().string();
    m_renameSelected  = true;   // whole name selected, like Explorer — typing replaces it
    closeMenu();
}

void ContentBrowser::commitRename() {
    if (!m_renaming) return;
    m_renaming = false;
    std::string oldName = fs::path(m_renamePath).filename().string();
    // Trim whitespace.
    size_t a = m_renameText.find_first_not_of(" \t");
    size_t b = m_renameText.find_last_not_of(" \t");
    std::string name = (a == std::string::npos) ? "" : m_renameText.substr(a, b - a + 1);
    if (!name.empty() && name != oldName) {
        std::string dest = parentOf(m_renamePath) + "/" + name;
        std::error_code ec;
        if (!fs::exists(dest, ec)) {
            fs::rename(m_renamePath, dest, ec);
            if (!ec) {
                m_undo.push_back({UndoKind::MoveBack, dest, m_renamePath});  // undo: rename back
                if (m_selectedPath == m_renamePath) m_selectedPath = dest;   // selection follows
            }
        }
    }
    m_renamePath.clear();
    m_renameText.clear();
    refresh();
}

void ContentBrowser::cancelRename() {
    m_renaming = false;
    m_renamePath.clear();
    m_renameText.clear();
}

void ContentBrowser::handleChar(unsigned int cp) {
    if (!m_renaming) return;
    if (cp < 32 || cp > 126) return;
    char c = static_cast<char>(cp);
    if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) return;  // illegal in names
    if (m_renameSelected) { m_renameText.clear(); m_renameSelected = false; }  // replace selection
    m_renameText.push_back(c);
}

bool ContentBrowser::handleKey(int key, int action, int mods) {
    // Inline rename captures all keys.
    if (m_renaming) {
        if (action != GLFW_RELEASE) {
            if (key == GLFW_KEY_BACKSPACE) {
                if (m_renameSelected) { m_renameText.clear(); m_renameSelected = false; }
                else if (!m_renameText.empty()) m_renameText.pop_back();
            }
            else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) commitRename();
            else if (key == GLFW_KEY_ESCAPE) cancelRename();
        }
        return true;
    }

    // When the tree has focus: Ctrl+C / X / V copy/cut/paste the selected row,
    // Ctrl+Z undoes the last file operation.
    if (!m_focused) return false;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return false;
    if ((mods & GLFW_MOD_CONTROL) == 0) return false;
    switch (key) {
        case GLFW_KEY_C:
            if (!m_selectedPath.empty()) { m_clipPath = m_selectedPath; m_clipCut = false; }
            return true;
        case GLFW_KEY_X:
            if (!m_selectedPath.empty()) { m_clipPath = m_selectedPath; m_clipCut = true; }
            return true;
        case GLFW_KEY_V: {
            if (m_clipPath.empty()) return true;
            std::string destDir = m_root.path;
            if (!m_selectedPath.empty()) {
                Node* sel = findByPath(m_root, m_selectedPath);
                destDir = (sel && sel->isDir) ? m_selectedPath : parentOf(m_selectedPath);
            }
            moveInto(m_clipPath, destDir, /*copy=*/!m_clipCut);
            if (m_clipCut) m_clipPath.clear();
            return true;
        }
        case GLFW_KEY_Z:
            doUndo();
            return true;
    }
    return false;
}

// ── Filesystem mutation helpers ──────────────────────────────────────────────

std::string ContentBrowser::parentOf(const std::string& path) {
    return fs::path(path).parent_path().generic_string();
}

bool ContentBrowser::isSubPath(const std::string& parent, const std::string& child) {
    if (child == parent) return true;
    return child.rfind(parent + "/", 0) == 0;
}

std::string ContentBrowser::uniqueChildPath(const std::string& dir, const std::string& name) const {
    fs::path base(name);
    std::string stem = base.stem().string();
    std::string ext  = base.extension().string();
    std::string cand = dir + "/" + name;
    std::error_code ec;
    for (int i = 2; fs::exists(cand, ec); ++i)
        cand = dir + "/" + stem + " (" + std::to_string(i) + ")" + ext;
    return cand;
}

void ContentBrowser::moveInto(const std::string& src, const std::string& destDir, bool copy) {
    if (src.empty() || destDir.empty()) return;
    if (!copy && (isSubPath(src, destDir) || destDir == parentOf(src))) return;  // no-op / invalid

    std::string dest = uniqueChildPath(destDir, fs::path(src).filename().string());
    std::error_code ec;
    if (copy) {
        fs::copy(src, dest, fs::copy_options::recursive, ec);
        if (!ec) m_undo.push_back({UndoKind::RemoveCreated, dest, ""});   // undo: delete the copy
    } else {
        fs::rename(src, dest, ec);
        if (ec) {   // cross-volume fallback
            ec.clear();
            fs::copy(src, dest, fs::copy_options::recursive, ec);
            fs::remove_all(src, ec);
        }
        if (fs::exists(dest)) {
            m_undo.push_back({UndoKind::MoveBack, dest, src});           // undo: move back
            if (m_selectedPath == src) m_selectedPath = dest;            // selection follows
        }
    }
    refresh();
    if (Node* d = findByPath(m_root, destDir)) d->expanded = true;
}

// Reverse the last file operation (Ctrl+Z): move-back, delete-created, or restore
// a deleted item from the temp trash.
void ContentBrowser::doUndo() {
    if (m_undo.empty()) return;
    UndoOp op = m_undo.back();
    m_undo.pop_back();
    std::error_code ec;
    switch (op.kind) {
        case UndoKind::MoveBack:        // move op.a back to op.b
            if (fs::exists(op.a, ec) && !fs::exists(op.b, ec)) fs::rename(op.a, op.b, ec);
            if (m_selectedPath == op.a) m_selectedPath = op.b;
            break;
        case UndoKind::RemoveCreated:   // delete op.a (created file/folder or pasted copy)
            fs::remove_all(op.a, ec);
            if (m_clipPath == op.a)     m_clipPath.clear();
            if (m_selectedPath == op.a) m_selectedPath.clear();
            if (m_onPathRemoved)        m_onPathRemoved(op.a);
            break;
        case UndoKind::RestoreDeleted:  // move trashed op.a back to original op.b
            if (fs::exists(op.a, ec)) fs::rename(op.a, op.b, ec);
            break;
    }
    refresh();
}

// ── Tree refresh (rescan, preserving expanded folders by path) ────────────────

void ContentBrowser::collectExpanded(const Node& n, std::set<std::string>& out) const {
    if (n.isDir && n.expanded) out.insert(n.path);
    for (const Node& c : n.children) collectExpanded(c, out);
}

void ContentBrowser::applyExpanded(Node& n, const std::set<std::string>& exp) {
    if (n.isDir) n.expanded = exp.count(n.path) > 0;
    for (Node& c : n.children) applyExpanded(c, exp);
}

void ContentBrowser::refresh() {
    std::set<std::string> exp;
    collectExpanded(m_root, exp);
    scan();
    applyExpanded(m_root, exp);
}

ContentBrowser::Node* ContentBrowser::findByPath(Node& n, const std::string& path) {
    if (n.path == path) return &n;
    for (Node& c : n.children)
        if (Node* hit = findByPath(c, path)) return hit;
    return nullptr;
}

bool ContentBrowser::handleScroll(double yoffset) {
    if (!m_visible || !m_expanded) return false;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    if (mx < 0.0 || mx >= m_width || my < TOP_OFFSET) return false;   // not over the panel
    m_scroll -= static_cast<float>(yoffset) * 28.0f;                  // wheel up → toward top
    if (m_scroll < 0.0f) m_scroll = 0.0f;                             // upper clamp done in update()
    return true;
}

bool ContentBrowser::overResizeEdge() const {
    if (!m_visible || !m_expanded) return false;
    if (m_resizing) return true;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    return std::fabs(mx - m_width) <= RESIZE_GRAB && my >= TOP_OFFSET && my < m_windowHeight;
}

// ════════════════════════════════════════════════════════════════════════════
// UPDATE — rebuild geometry
// ════════════════════════════════════════════════════════════════════════════

void ContentBrowser::update(float windowWidth, float windowHeight, bool cursorActive) {
    m_windowWidth  = windowWidth;
    m_windowHeight = windowHeight;
    m_overButton   = false;          // re-tested below; cleared so it doesn't latch

    if (!m_visible) { m_indexCount = 0; return; }

    flatten();
    m_vertices.clear();
    m_indices.clear();

    // Colors (authored in sRGB; ui.frag converts to linear).
    const glm::vec4 panelBg    = {0.065f, 0.070f, 0.085f, 0.97f};
    const glm::vec4 headerBg   = {0.10f,  0.105f, 0.130f, 1.0f};
    const glm::vec4 border     = {0.22f,  0.24f,  0.30f,  1.0f};
    const glm::vec4 hoverBg    = {0.16f,  0.17f,  0.22f,  1.0f};
    const glm::vec4 labelCol   = {0.50f,  0.54f,  0.60f,  1.0f};   // "CONTENT"
    const glm::vec4 nameCol    = {0.42f,  1.00f,  0.66f,  1.0f};   // project name (mint)
    const glm::vec4 folderTxt  = {0.92f,  0.88f,  0.72f,  1.0f};
    const glm::vec4 fileTxt    = {0.80f,  0.83f,  0.88f,  1.0f};
    const glm::vec4 folderIcon = {0.95f,  0.78f,  0.32f,  1.0f};
    const glm::vec4 chevronCol = {0.60f,  0.63f,  0.70f,  1.0f};

    const glm::vec4 toggleCol  = {0.78f,  0.80f,  0.86f,  1.0f};   // header toggle chevron
    const glm::vec4 resizeCol  = {0.42f,  1.00f,  0.66f,  1.0f};   // resize-edge highlight (mint)
    const glm::vec4 dropHi     = {0.42f,  1.00f,  0.66f,  0.22f};  // drag drop-target row (mint wash)
    const glm::vec4 selRowBg   = {0.14f,  0.30f,  0.24f,  1.0f};   // selected row (tree focused)
    const glm::vec4 selRowDim  = {0.15f,  0.16f,  0.20f,  1.0f};   // selected row (tree unfocused)
    const glm::vec4 editBg     = {0.04f,  0.05f,  0.07f,  1.0f};   // rename input field
    const glm::vec4 editTxt    = {0.95f,  0.97f,  1.00f,  1.0f};
    const glm::vec4 selBg      = {0.18f,  0.40f,  0.66f,  1.0f};   // rename selection highlight
    const glm::vec4 caretCol   = {0.42f,  1.00f,  0.66f,  1.0f};
    const glm::vec4 menuBg     = {0.11f,  0.12f,  0.15f,  1.0f};   // context menu
    const glm::vec4 menuBorder = {0.30f,  0.55f,  0.42f,  1.0f};
    const glm::vec4 menuHov    = {0.20f,  0.34f,  0.27f,  1.0f};
    const glm::vec4 menuTxt    = {0.88f,  0.90f,  0.94f,  1.0f};
    const glm::vec4 ghostBg    = {0.16f,  0.30f,  0.23f,  0.92f};  // drag ghost label
    const glm::vec4 ghostTxt   = {0.95f,  1.00f,  0.97f,  1.0f};

    const float fsz = PixelFont::SCALE;

    double mx = -1.0, my = -1.0;
    if (cursorActive) glfwGetCursorPos(m_window, &mx, &my);

    // ── Geometry cache: skip the (expensive) per-pixel-font rebuild when nothing
    // that affects the rendered tree changed. When the cursor isn't over the panel
    // and no interaction is active, the geometry is static, so reuse the last
    // frame's upload (m_indexCount + GPU buffers are left untouched). Without this
    // the whole asset tree is regenerated + re-uploaded (~MBs) every frame.
    {
        bool cursorOver = cursorActive && mx >= 0.0 && mx < currentWidth() + RESIZE_GRAB && my >= TOP_OFFSET;
        bool busy = m_resizing || m_leftDown || m_renaming || m_menuOpen || m_dragActive;
        // Include cursorOver in the change set: when the cursor leaves the
        // panel, the cached geometry still has the resize-edge highlight quad
        // baked into the vertex buffer. Rebuild exactly once to clear it.
        bool changed = windowWidth != m_cacheW || windowHeight != m_cacheH || m_width != m_cacheWidth
                    || m_expanded != m_cacheExpanded || m_focused != m_cacheFocused
                    || cursorOver != m_cacheCursorOver || m_geomDirty;
        if (m_haveGeom && !cursorOver && !busy && !changed) return;   // reuse cached geometry
        m_geomDirty     = false;
        m_cacheW        = windowWidth;  m_cacheH = windowHeight;  m_cacheWidth = m_width;
        m_cacheExpanded = m_expanded;   m_cacheFocused = m_focused;
        m_cacheCursorOver = cursorOver;
        m_haveGeom      = true;
    }

    // Apply an in-progress resize drag (stop if the button was let go).
    if (m_resizing) {
        bool leftDown = cursorActive && glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (!leftDown) m_resizing = false;
        else m_width = std::clamp(static_cast<float>(mx), MIN_WIDTH, MAX_WIDTH);
    }

    // Drag detection: once the cursor moves past a threshold from the press, the
    // pending row becomes a drag; resolve the drop target (a folder, or a file's
    // parent) under the cursor, skipping invalid moves (into self/own subtree).
    if (m_leftDown && !m_renaming && cursorActive) {
        bool leftHeld = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (leftHeld) {
            if (!m_dragActive &&
                (std::fabs(mx - m_pressX) > 4.0 || std::fabs(my - m_pressY) > 4.0))
                m_dragActive = true;

            if (m_dragActive) {
                m_dropTarget.clear();
                m_dropRow = -1;
                bool overPanel = m_expanded && mx >= 0.0 && mx < m_width && my >= TOP_OFFSET;
                if (overPanel) {
                    std::string dropDir = m_root.path;
                    int hitRow = -1;
                    if (my >= LIST_TOP) {
                        int idx = (int)std::floor((my - LIST_TOP + m_scroll) / ROW_H);
                        if (idx >= 0 && idx < (int)m_rows.size()) {
                            Node* n = m_rows[idx].node;
                            if (n->isDir) { dropDir = n->path; hitRow = idx; }
                            else            dropDir = parentOf(n->path);
                        }
                    }
                    if (!isSubPath(m_pressPath, dropDir) && dropDir != parentOf(m_pressPath)) {
                        m_dropTarget = dropDir;
                        m_dropRow    = hitRow;
                    }
                }
            }
        }
    }

    const float pw     = m_expanded ? m_width : COLLAPSED_WIDTH;
    const float panelH = windowHeight - TOP_OFFSET;

    // Background (drawn first; content draws over it, header/chrome over content).
    addQuad(0.0f, TOP_OFFSET, pw, panelH, panelBg);

    if (!m_expanded) {
        bool railHover = cursorActive && mx >= 0.0 && mx < pw && my >= TOP_OFFSET;
        if (railHover) m_overButton = true;
        addQuad(0.0f, TOP_OFFSET, pw, HEADER_H, railHover ? hoverBg : headerBg);
        addArrow(pw * 0.5f, TOP_OFFSET + HEADER_H * 0.5f, true, toggleCol);   // ▶ expand
        addQuad(pw - 1.0f, TOP_OFFSET, 1.0f, panelH, border);
    } else {
        // Clamp scroll to the content.
        float viewH    = windowHeight - LIST_TOP;
        float contentH = static_cast<float>(m_rows.size()) * ROW_H;
        float maxScroll = std::max(0.0f, contentH - viewH);
        m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);

        int hovered = -1;
        if (cursorActive && mx >= 0.0 && mx < pw && my >= LIST_TOP) {
            int idx = (int)std::floor((my - LIST_TOP + m_scroll) / ROW_H);
            if (idx >= 0 && idx < (int)m_rows.size()) hovered = idx;
        }
        m_overButton = (hovered >= 0);   // tree row → pointer cursor

        // CONTENT — rows (before the header so scrolled-off-top rows get covered).
        for (int i = 0; i < (int)m_rows.size(); ++i) {
            float rowY = LIST_TOP + i * ROW_H - m_scroll;
            if (rowY + ROW_H <= LIST_TOP) continue;   // fully above the content area
            if (rowY >= windowHeight) break;          // below the window edge

            Node* n = m_rows[i].node;
            int   depth = m_rows[i].depth;
            if (!n->path.empty() && n->path == m_selectedPath)                  // selected row
                addQuad(1.0f, rowY, pw - 2.0f, ROW_H, m_focused ? selRowBg : selRowDim);
            if (i == hovered)  addQuad(1.0f, rowY, pw - 2.0f, ROW_H, hoverBg);
            if (i == m_dropRow) addQuad(1.0f, rowY, pw - 2.0f, ROW_H, dropHi);  // drag drop target

            float x      = 10.0f + depth * 12.0f;
            float iconX  = x + 10.0f;
            float textX  = x + 24.0f;
            float iconY  = rowY + std::floor((ROW_H - 8.0f) * 0.5f);
            float textY  = rowY + std::floor((ROW_H - PixelFont::CELL_H) * 0.5f);

            // Inline rename: an editable field with the (real-case) name + caret.
            if (m_renaming && n->path == m_renamePath) {
                addQuad(textX - 2.0f, rowY + 1.0f, pw - (textX - 2.0f) - 3.0f, ROW_H - 2.0f, editBg);
                float textW = m_renameText.size() * PixelFont::ADVANCE * fsz;
                if (m_renameSelected && !m_renameText.empty())  // selection wash behind the name
                    addQuad(textX - 1.0f, rowY + 2.0f, std::min(textW + 1.0f, pw - 3.0f - textX), ROW_H - 4.0f, selBg);
                addText(textX, textY, m_renameText, fsz, editTxt, pw - 3.0f);
                float caretX = textX + textW;
                if (!m_renameSelected && caretX < pw - 3.0f)
                    addQuad(caretX, rowY + 2.0f, 1.0f, ROW_H - 4.0f, caretCol);
                if (n->isDir) addFolderIcon(iconX, iconY, false, folderIcon);
                continue;
            }

            std::string nm = n->name;
            std::transform(nm.begin(), nm.end(), nm.begin(), [](unsigned char ch){ return (char)std::toupper(ch); });

            // A "cut" item is shown dimmed (translucent) until it's pasted, so Cut
            // gives visible feedback like Explorer.
            const bool isCut = m_clipCut && n->path == m_clipPath;
            auto dim = [&](glm::vec4 c){ if (isCut) c.a *= 0.40f; return c; };

            if (n->isDir) {
                bool hasChildren = !n->children.empty();
                if (hasChildren) addChevron(x, rowY, n->expanded, dim(chevronCol));
                addFolderIcon(iconX, iconY, hasChildren && n->expanded, dim(folderIcon));
                addText(textX, textY, nm, fsz, dim(folderTxt), pw - 2.0f);
            } else {
                std::string ext = fs::path(n->name).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch){ return (char)std::tolower(ch); });
                FileCat cat = fileCategory(ext);
                glm::vec4 ic;
                switch (cat) {
                    case FileCat::Object:   ic = {0.42f, 0.68f, 1.00f, 1.0f}; break;  // blue ball
                    case FileCat::Material: ic = {0.96f, 0.66f, 0.30f, 1.0f}; break;  // amber ball
                    case FileCat::Image:  ic = {0.48f, 0.85f, 0.55f, 1.0f}; break;  // green
                    case FileCat::Script: ic = {0.78f, 0.58f, 0.96f, 1.0f}; break;  // purple
                    case FileCat::Audio:  ic = {0.96f, 0.55f, 0.78f, 1.0f}; break;  // pink
                    case FileCat::Doc:    ic = {0.82f, 0.85f, 0.90f, 1.0f}; break;  // light grey
                    case FileCat::Data:   ic = {0.40f, 0.82f, 0.82f, 1.0f}; break;  // teal
                    default:              ic = {0.62f, 0.64f, 0.70f, 1.0f}; break;  // grey
                }
                addFileIcon(iconX, rowY, cat, dim(ic));
                addText(textX, textY, nm, fsz, dim(fileTxt), pw - 2.0f);
            }
        }

        // CHROME — header band + FILE menu button + project name + ◀ collapse toggle.
        addQuad(0.0f, TOP_OFFSET, pw, HEADER_H, headerBg);
        float hy = TOP_OFFSET + std::floor((HEADER_H - PixelFont::CELL_H) * 0.5f);
        // "FILE ▾" button (opens the project menu).
        bool fHover = cursorActive && mx >= 0.0 && mx < FILE_BTN_W
                      && my >= TOP_OFFSET && my < TOP_OFFSET + HEADER_H;
        if (fHover) m_overButton = true;
        if (fHover || m_menuOpen) addQuad(0.0f, TOP_OFFSET, FILE_BTN_W, HEADER_H, hoverBg);
        addText(6.0f, hy, "FILE", fsz, (fHover || m_menuOpen) ? nameCol : labelCol, FILE_BTN_W);

        // New File / New Folder toolbar icons — right side, just left of the project name.
        {
            const glm::vec4 plusCol = {0.48f, 0.92f, 0.58f, 1.0f};   // green "+"
            const glm::vec4 pageCol = {0.82f, 0.85f, 0.90f, 1.0f};
            static const uint8_t PAGE[8] = {0x7E, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00};
            float iconY   = TOP_OFFSET + std::floor((HEADER_H - 8.0f) * 0.5f);
            float fileX   = toolButtonX(false, pw);
            float folderX = toolButtonX(true,  pw);
            auto toolHover = [&](float bx) {
                return cursorActive && mx >= bx && mx < bx + TOOL_BTN_W
                       && my >= TOP_OFFSET && my < TOP_OFFSET + HEADER_H;
            };
            if (toolHover(fileX))   { m_overButton = true; addQuad(fileX, TOP_OFFSET, TOOL_BTN_W, HEADER_H, hoverBg); }
            addIconBitmap(fileX + 3.0f, iconY, PAGE, pageCol);
            addPlus(fileX + 13.0f, iconY + 5.0f, 3.0f, plusCol);

            if (toolHover(folderX)) { m_overButton = true; addQuad(folderX, TOP_OFFSET, TOOL_BTN_W, HEADER_H, hoverBg); }
            addFolderIcon(folderX + 3.0f, iconY, false, folderIcon);
            addPlus(folderX + 14.0f, iconY + 5.0f, 3.0f, plusCol);
        }

        std::string proj = m_projectName;
        std::transform(proj.begin(), proj.end(), proj.begin(), [](unsigned char ch){ return (char)std::toupper(ch); });
        addText(pw - TOGGLE_W - projLabelWidth(), hy, proj, fsz, nameCol, pw - TOGGLE_W);

        bool tHover = cursorActive && mx >= pw - TOGGLE_W && mx < pw
                      && my >= TOP_OFFSET && my < TOP_OFFSET + HEADER_H;
        if (tHover) { m_overButton = true; addQuad(pw - TOGGLE_W, TOP_OFFSET, TOGGLE_W, HEADER_H, hoverBg); }
        addArrow(pw - TOGGLE_W * 0.5f, TOP_OFFSET + HEADER_H * 0.5f, false, toggleCol);  // ◀ collapse

        // Right border + resize-edge highlight (full height, incl. the header).
        addQuad(pw - 1.0f, TOP_OFFSET, 1.0f, panelH, border);
        bool edgeHover = cursorActive && std::fabs(mx - m_width) <= RESIZE_GRAB && my >= TOP_OFFSET;
        if (edgeHover || m_resizing)
            addQuad(m_width - 1.0f, TOP_OFFSET, 2.0f, panelH, resizeCol);
    }

    // Drag ghost — a small label that follows the cursor while dragging.
    if (m_dragActive && cursorActive && !m_pressPath.empty()) {
        std::string nm = fs::path(m_pressPath).filename().string();
        float gw = nm.size() * PixelFont::ADVANCE * fsz + 14.0f;
        float gx = static_cast<float>(mx) + 12.0f, gy = static_cast<float>(my) + 2.0f;
        addQuad(gx, gy, gw, ROW_H + 2.0f, ghostBg);
        addText(gx + 6.0f, gy + std::floor((ROW_H + 2.0f - PixelFont::CELL_H) * 0.5f),
                nm, fsz, ghostTxt, gx + gw);
    }

    // Context menu — drawn last so it sits on top of everything in the panel.
    if (m_menuOpen) {
        float mh = m_menuItems.size() * MENU_ROW_H;
        addQuad(m_menuX - 1.0f, m_menuY - 1.0f, m_menuW + 2.0f, mh + 2.0f, menuBorder);
        addQuad(m_menuX, m_menuY, m_menuW, mh, menuBg);

        int hov = -1;
        if (cursorActive && mx >= m_menuX && mx < m_menuX + m_menuW && my >= m_menuY && my < m_menuY + mh)
            hov = (int)std::floor((my - m_menuY) / MENU_ROW_H);

        for (int i = 0; i < (int)m_menuItems.size(); ++i) {
            float iy = m_menuY + i * MENU_ROW_H;
            if (i == hov) addQuad(m_menuX, iy, m_menuW, MENU_ROW_H, menuHov);
            addText(m_menuX + 8.0f, iy + std::floor((MENU_ROW_H - PixelFont::CELL_H) * 0.5f),
                    m_menuItems[i].label, fsz, menuTxt, m_menuX + m_menuW - 4.0f);
        }
    }

    if (!m_vertices.empty()) {
        m_vertexBuffer.uploadData(m_allocator, m_vertices.data(), m_vertices.size() * sizeof(UIVertex));
        m_indexBuffer.uploadData(m_allocator, m_indices.data(), m_indices.size() * sizeof(uint32_t));
        // Cap the draw to the index buffer's capacity (196608) so an over-large tree
        // (clamped in uploadData) never issues an out-of-bounds indexed draw.
        m_indexCount = (uint32_t)std::min<size_t>(m_indices.size(), 196608);
    } else {
        m_indexCount = 0;
    }
}

void ContentBrowser::draw(VkCommandBuffer cmd) {
    if (!m_visible || m_indexCount == 0) return;
    VkBuffer buffers[] = { m_vertexBuffer.getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

// ════════════════════════════════════════════════════════════════════════════
// GEOMETRY / TEXT HELPERS
// ════════════════════════════════════════════════════════════════════════════

void ContentBrowser::addQuad(float x, float y, float w, float h, const glm::vec4& color) {
    const glm::vec2 z2{0.0f, 0.0f};
    const glm::vec4 z4{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t base = (uint32_t)m_vertices.size();
    m_vertices.push_back({{x,     y},     color, z2, z4, z4});
    m_vertices.push_back({{x + w, y},     color, z2, z4, z4});
    m_vertices.push_back({{x + w, y + h}, color, z2, z4, z4});
    m_vertices.push_back({{x,     y + h}, color, z2, z4, z4});
    m_indices.push_back(base);     m_indices.push_back(base + 1); m_indices.push_back(base + 2);
    m_indices.push_back(base + 2); m_indices.push_back(base + 3); m_indices.push_back(base);
}

void ContentBrowser::addBall(float cx, float cy, float radius, const glm::vec4& color) {
    const float hw = radius + 1.5f;
    const glm::vec2 off[4] = {{-hw,-hw},{hw,-hw},{hw,hw},{-hw,hw}};
    const glm::vec4 data0{radius, 0.0f, 0.0f, 0.0f};
    const glm::vec4 data1{4.0f,   0.0f, 0.0f, 0.0f};   // shape 4 = shaded sphere
    uint32_t base = (uint32_t)m_vertices.size();
    for (const glm::vec2& o : off) m_vertices.push_back({{cx + o.x, cy + o.y}, color, o, data0, data1});
    m_indices.push_back(base);     m_indices.push_back(base + 1); m_indices.push_back(base + 2);
    m_indices.push_back(base + 2); m_indices.push_back(base + 3); m_indices.push_back(base);
}

// ── File-type icons ──────────────────────────────────────────────────────────

ContentBrowser::FileCat ContentBrowser::fileCategory(const std::string& ext) {
    static const std::set<std::string> obj = {".obj",".gltf",".glb",".fbx",".stl",".ply",".dae",".3ds"};
    static const std::set<std::string> img = {".png",".jpg",".jpeg",".bmp",".tga",".gif",".hdr",".psd",".pic"};
    static const std::set<std::string> scr = {".lua",".py",".js",".ts",".cpp",".cxx",".cc",".c",".h",".hpp",
                                              ".hh",".inl",".glsl",".vert",".frag",".comp",".geom",".cmake",
                                              ".sh",".bat",".ps1"};
    static const std::set<std::string> aud = {".wav",".mp3",".ogg",".flac",".aiff",".aif",".mid",".midi",".opus"};
    static const std::set<std::string> doc = {".txt",".md",".markdown",".log",".rtf"};
    static const std::set<std::string> dat = {".json",".xml",".yaml",".yml",".toml",".ini",".cfg",".csv",
                                              ".bin",".spv",".dat",".mtl"};
    if (ext == ".mat")  return FileCat::Material;
    if (obj.count(ext)) return FileCat::Object;
    if (img.count(ext)) return FileCat::Image;
    if (scr.count(ext)) return FileCat::Script;
    if (aud.count(ext)) return FileCat::Audio;
    if (doc.count(ext)) return FileCat::Doc;
    if (dat.count(ext)) return FileCat::Data;
    return FileCat::Other;
}

// Render an 8x8 1-bit pictograph (MSB = leftmost column) as 1px quads.
void ContentBrowser::addIconBitmap(float ix, float iy, const uint8_t rows[8], const glm::vec4& color) {
    for (int r = 0; r < 8; ++r) {
        uint8_t b = rows[r];
        for (int c = 0; c < 8; ++c)
            if (b & (1 << (7 - c))) addQuad(ix + c, iy + r, 1.0f, 1.0f, color);
    }
}

void ContentBrowser::addFileIcon(float iconX, float rowY, FileCat cat, const glm::vec4& color) {
    // 3D assets and materials keep the shaded sphere — the engine's signature
    // shape (a material IS previewed on a sphere); the color tells them apart.
    if (cat == FileCat::Object || cat == FileCat::Material) {
        addBall(iconX + 4.0f, rowY + ROW_H * 0.5f, 4.0f, color);
        return;
    }
    static const uint8_t IMG[8] = {0xFF,0x81,0x85,0x81,0x99,0xBD,0xFF,0x00};  // framed photo: sun + mountains
    static const uint8_t SCR[8] = {0x00,0x24,0x4A,0x89,0x91,0x52,0x24,0x00};  // code  < / >
    static const uint8_t AUD[8] = {0x0C,0x0E,0x0C,0x08,0x08,0x78,0xF0,0x70};  // music note
    static const uint8_t DOC[8] = {0xFE,0x82,0xBA,0x82,0xBA,0x82,0xFE,0x00};  // document with lines
    static const uint8_t DAT[8] = {0x7C,0x82,0x7C,0x82,0x7C,0x82,0x7C,0x00};  // database cylinder
    static const uint8_t OTH[8] = {0x7E,0x42,0x42,0x42,0x42,0x42,0x7E,0x00};  // plain page
    const uint8_t* rows = OTH;
    switch (cat) {
        case FileCat::Image:  rows = IMG; break;
        case FileCat::Script: rows = SCR; break;
        case FileCat::Audio:  rows = AUD; break;
        case FileCat::Doc:    rows = DOC; break;
        case FileCat::Data:   rows = DAT; break;
        default: break;
    }
    addIconBitmap(iconX, rowY + 2.0f, rows, color);
}

// Small filled triangle: right-pointing when collapsed, down-pointing when expanded.
void ContentBrowser::addChevron(float x, float rowY, bool expanded, const glm::vec4& color) {
    if (expanded) {
        // Down-pointing ▼ (5 wide, 3 tall), apex at bottom.
        float top = rowY + std::floor((ROW_H - 3.0f) * 0.5f);
        for (int r = 0; r < 3; ++r) addQuad(x + 1.0f + r, top + r, 5.0f - 2.0f * r, 1.0f, color);
    } else {
        // Right-pointing ▶ (4 wide, 7 tall), apex at right-middle.
        float top = rowY + std::floor((ROW_H - 7.0f) * 0.5f);
        for (int r = 0; r < 7; ++r) {
            float len = 4.0f - std::fabs(static_cast<float>(r - 3));
            if (len > 0.0f) addQuad(x + 1.0f, top + r, len, 1.0f, color);
        }
    }
}

// Folder glyph (flat, front-facing): tab + body. When open, a dark slot near the
// top shows the folder's interior.
void ContentBrowser::addFolderIcon(float iconX, float iconY, bool open, const glm::vec4& color) {
    addQuad(iconX, iconY + 1.0f, 5.0f, 2.0f, color);       // tab
    addQuad(iconX, iconY + 2.0f, 9.0f, 6.0f, color);       // body
    if (open) {
        glm::vec4 interior = {color.r * 0.40f, color.g * 0.40f, color.b * 0.40f, color.a};
        addQuad(iconX + 1.0f, iconY + 3.0f, 7.0f, 2.0f, interior);   // opening
    }
}

// Small triangle (4 wide, 7 tall) centered at (cx,cy): ▶ (right) to expand the
// collapsed rail, ◀ (left) to collapse the panel.
void ContentBrowser::addArrow(float cx, float cy, bool right, const glm::vec4& color) {
    for (int r = 0; r < 7; ++r) {
        float len = 4.0f - std::fabs(static_cast<float>(r - 3));
        if (len <= 0.0f) continue;
        float x = right ? (cx - 2.0f) : (cx + 2.0f - len);
        addQuad(x, cy - 3.0f + r, len, 1.0f, color);
    }
}

void ContentBrowser::addPlus(float cx, float cy, float r, const glm::vec4& color) {
    addQuad(cx - r, cy - 1.0f, 2.0f * r, 2.0f, color);   // horizontal bar
    addQuad(cx - 1.0f, cy - r, 2.0f, 2.0f * r, color);   // vertical bar
}

void ContentBrowser::addGlyph(float x, float y, char c, float s, const glm::vec4& color) {
    const uint8_t* rows = PixelFont::glyphRows(c);
    if (!rows) return;
    for (int r = 0; r < PixelFont::CELL_H; ++r) {
        uint8_t bits = rows[r];
        for (int col = 0; col < PixelFont::CELL_W; ++col)
            if (bits & (1 << (PixelFont::CELL_W - 1 - col)))
                addQuad(x + col * s, y + r * s, s, s, color);
    }
}

float ContentBrowser::addText(float x, float y, const std::string& text, float s, const glm::vec4& color, float maxX) {
    float cx = x;
    for (char c : text) {
        if (cx + PixelFont::CELL_W * s > maxX) break;   // clip to the panel's right edge
        addGlyph(cx, y, c, s, color);
        cx += PixelFont::ADVANCE * s;
    }
    return cx - x;
}

} // namespace Nyx
