#include "ui/SceneHierarchy.h"
#include "ui/PixelFont.h"
#include "ecs/Registry.h"
#include "ecs/components/MeshComponent.h"
#include "ecs/components/LightComponent.h"
#include "ecs/components/EnvironmentComponent.h"
#include "ecs/components/TransformComponent.h"
#include "ecs/components/NameComponent.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace Nyx {

void SceneHierarchy::init(VmaAllocator allocator, GLFWwindow* window) {
    m_allocator = allocator;
    m_window    = window;
    constexpr VkDeviceSize vertexBufSize = sizeof(UIVertex) * VERT_CAP;
    constexpr VkDeviceSize indexBufSize  = sizeof(uint32_t) * IDX_CAP;
    m_vertexBuffer.init(allocator, vertexBufSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_indexBuffer.init(allocator, indexBufSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_buffersInitialized = true;
}

void SceneHierarchy::cleanup(VmaAllocator allocator) {
    if (m_buffersInitialized) {
        m_vertexBuffer.cleanup(allocator);
        m_indexBuffer.cleanup(allocator);
        m_buffersInitialized = false;
    }
}

// Build a tree of rows: roots (parent == NULL_ENTITY) first, then their children
// recursively if the parent is in m_expandedGroups. Order within each sibling band
// follows m_order (the manual drag-reorder); Environment is pinned to the top.
void SceneHierarchy::buildRows(Registry& reg) {
    m_rows.clear();
    std::unordered_set<Entity> seen;

    auto makeRow = [&](Entity e, int depth) -> Row {
        Row row{};
        row.entity = e;
        row.depth  = depth;
        if (reg.has<EnvironmentComponent>(e)) {
            row.kind  = Kind::Environment;
            row.label = "Environment";
        } else if (reg.has<LightComponent>(e)) {
            row.kind = Kind::Light;
            const auto& lc = reg.get<LightComponent>(e);
            row.label = (lc.type == LightComponent::Type::Directional) ? "Directional Light" : "Point Light";
        } else if (reg.has<MeshComponent>(e)) {
            row.kind  = Kind::Mesh;
            row.label = "Mesh " + std::to_string(e);
        } else {
            row.kind  = Kind::Other;
            row.label = "Group " + std::to_string(e);   // empty entity → treat as group
        }
        // A user-set name overrides the kind-derived default label entirely.
        if (reg.has<NameComponent>(e)) {
            const auto& nc = reg.get<NameComponent>(e);
            if (!nc.name.empty()) row.label = nc.name;
        }
        return row;
    };

    // Collect every entity that has any of the displayed components, plus build a
    // parent → children adjacency so we can emit a tree below. consider() is the
    // single point of de-dup.
    std::vector<Entity> all;
    std::unordered_set<Entity> seenAll;
    auto consider = [&](Entity e) {
        if (e == NULL_ENTITY || !seenAll.insert(e).second) return;
        all.push_back(e);
    };
    auto& envPool = reg.pool<EnvironmentComponent>();
    for (size_t i = 0; i < envPool.size(); ++i) consider(envPool.getEntity(i));
    auto& meshPool = reg.pool<MeshComponent>();
    for (size_t i = 0; i < meshPool.size(); ++i) consider(meshPool.getEntity(i));
    auto& lightPool = reg.pool<LightComponent>();
    for (size_t i = 0; i < lightPool.size(); ++i) consider(lightPool.getEntity(i));
    auto& tfPool = reg.pool<TransformComponent>();
    for (size_t i = 0; i < tfPool.size(); ++i) consider(tfPool.getEntity(i));

    // Sync manual order vector to current entity set (Environment excluded).
    m_order.erase(std::remove_if(m_order.begin(), m_order.end(),
                  [&](Entity e) { return !seenAll.count(e); }), m_order.end());
    {
        std::unordered_set<Entity> inOrder(m_order.begin(), m_order.end());
        std::vector<Entity> fresh;
        for (Entity e : all)
            if (!reg.has<EnvironmentComponent>(e) && !inOrder.count(e)) fresh.push_back(e);
        std::sort(fresh.begin(), fresh.end());
        for (Entity e : fresh) m_order.push_back(e);
    }

    // parent → children, sorted by m_order index.
    std::unordered_map<Entity, std::vector<Entity>> children;
    std::vector<Entity> roots;
    for (Entity e : all) {
        Entity p = NULL_ENTITY;
        if (reg.has<TransformComponent>(e))
            p = reg.get<TransformComponent>(e).parent;
        if (p == NULL_ENTITY) roots.push_back(e); else children[p].push_back(e);
    }
    std::unordered_map<Entity, int> idx;
    idx.reserve(m_order.size());
    for (size_t i = 0; i < m_order.size(); ++i) idx[m_order[i]] = static_cast<int>(i);
    auto orderless = [&](Entity a, Entity b) {
        auto ia = idx.find(a), ib = idx.find(b);
        int va = ia == idx.end() ? INT_MAX : ia->second;
        int vb = ib == idx.end() ? INT_MAX : ib->second;
        return va < vb;
    };
    std::sort(roots.begin(), roots.end(), [&](Entity a, Entity b) {
        // Environment pinned to the top.
        bool aEnv = reg.has<EnvironmentComponent>(a);
        bool bEnv = reg.has<EnvironmentComponent>(b);
        if (aEnv != bEnv) return aEnv;
        return orderless(a, b);
    });
    for (auto& [p, kids] : children) std::sort(kids.begin(), kids.end(), orderless);

    // Drop expanded-set entries for entities that no longer exist.
    for (auto it = m_expandedGroups.begin(); it != m_expandedGroups.end(); ) {
        if (!seenAll.count(*it)) it = m_expandedGroups.erase(it); else ++it;
    }

    // Emit rows depth-first.
    std::function<void(Entity, int)> emit = [&](Entity e, int depth) {
        if (!seen.insert(e).second) return;
        Row row = makeRow(e, depth);
        auto itC = children.find(e);
        bool hasKids = (itC != children.end() && !itC->second.empty());
        row.hasChildren = hasKids;
        row.expanded    = hasKids && m_expandedGroups.count(e) > 0;
        m_rows.push_back(std::move(row));
        if (hasKids && m_expandedGroups.count(e) > 0)
            for (Entity k : itC->second) emit(k, depth + 1);
    };
    for (Entity r : roots) emit(r, 0);

    // Prune selection / anchor to entities that still exist (and are visible —
    // a collapsed-parent's children still exist in the registry, so keep them
    // selectable through the engine even if not shown).
    m_selected.erase(std::remove_if(m_selected.begin(), m_selected.end(),
                     [&](Entity e) { return !seenAll.count(e); }), m_selected.end());
    if (m_anchor != NULL_ENTITY && !seenAll.count(m_anchor)) m_anchor = NULL_ENTITY;
}

bool SceneHierarchy::isSelected(Entity e) const {
    return std::find(m_selected.begin(), m_selected.end(), e) != m_selected.end();
}
int SceneHierarchy::rowOf(Entity e) const {
    for (int i = 0; i < (int)m_rows.size(); ++i) if (m_rows[i].entity == e) return i;
    return -1;
}
int SceneHierarchy::rowAtY(double my) const {
    const float listTop = m_top + HEADER_H;
    if (my < listTop) return -1;
    int idx = (int)std::floor((my - listTop + m_scroll) / ROW_H);
    return (idx >= 0 && idx < (int)m_rows.size()) ? idx : -1;
}
void SceneHierarchy::selectSingle(Entity e) { m_selected.assign(1, e); m_anchor = e; }
void SceneHierarchy::toggle(Entity e) {
    auto it = std::find(m_selected.begin(), m_selected.end(), e);
    if (it != m_selected.end()) m_selected.erase(it);
    else                        m_selected.push_back(e);
    m_anchor = e;
}
void SceneHierarchy::selectRange(int rowA, int rowB) {
    m_selected.clear();
    int step = (rowA <= rowB) ? 1 : -1;                    // fill toward the clicked end
    for (int i = rowA; ; i += step) {
        if (i >= 0 && i < (int)m_rows.size()) m_selected.push_back(m_rows[i].entity);
        if (i == rowB) break;
    }
}
void SceneHierarchy::notifyPrimary() {
    Entity primary = m_selected.empty() ? NULL_ENTITY : m_selected.back();
    if (primary != m_lastPrimary) {
        m_lastPrimary = primary;
        if (m_onSelect) m_onSelect(primary);
    }
}

void SceneHierarchy::setSelection(const std::vector<Entity>& sel) {
    m_selected = sel;
    m_anchor   = sel.empty() ? NULL_ENTITY : sel.back();
    notifyPrimary();
}

void SceneHierarchy::update(Registry& registry, float x0, float width, float top, float height, bool cursorActive) {
    m_x0 = x0; m_w = width; m_top = top; m_h = height;

    if (!m_visible) { m_indexCount = 0; return; }

    buildRows(registry);

    double mx = -1.0, my = -1.0;
    if (cursorActive || m_leftDown) glfwGetCursorPos(m_window, &mx, &my);
    m_curX = mx; m_curY = my;

    // Drag past a threshold from the press point starts a marquee (if pressed
    // on empty area) or a reorder drag (if pressed on a row that isn't the
    // pinned Environment).
    if (m_leftDown) {
        bool down = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (!down) { m_leftDown = false; }   // safety; handleRelease normally clears it
        else {
            const bool moved = std::fabs(mx - m_pressX) > 4.0 || std::fabs(my - m_pressY) > 4.0;
            if (!m_marquee && !m_reorderDrag && moved) {
                if (m_pressRow >= 0
                    && m_pressRow < static_cast<int>(m_rows.size())
                    && m_rows[m_pressRow].kind != Kind::Environment) {
                    m_reorderDrag = true;
                } else {
                    m_marquee = true;
                }
            }
            if (m_marquee) {
                double y0 = std::min(m_pressY, my), y1 = std::max(m_pressY, my);
                std::vector<Entity> hit;
                const float listTop = top + HEADER_H;
                for (size_t i = 0; i < m_rows.size(); ++i) {
                    float ry = listTop + i * ROW_H - m_scroll;
                    if (ry + ROW_H > y0 && ry < y1 && ry + ROW_H > listTop) hit.push_back(m_rows[i].entity);
                }
                if (m_pressCtrl) {                          // union with the pre-drag selection
                    m_selected = m_preMarquee;
                    for (Entity e : hit) if (!isSelected(e)) m_selected.push_back(e);
                } else {
                    m_selected = hit;
                }
            }
            if (m_reorderDrag) {
                const float listTop = top + HEADER_H;
                int insert;
                int row = rowAtY(my);
                if (row < 0) {
                    insert = static_cast<int>(m_rows.size());
                } else {
                    float ry      = listTop + row * ROW_H - m_scroll;
                    float center  = ry + ROW_H * 0.5f;
                    insert = (my < center) ? row : row + 1;
                }
                // Environment is always pinned at index 0; clamp so the drop
                // can never land above it.
                if (!m_rows.empty() && m_rows[0].kind == Kind::Environment && insert < 1)
                    insert = 1;
                m_reorderInsertIdx = insert;
            }
        }
    }

    m_vertices.clear();
    m_indices.clear();
    m_overButton = false;   // re-tested below for hover over the collapse toggle / rail

    const glm::vec4 panelBg  = {0.065f, 0.070f, 0.085f, 0.97f};
    const glm::vec4 headerBg = {0.10f,  0.105f, 0.130f, 1.0f};
    const glm::vec4 border   = {0.22f,  0.24f,  0.30f,  1.0f};
    const glm::vec4 labelCol = {0.50f,  0.54f,  0.60f,  1.0f};
    const glm::vec4 rowTxt   = {0.82f,  0.85f,  0.90f,  1.0f};
    const glm::vec4 selBg    = {0.14f,  0.30f,  0.24f,  1.0f};   // selected, focused (mint)
    const glm::vec4 selDim   = {0.15f,  0.16f,  0.20f,  1.0f};   // selected, unfocused
    const glm::vec4 hoverBg  = {0.16f,  0.17f,  0.22f,  1.0f};
    const glm::vec4 meshIcon = {0.45f,  0.62f,  0.95f,  1.0f};
    const glm::vec4 lightCol = {1.00f,  0.85f,  0.35f,  1.0f};
    const glm::vec4 otherCol = {0.60f,  0.62f,  0.68f,  1.0f};
    const glm::vec4 marqFill = {0.42f,  1.00f,  0.66f,  0.14f};
    const glm::vec4 marqLine = {0.42f,  1.00f,  0.66f,  0.85f};

    const float fsz = PixelFont::SCALE;

    addQuad(x0, top, width, height, panelBg);

    // Collapsed rail mode — same shape the content browser uses when collapsed.
    // Whole panel becomes a thin clickable strip with a ◀ chevron at the top.
    if (m_collapsedRail) {
        bool railHover = cursorActive && mx >= x0 && mx < x0 + width
                      && my >= top    && my < top   + height;
        if (railHover) m_overButton = true;
        addQuad(x0, top, width, HEADER_H, railHover ? hoverBg : headerBg);
        const glm::vec4 toggleCol{0.78f, 0.80f, 0.86f, 1.0f};
        addArrow(x0 + width * 0.5f, top + HEADER_H * 0.5f, false, toggleCol);  // ◀ expand
        // Left divider + resize-edge highlight — drawn LAST so the mint strip
        // isn't covered by the rail hoverBg in the top HEADER_H rows.
        if (m_leftEdgeHighlight) {
            const glm::vec4 mint = {0.42f, 1.00f, 0.66f, 1.0f};
            addQuad(x0, top, 2.0f, height, mint);
        } else {
            addQuad(x0, top, 1.0f, height, border);
        }
        // Cache the whole panel rect as the toggle hit area.
        m_collapseBtnX = x0;
        m_collapseBtnY = top;
        m_collapseBtnW = width;
        m_collapseBtnH = height;
        if (m_vertices.size() <= VERT_CAP && m_indices.size() <= IDX_CAP) {
            if (!m_vertices.empty()) {
                m_vertexBuffer.uploadData(m_allocator, m_vertices.data(), m_vertices.size() * sizeof(UIVertex));
                m_indexBuffer .uploadData(m_allocator, m_indices .data(), m_indices .size() * sizeof(uint32_t));
                m_indexCount = static_cast<uint32_t>(m_indices.size());
            } else m_indexCount = 0;
        }
        return;
    }

    // Header — "HIERARCHY" + selection count.
    addQuad(x0, top, width, HEADER_H, headerBg);

    // ▶ collapse toggle on the LEFT side of the header — mirrors the content
    // browser's ◀ on its right side (each panel's toggle sits on the edge
    // facing the editor centre).
    constexpr float TOGGLE_W = 18.0f;
    m_collapseBtnX = x0;
    m_collapseBtnY = top;
    m_collapseBtnW = TOGGLE_W;
    m_collapseBtnH = HEADER_H;
    bool tHover = cursorActive
               && mx >= m_collapseBtnX && mx < m_collapseBtnX + m_collapseBtnW
               && my >= m_collapseBtnY && my < m_collapseBtnY + m_collapseBtnH;
    if (tHover) {
        m_overButton = true;
        addQuad(m_collapseBtnX, m_collapseBtnY, m_collapseBtnW, m_collapseBtnH, hoverBg);
    }
    const glm::vec4 toggleCol{0.78f, 0.80f, 0.86f, 1.0f};
    addArrow(m_collapseBtnX + TOGGLE_W * 0.5f, m_collapseBtnY + HEADER_H * 0.5f,
             true, toggleCol);

    std::string hdr = "HIERARCHY";
    if (m_selected.size() > 1) hdr += "  " + std::to_string(m_selected.size()) + " SEL";
    addText(x0 + TOGGLE_W + 4.0f, top + std::floor((HEADER_H - PixelFont::CELL_H) * 0.5f),
            hdr, fsz, m_focused ? rowTxt : labelCol, x0 + width - 4.0f);

    // Left divider / resize-edge highlight — drawn AFTER the header band AND
    // the collapse-button hover quad so the mint strip stays uninterrupted
    // across the full panel height.
    if (m_leftEdgeHighlight) {
        const glm::vec4 mint = {0.42f, 1.00f, 0.66f, 1.0f};   // matches the other resize edges
        addQuad(x0, top, 2.0f, height, mint);
    } else {
        addQuad(x0, top, 1.0f, height, border);               // normal divider
    }

    // Rows.
    const float listTop = top + HEADER_H;
    float contentH  = m_rows.size() * ROW_H;
    float listH     = height - HEADER_H;
    float maxScroll = std::max(0.0f, contentH - listH);
    if (m_scroll > maxScroll) m_scroll = maxScroll;
    if (m_scroll < 0.0f)      m_scroll = 0.0f;

    const float maxX = x0 + width - 4.0f;
    constexpr float INDENT_PX  = 12.0f;   // each depth level shifts the row by this much
    constexpr float CHEVRON_W  = 10.0f;
    m_chevronHits.clear();
    for (size_t i = 0; i < m_rows.size(); ++i) {
        float ry = listTop + i * ROW_H - m_scroll;
        if (ry + ROW_H <= listTop || ry >= top + height) continue;

        bool hover = cursorActive && !m_marquee && mx >= x0 && mx < x0 + width
                     && my >= ry && my < ry + ROW_H && my >= listTop;
        if (isSelected(m_rows[i].entity)) addQuad(x0 + 1.0f, ry, width - 1.0f, ROW_H, m_focused ? selBg : selDim);
        else if (hover)                   addQuad(x0 + 1.0f, ry, width - 1.0f, ROW_H, hoverBg);

        float indent = m_rows[i].depth * INDENT_PX;
        if (m_rows[i].hasChildren) {
            // Chevron at the front: ▶ collapsed, ▼ expanded. Reuses addArrow
            // (mint when hovered for affordance, otherwise the row's text colour).
            float cx = x0 + 6.0f + indent;
            float cy = ry + ROW_H * 0.5f;
            bool chevHover = cursorActive && mx >= cx - 4.0f && mx < cx + CHEVRON_W - 2.0f
                          && my >= ry && my < ry + ROW_H && my >= listTop;
            // Always render: rotate ▶ → ▼ by drawing a downward triangle when expanded.
            // We don't have a "down" arrow primitive, so cheat by drawing a small
            // ▼ as a 3-pixel triangle quad. Keep it cheap and obviously different
            // from the closed state.
            glm::vec4 chColor = chevHover ? glm::vec4{0.42f, 1.0f, 0.66f, 1.0f} : rowTxt;
            if (m_rows[i].expanded) {
                addQuad(cx, cy - 1.0f, 6.0f, 2.0f, chColor);
                addQuad(cx + 1.0f, cy + 1.0f, 4.0f, 1.0f, chColor);
                addQuad(cx + 2.0f, cy + 2.0f, 2.0f, 1.0f, chColor);
            } else {
                addArrow(cx + 3.0f, cy, /*right=*/true, chColor);
            }
            m_chevronHits.push_back({m_rows[i].entity, cx - 4.0f, ry, CHEVRON_W + 2.0f, ROW_H});
        }

        const glm::vec4 envIcon{0.42f, 1.00f, 0.66f, 1.0f};   // mint, matches editor accent
        glm::vec4 ic = (m_rows[i].kind == Kind::Environment) ? envIcon
                     : (m_rows[i].kind == Kind::Mesh)        ? meshIcon
                     : (m_rows[i].kind == Kind::Light)       ? lightCol : otherCol;
        addBall(x0 + 10.0f + indent + CHEVRON_W, ry + ROW_H * 0.5f, 3.5f, ic);
        if (m_rows[i].entity == m_renamingEntity) {
            // Inline edit: render the buffer with a 1px cursor at the end.
            float tx = x0 + 18.0f + indent + CHEVRON_W;
            float ty = ry + std::floor((ROW_H - PixelFont::CELL_H) * 0.5f);
            const glm::vec4 editCol{1.0f, 1.0f, 1.0f, 1.0f};
            float endX = addText(tx, ty, m_renameBuffer, fsz, editCol, maxX);
            addQuad(endX + 1.0f, ty, 1.0f, PixelFont::CELL_H, editCol);
        } else {
            addText(x0 + 18.0f + indent + CHEVRON_W,
                    ry + std::floor((ROW_H - PixelFont::CELL_H) * 0.5f),
                    m_rows[i].label, fsz, rowTxt, maxX);
        }
    }

    // Drop indicator for the reorder drag — mint 2 px line between rows.
    if (m_reorderDrag && m_reorderInsertIdx >= 0) {
        float lineY = listTop + m_reorderInsertIdx * ROW_H - m_scroll - 1.0f;
        addQuad(x0 + 2.0f, lineY, width - 4.0f, 2.0f, marqLine);
    }

    // Marquee box.
    if (m_marquee && mx >= 0.0) {
        float bx0 = std::clamp((float)std::min(m_pressX, mx), x0, x0 + width);
        float bx1 = std::clamp((float)std::max(m_pressX, mx), x0, x0 + width);
        float by0 = std::clamp((float)std::min(m_pressY, my), listTop, top + height);
        float by1 = std::clamp((float)std::max(m_pressY, my), listTop, top + height);
        addQuad(bx0, by0, bx1 - bx0, by1 - by0, marqFill);
        addQuad(bx0, by0, bx1 - bx0, 1.0f, marqLine);
        addQuad(bx0, by1 - 1.0f, bx1 - bx0, 1.0f, marqLine);
        addQuad(bx0, by0, 1.0f, by1 - by0, marqLine);
        addQuad(bx1 - 1.0f, by0, 1.0f, by1 - by0, marqLine);
    }

    // Context menu — drawn last so it sits on top.
    if (m_menuOpen) {
        const glm::vec4 menuBg     = {0.11f, 0.12f, 0.15f, 1.0f};
        const glm::vec4 menuBorder = {0.30f, 0.55f, 0.42f, 1.0f};
        const glm::vec4 menuHov    = {0.20f, 0.34f, 0.27f, 1.0f};
        const glm::vec4 menuTxt    = {0.88f, 0.90f, 0.94f, 1.0f};
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

    notifyPrimary();

    if (m_vertices.size() > VERT_CAP || m_indices.size() > IDX_CAP) return;
    if (!m_vertices.empty()) {
        m_vertexBuffer.uploadData(m_allocator, m_vertices.data(), m_vertices.size() * sizeof(UIVertex));
        m_indexBuffer.uploadData(m_allocator, m_indices.data(), m_indices.size() * sizeof(uint32_t));
        m_indexCount = (uint32_t)m_indices.size();
    } else {
        m_indexCount = 0;
    }
}

void SceneHierarchy::draw(VkCommandBuffer cmd) {
    if (!m_visible || m_indexCount == 0) return;
    VkBuffer buffers[] = { m_vertexBuffer.getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

bool SceneHierarchy::handleMouseButton(int button, int action) {
    if (!m_visible || button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return false;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);

    // An open context menu eats the next click: run the item, or dismiss off it.
    if (m_menuOpen) {
        float mh = m_menuItems.size() * MENU_ROW_H;
        if (mx >= m_menuX && mx < m_menuX + m_menuW && my >= m_menuY && my < m_menuY + mh) {
            int idx = (int)std::floor((my - m_menuY) / MENU_ROW_H);
            if (idx >= 0 && idx < (int)m_menuItems.size()) {
                Command c = m_menuItems[idx].cmd;
                closeMenu();
                if (m_onCommand) m_onCommand(c);
            }
        } else {
            closeMenu();
        }
        return true;
    }

    if (mx < m_x0 || mx >= m_x0 + m_w || my < m_top || my >= m_top + m_h) return false;
    // Collapse toggle has priority over the header-band consume so the click
    // actually fires the callback before the header swallows it.
    if (mx >= m_collapseBtnX && mx < m_collapseBtnX + m_collapseBtnW
     && my >= m_collapseBtnY && my < m_collapseBtnY + m_collapseBtnH) {
        if (m_onToggleCollapse) m_onToggleCollapse();
        return true;
    }
    if (my < m_top + HEADER_H) return true;   // header — consume, no selection

    // Chevron click: flip expanded state for that entity, don't change selection.
    for (const ChevronHit& h : m_chevronHits) {
        if (mx >= h.x && mx < h.x + h.w && my >= h.y && my < h.y + h.h) {
            if (m_expandedGroups.count(h.entity)) m_expandedGroups.erase(h.entity);
            else                                  m_expandedGroups.insert(h.entity);
            return true;
        }
    }

    m_leftDown    = true;
    m_marquee     = false;
    m_reorderDrag = false;
    m_reorderInsertIdx = -1;
    m_pressX      = mx; m_pressY = my;
    m_pressRow    = rowAtY(my);
    m_pressShift = glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT)   == GLFW_PRESS ||
                   glfwGetKey(m_window, GLFW_KEY_RIGHT_SHIFT)  == GLFW_PRESS;
    m_pressCtrl  = glfwGetKey(m_window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                   glfwGetKey(m_window, GLFW_KEY_RIGHT_CONTROL)== GLFW_PRESS;
    m_preMarquee = m_selected;

    // Double-click: same row pressed twice in quick succession → activate
    // (engine frames the camera on the entity). Single-click still selects
    // via the release path; activation is additive, not exclusive.
    const double now = glfwGetTime();
    if (m_pressRow >= 0 && m_pressRow == m_lastClickRow
        && (now - m_lastClickTime) < DOUBLE_CLICK_S) {
        if (m_onActivate) m_onActivate(m_rows[m_pressRow].entity);
        m_lastClickTime = 0.0;     // consume — no triple-click activation
        m_lastClickRow  = -1;
    } else {
        m_lastClickRow  = m_pressRow;
        m_lastClickTime = now;
    }
    return true;
}

void SceneHierarchy::insertAfterSources(const std::vector<Entity>& sources,
                                         const std::vector<Entity>& fresh) {
    if (fresh.empty()) return;
    // Pick the lowest source-row position so when several rows are duplicated
    // together the whole new block lands right under the bottom one.
    int landAfter = -1;
    for (Entity s : sources) {
        auto it = std::find(m_order.begin(), m_order.end(), s);
        if (it != m_order.end()) {
            int idx = static_cast<int>(it - m_order.begin());
            if (idx > landAfter) landAfter = idx;
        }
    }
    // Drop any of the new entities that are already in m_order (paranoia).
    std::vector<Entity> toInsert;
    toInsert.reserve(fresh.size());
    for (Entity e : fresh)
        if (std::find(m_order.begin(), m_order.end(), e) == m_order.end())
            toInsert.push_back(e);
    if (landAfter < 0)
        m_order.insert(m_order.end(), toInsert.begin(), toInsert.end());
    else
        m_order.insert(m_order.begin() + landAfter + 1,
                       toInsert.begin(), toInsert.end());
}

void SceneHierarchy::handleRelease() {
    if (!m_leftDown) return;
    if (m_reorderDrag) {
        // Move the dragged entity in m_order to land at the insert index.
        if (m_pressRow >= 0 && m_pressRow < static_cast<int>(m_rows.size())) {
            Entity dragged = m_rows[m_pressRow].entity;
            // Find the entity that currently sits AT the insert index — once we
            // remove the dragged entry from m_order, "insert before that entity"
            // gives us the right landing slot regardless of which way we moved.
            Entity anchorEnt = NULL_ENTITY;
            for (int i = m_reorderInsertIdx; i < static_cast<int>(m_rows.size()); ++i) {
                if (m_rows[i].entity != dragged && m_rows[i].kind != Kind::Environment) {
                    anchorEnt = m_rows[i].entity;
                    break;
                }
            }
            m_order.erase(std::remove(m_order.begin(), m_order.end(), dragged), m_order.end());
            if (anchorEnt == NULL_ENTITY) {
                m_order.push_back(dragged);
            } else {
                auto it = std::find(m_order.begin(), m_order.end(), anchorEnt);
                m_order.insert(it, dragged);
            }
        }
    } else if (!m_marquee) {
        // Plain click: apply selection rules.
        if (m_pressRow < 0) {
            if (!m_pressShift && !m_pressCtrl) { m_selected.clear(); m_anchor = NULL_ENTITY; }
        } else {
            Entity e = m_rows[m_pressRow].entity;
            int anchorRow = rowOf(m_anchor);
            if (m_pressShift && anchorRow >= 0) selectRange(anchorRow, m_pressRow);
            else if (m_pressCtrl)               toggle(e);
            else                                selectSingle(e);
        }
    }
    m_leftDown    = false;
    m_marquee     = false;
    m_reorderDrag = false;
    m_reorderInsertIdx = -1;
    notifyPrimary();
}

bool SceneHierarchy::handleRightPress() {
    if (!m_visible) return false;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    if (mx < m_x0 || mx >= m_x0 + m_w || my < m_top || my >= m_top + m_h) { closeMenu(); return false; }

    // Right-clicking an unselected row selects just it; right-clicking inside the
    // current (multi-)selection keeps it, so the menu acts on everything selected.
    int row = rowAtY(my);
    if (row >= 0) {
        Entity e = m_rows[row].entity;
        if (!isSelected(e)) { selectSingle(e); notifyPrimary(); }
    }
    openMenu(mx, my);
    return true;
}

bool SceneHierarchy::startRename(Entity e, const std::string& initial) {
    if (e == NULL_ENTITY) return false;
    m_renamingEntity = e;
    m_renameBuffer   = initial;
    m_focused        = true;     // suppress camera + flag this panel as keyboard owner
    return true;
}

void SceneHierarchy::commitRename() {
    if (m_renamingEntity == NULL_ENTITY) return;
    if (m_onRenameCommit) m_onRenameCommit(m_renamingEntity, m_renameBuffer);
    m_renamingEntity = NULL_ENTITY;
    m_renameBuffer.clear();
}

void SceneHierarchy::cancelRename() {
    m_renamingEntity = NULL_ENTITY;
    m_renameBuffer.clear();
}

void SceneHierarchy::handleChar(unsigned int codepoint) {
    if (m_renamingEntity == NULL_ENTITY) return;
    // Ignore control chars (Backspace/Enter come through handleKey).
    if (codepoint < 0x20 || codepoint == 0x7F) return;
    // Sub-set to printable ASCII for the pixel font's character set.
    if (codepoint < 0x80) m_renameBuffer.push_back(static_cast<char>(codepoint));
}

void SceneHierarchy::openMenu(double mx, double my) {
    m_menuItems.clear();

    // Create — always available, with or without a selection (right-clicking
    // empty space is the classic "make a new object here" gesture).
    m_menuItems.push_back({"Create Cube",              Command::CreateCube});
    m_menuItems.push_back({"Create Point Light",       Command::CreatePointLight});
    m_menuItems.push_back({"Create Directional Light", Command::CreateDirLight});

    bool has = !m_selected.empty();
    if (has) {
        // Single-selection-only: Rename targets the primary entity.
        if (m_selected.size() == 1) m_menuItems.push_back({"Rename", Command::Rename});
        m_menuItems.push_back({"Copy",      Command::Copy});
        m_menuItems.push_back({"Cut",       Command::Cut});
        m_menuItems.push_back({"Duplicate", Command::Duplicate});
        m_menuItems.push_back({"Group",     Command::Group});
        // "Ungroup" only when the selection contains at least one group — i.e.
        // an empty-parent row (the build_rows tree labels these "Group N" and
        // sets row.hasChildren). Avoid offering Ungroup on a meshed entity that
        // just happens to have children — only the engine knows for sure
        // because m_rows isn't always in sync with the latest registry, so the
        // engine's ungroupSelected() filters again before acting.
        bool anyGroupSelected = false;
        for (const Row& r : m_rows) {
            if (r.kind == Kind::Other && r.hasChildren && isSelected(r.entity)) {
                anyGroupSelected = true; break;
            }
        }
        if (anyGroupSelected) m_menuItems.push_back({"Ungroup", Command::Ungroup});
    }
    m_menuItems.push_back({"Paste", Command::Paste});
    if (has) m_menuItems.push_back({"Delete", Command::Delete});

    size_t longest = 0;
    for (const auto& it : m_menuItems) longest = std::max(longest, it.label.size());
    m_menuW = longest * PixelFont::ADVANCE * PixelFont::SCALE + 18.0f;
    float mh = m_menuItems.size() * MENU_ROW_H;

    float winRight = m_x0 + m_w;   // the dock hugs the window's right edge
    m_menuX = std::min((float)mx, winRight - m_menuW);
    m_menuY = std::min((float)my, (m_top + m_h) - mh);
    if (m_menuX < m_x0)            m_menuX = m_x0;
    if (m_menuY < m_top + HEADER_H) m_menuY = m_top + HEADER_H;
    m_menuOpen = true;
}

void SceneHierarchy::openContextMenuAt(double sx, double sy) {
    openMenu(sx, sy);   // builds the items, sets m_menuOpen, computes m_menuW
    // openMenu clamped to the panel; re-clamp to the whole window so the menu can
    // sit over the 3D viewport (the central area, left of this dock).
    int ww = 0, wh = 0;
    glfwGetWindowSize(m_window, &ww, &wh);
    float mh = m_menuItems.size() * MENU_ROW_H;
    m_menuX = std::min((float)sx, (float)ww - m_menuW);  if (m_menuX < 0.0f) m_menuX = 0.0f;
    m_menuY = std::min((float)sy, (float)wh - mh);       if (m_menuY < 0.0f) m_menuY = 0.0f;
}

bool SceneHierarchy::handleScroll(double yoffset) {
    if (!m_visible) return false;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    if (mx < m_x0 || mx >= m_x0 + m_w || my < m_top || my >= m_top + m_h) return false;
    m_scroll -= (float)yoffset * 28.0f;
    if (m_scroll < 0.0f) m_scroll = 0.0f;
    return true;
}

bool SceneHierarchy::handleKey(int key, int action, int mods) {
    if (!m_focused || action != GLFW_PRESS) return false;
    if (key == GLFW_KEY_ESCAPE && m_menuOpen) { closeMenu(); return true; }
    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    // Rename mode owns the keyboard while active — Enter commits, Esc cancels,
    // Backspace erases. Everything else (printables) routes through handleChar.
    if (m_renamingEntity != NULL_ENTITY) {
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) { commitRename(); return true; }
        if (key == GLFW_KEY_ESCAPE) { cancelRename(); return true; }
        if (key == GLFW_KEY_BACKSPACE) {
            if (!m_renameBuffer.empty()) m_renameBuffer.pop_back();
            return true;
        }
        return true;   // swallow everything else while renaming
    }

    // F2 starts rename on the primary selection — Maya/Unity standard.
    if (key == GLFW_KEY_F2 && !m_selected.empty()) {
        if (m_onCommand) m_onCommand(Command::Rename);
        return true;
    }

    if (key == GLFW_KEY_DELETE || key == GLFW_KEY_BACKSPACE) {
        if (!m_selected.empty() && m_onCommand) m_onCommand(Command::Delete);
        return true;
    }
    if (ctrl && key == GLFW_KEY_C) { if (m_onCommand) m_onCommand(Command::Copy);  return true; }
    if (ctrl && key == GLFW_KEY_X) { if (m_onCommand) m_onCommand(Command::Cut);   return true; }
    if (ctrl && key == GLFW_KEY_V) { if (m_onCommand) m_onCommand(Command::Paste); return true; }
    if (ctrl && key == GLFW_KEY_A) {                                   // select all
        m_selected.clear();
        for (const Row& r : m_rows) m_selected.push_back(r.entity);
        m_anchor = m_selected.empty() ? NULL_ENTITY : m_selected.back();
        notifyPrimary();
        return true;
    }
    return false;
}

// ── geometry helpers ─────────────────────────────────────────────────────────

void SceneHierarchy::addQuad(float x, float y, float w, float h, const glm::vec4& color) {
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

void SceneHierarchy::addBall(float cx, float cy, float radius, const glm::vec4& color) {
    const float hw = radius + 1.5f;
    const glm::vec2 off[4] = {{-hw,-hw},{hw,-hw},{hw,hw},{-hw,hw}};
    const glm::vec4 data0{radius, 0.0f, 0.0f, 0.0f};
    const glm::vec4 data1{4.0f,   0.0f, 0.0f, 0.0f};   // shape 4 = shaded sphere
    uint32_t base = (uint32_t)m_vertices.size();
    for (const glm::vec2& o : off) m_vertices.push_back({{cx + o.x, cy + o.y}, color, o, data0, data1});
    m_indices.push_back(base);     m_indices.push_back(base + 1); m_indices.push_back(base + 2);
    m_indices.push_back(base + 2); m_indices.push_back(base + 3); m_indices.push_back(base);
}

void SceneHierarchy::addArrow(float cx, float cy, bool right, const glm::vec4& color) {
    // Same chevron the content browser draws — 7 stacked rows of increasing
    // then decreasing length, anchored toward the side the arrow points to.
    for (int r = 0; r < 7; ++r) {
        float len = 4.0f - std::fabs(static_cast<float>(r - 3));
        if (len <= 0.0f) continue;
        float x = right ? (cx - 2.0f) : (cx + 2.0f - len);
        addQuad(x, cy - 3.0f + r, len, 1.0f, color);
    }
}

void SceneHierarchy::addGlyph(float x, float y, char c, float s, const glm::vec4& color) {
    const uint8_t* rows = PixelFont::glyphRows(c);
    if (!rows) return;
    for (int r = 0; r < PixelFont::CELL_H; ++r) {
        uint8_t bits = rows[r];
        for (int col = 0; col < PixelFont::CELL_W; ++col)
            if (bits & (1 << (PixelFont::CELL_W - 1 - col)))
                addQuad(x + col * s, y + r * s, s, s, color);
    }
}

float SceneHierarchy::addText(float x, float y, const std::string& text, float s, const glm::vec4& color, float maxX) {
    float cx = x;
    for (char c : text) {
        if (cx + PixelFont::CELL_W * s > maxX) break;
        addGlyph(cx, y, c, s, color);
        cx += PixelFont::ADVANCE * s;
    }
    return cx - x;
}

} // namespace Nyx
