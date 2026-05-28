#include "ui/Inspector.h"
#include "ui/PixelFont.h"
#include "ecs/Registry.h"
#include "ecs/components/MeshComponent.h"
#include "ecs/components/MaterialComponent.h"
#include "ecs/components/LightComponent.h"
#include "ecs/components/TransformComponent.h"
#include "renderer/Mesh.h"

#include <cstdio>
#include <cmath>

namespace Nyx {

namespace {
std::string fnum(float v, int dec = 2) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
    return std::string(buf);
}
std::string entityLabel(Registry& reg, Entity e) {
    if (reg.has<LightComponent>(e))
        return reg.get<LightComponent>(e).type == LightComponent::Type::Directional
                   ? "Directional Light" : "Point Light";
    if (reg.has<MeshComponent>(e)) return "Mesh " + std::to_string(e);
    return "Entity " + std::to_string(e);
}
} // namespace

void Inspector::init(VmaAllocator allocator, GLFWwindow* window) {
    m_allocator = allocator;
    m_window    = window;
    constexpr VkDeviceSize vertexBufSize = sizeof(UIVertex) * VERT_CAP;
    constexpr VkDeviceSize indexBufSize  = sizeof(uint32_t) * IDX_CAP;
    m_vertexBuffer.init(allocator, vertexBufSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_indexBuffer.init(allocator, indexBufSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_buffersInitialized = true;
}

void Inspector::cleanup(VmaAllocator allocator) {
    if (m_buffersInitialized) {
        m_vertexBuffer.cleanup(allocator);
        m_indexBuffer.cleanup(allocator);
        m_buffersInitialized = false;
    }
}

void Inspector::update(Registry& reg, Entity selected, bool dragActive,
                       float x0, float width, float top, float height, bool cursorActive,
                       bool sceneHasAnim, bool animPlaying, bool selectedAnimated) {
    m_x0 = x0; m_w = width; m_top = top; m_h = height;

    if (!m_visible) { m_indexCount = 0; m_hasSlot = false; m_hasAnimBtn = false; return; }

    m_vertices.clear();
    m_indices.clear();
    m_hasSlot = false;
    m_hasFields = false;
    m_hasDelete = false;
    m_hasAnimBtn = false;
    m_overButton = false;   // re-tested below as each clickable rect is laid out

    const glm::vec4 panelBg    = {0.065f, 0.070f, 0.085f, 0.97f};
    const glm::vec4 headerBg   = {0.10f,  0.105f, 0.130f, 1.0f};
    const glm::vec4 border     = {0.22f,  0.24f,  0.30f,  1.0f};
    const glm::vec4 labelCol   = {0.50f,  0.54f,  0.60f,  1.0f};
    const glm::vec4 titleCol   = {0.42f,  1.00f,  0.66f,  1.0f};   // selected entity (mint)
    const glm::vec4 sectionCol = {0.45f,  0.66f,  0.86f,  1.0f};   // sub-headers (blue)
    const glm::vec4 keyCol     = {0.62f,  0.65f,  0.72f,  1.0f};
    const glm::vec4 slotBg     = {0.085f, 0.095f, 0.120f, 1.0f};
    const glm::vec4 slotBorder = {0.30f,  0.34f,  0.42f,  1.0f};
    const glm::vec4 slotHiBg   = {0.14f,  0.30f,  0.24f,  1.0f};
    const glm::vec4 mint       = {0.42f,  1.00f,  0.66f,  1.0f};
    const glm::vec4 slotTxt    = {0.66f,  0.70f,  0.78f,  1.0f};
    const glm::vec4 fieldBg    = {0.090f, 0.100f, 0.130f, 1.0f};   // number field
    const glm::vec4 fieldActive= {0.14f,  0.30f,  0.24f,  1.0f};   // field being scrubbed
    const glm::vec4 valCol     = {0.86f,  0.88f,  0.93f,  1.0f};
    const glm::vec4 delBg      = {0.26f,  0.10f,  0.11f,  1.0f};   // delete button
    const glm::vec4 delHi      = {0.40f,  0.14f,  0.15f,  1.0f};
    const glm::vec4 delBorder  = {0.58f,  0.26f,  0.26f,  1.0f};
    const glm::vec4 delTxt     = {0.96f,  0.74f,  0.72f,  1.0f};
    const glm::vec4 animBg     = {0.10f,  0.18f,  0.15f,  1.0f};   // play/pause button
    const glm::vec4 animHi     = {0.16f,  0.30f,  0.22f,  1.0f};

    const float fsz = PixelFont::SCALE;
    const float LH  = 12.0f;
    const float px  = x0 + 8.0f;
    const float maxX = x0 + width - 4.0f;

    double mx = -1.0, my = -1.0;
    if (cursorActive) glfwGetCursorPos(m_window, &mx, &my);

    addQuad(x0, top, width, height, panelBg);
    addQuad(x0, top, width, HEADER_H, headerBg);
    // Left divider / resize-edge highlight — drawn AFTER the header band so it
    // sits on top instead of being covered by it across the top HEADER_H rows.
    if (m_leftEdgeHighlight) {
        const glm::vec4 mint = {0.42f, 1.00f, 0.66f, 1.0f};
        addQuad(x0, top, 2.0f, height, mint);
    } else {
        addQuad(x0, top, 1.0f, height, border);
    }
    addText(px - 2.0f, top + std::floor((HEADER_H - PixelFont::CELL_H) * 0.5f),
            "INSPECTOR", fsz, labelCol, maxX);

    float y = top + HEADER_H + 6.0f;

    // Global animation Play/Pause (shown whenever the scene has clips, selected or not).
    auto drawAnim = [&]() {
        y += 5.0f;
        addText(px, y, "ANIMATION", fsz, sectionCol, maxX);
        y += LH + 2.0f;
        float bw = width - 16.0f, bh = 16.0f;
        m_animBtnRect = {px, y, bw, bh};
        m_hasAnimBtn  = true;
        bool hov = cursorActive && mx >= px && mx < px + bw && my >= y && my < y + bh;
        if (hov) m_overButton = true;
        addQuad(px, y, bw, bh, hov ? animHi : animBg);
        addOutline(px, y, bw, bh, 1.0f, slotBorder);
        addText(px + 8.0f, y + std::floor((bh - PixelFont::CELL_H) * 0.5f),
                animPlaying ? "|| Pause animation" : ">  Play animation", fsz, valCol, px + bw - 4.0f);
        y += bh + 4.0f;
    };

    if (selected == NULL_ENTITY) {
        addText(px, y, "No entity selected.", fsz, keyCol, maxX); y += LH;
        if (sceneHasAnim) drawAnim();
        if (!m_vertices.empty()) {
            m_vertexBuffer.uploadData(m_allocator, m_vertices.data(), m_vertices.size() * sizeof(UIVertex));
            m_indexBuffer.uploadData(m_allocator, m_indices.data(), m_indices.size() * sizeof(uint32_t));
            m_indexCount = (uint32_t)m_indices.size();
        } else m_indexCount = 0;
        return;
    }

    // Apply an in-progress transform drag-scrub (begun in handleMouseButton).
    if (m_scrubField >= 0) {
        bool down = cursorActive && glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (!down || !reg.has<TransformComponent>(selected)) {
            m_scrubField = -1;
            if (m_scrubDirty) { if (m_onEndEdit) m_onEndEdit(); m_scrubDirty = false; }
        } else {
            float d = (float)(mx - m_scrubLastX);
            m_scrubLastX = mx;
            if (d != 0.0f) {
                if (!m_scrubDirty) { if (m_onBeginEdit) m_onBeginEdit(); m_scrubDirty = true; }  // undo snapshot pre-edit
                auto& t = reg.get<TransformComponent>(selected);

                // Local-space pivot: the mesh's bbox centre (0 for non-mesh entities).
                // Rotation/scale rotate the entity around its origin (T*R*S), so we
                // compensate `position` to keep the world-space pivot fixed — the
                // visual "rotates in place" without changing the local matrix shape
                // (which would shift every parent-child relationship in the scene).
                glm::vec3 c(0.0f);
                if (reg.has<MeshComponent>(selected)) {
                    const Mesh* m = reg.get<MeshComponent>(selected).mesh;
                    if (m) c = (m->boundsMin() + m->boundsMax()) * 0.5f;
                }

                if (m_scrubField < 3) {
                    t.position[m_scrubField] += d * 0.02f;        // units/px
                } else if (m_scrubField < 6) {
                    glm::quat rOld = t.rotation;
                    glm::vec3 sc   = t.scale;
                    m_euler[m_scrubField - 3] += d * 0.5f;        // degrees/px
                    t.rotation = glm::quat(glm::radians(m_euler));
                    // pos1 = pos0 + Rold*(S*c) - Rnew*(S*c)
                    glm::vec3 scaled = sc * c;
                    t.position += rOld * scaled - t.rotation * scaled;
                } else {
                    int a = m_scrubField - 6;
                    glm::vec3 sOld = t.scale;
                    t.scale[a] = std::max(0.01f, t.scale[a] + d * 0.01f);
                    // pos1 = pos0 + R*(Sold*c - Snew*c)
                    t.position += t.rotation * ((sOld - t.scale) * c);
                }
                if (m_onEdit) m_onEdit();   // mark scene dirty so auto-save catches the new value
            }
        }
    }

    auto line    = [&](const std::string& s, const glm::vec4& c) { addText(px, y, s, fsz, c, maxX); y += LH; };
    auto section = [&](const std::string& s) { y += 5.0f; addText(px, y, s, fsz, sectionCol, maxX); y += LH + 2.0f; };

    line(entityLabel(reg, selected) + (selectedAnimated ? "  (animated)" : ""), titleCol);

    if (reg.has<TransformComponent>(selected)) {
        const auto& tc = reg.get<TransformComponent>(selected);
        // Refresh the Euler cache from the quaternion only when the selection changes,
        // so scrubbing keeps the user's typed-in degrees instead of jumping on round-trip.
        if (m_eulerEntity != selected) {
            m_eulerEntity = selected;
            m_euler = glm::degrees(glm::eulerAngles(tc.rotation));
        }
        section("TRANSFORM");
        const float fLabelW = 26.0f;
        const float fx0     = px + fLabelW;
        const float availW  = (x0 + width - 6.0f) - fx0;
        const float fw      = std::floor((availW - 4.0f) / 3.0f);   // 3 fields, 2px gaps
        const float fh      = 11.0f;
        auto fieldRow = [&](const char* lbl, const glm::vec3& v, int baseIdx) {
            addText(px, y + 2.0f, lbl, fsz, keyCol, maxX);
            for (int a = 0; a < 3; ++a) {
                float fx  = fx0 + a * (fw + 2.0f);
                int   idx = baseIdx + a;
                m_fieldRect[idx] = {fx, y, fw, fh};
                if (cursorActive && mx >= fx && mx < fx + fw && my >= y && my < y + fh)
                    m_overButton = true;   // scrubbable transform field — pointer cursor
                addQuad(fx, y, fw, fh, (m_scrubField == idx) ? fieldActive : fieldBg);
                addOutline(fx, y, fw, fh, 1.0f, slotBorder);
                addText(fx + 3.0f, y + 2.0f, fnum(v[a]), fsz, valCol, fx + fw - 2.0f);
            }
            y += fh + 3.0f;
        };
        fieldRow("Pos", tc.position, 0);
        fieldRow("Rot", m_euler,     3);
        fieldRow("Scl", tc.scale,    6);
        m_hasFields = true;
    }

    if (reg.has<MaterialComponent>(selected)) {
        const auto& mat = reg.get<MaterialComponent>(selected);
        section("MATERIAL");

        glm::vec4 sw = {mat.baseColorFactor.r, mat.baseColorFactor.g, mat.baseColorFactor.b, 1.0f};
        addQuad(px, y, 10.0f, 10.0f, sw);
        addOutline(px, y, 10.0f, 10.0f, 1.0f, slotBorder);
        addText(px + 16.0f, y + 1.0f, "Base Color", fsz, keyCol, maxX);
        y += LH + 3.0f;

        line("Metallic   " + fnum(mat.metallic),  keyCol);
        line("Roughness  " + fnum(mat.roughness), keyCol);
        line("Albedo: " + (mat.albedoName.empty() ? std::string("(default)") : mat.albedoName), keyCol);
        y += 4.0f;

        // Droppable / clickable material slot.
        float slotW = width - 16.0f;
        float slotH = 30.0f;
        m_slotX = px; m_slotY = y; m_slotW = slotW; m_slotH = slotH; m_hasSlot = true;

        bool over = cursorActive && mx >= m_slotX && mx < m_slotX + slotW && my >= m_slotY && my < m_slotY + slotH;
        if (over) m_overButton = true;
        glm::vec4 bg  = (dragActive && over) ? slotHiBg : (over ? glm::vec4{0.11f, 0.13f, 0.16f, 1.0f} : slotBg);
        glm::vec4 brd = (dragActive && over) ? mint     : slotBorder;
        addQuad(m_slotX, m_slotY, slotW, slotH, bg);
        addOutline(m_slotX, m_slotY, slotW, slotH, 1.0f, brd);
        addText(m_slotX + 6.0f, m_slotY + 4.0f,                 "Drop .png / .mat here",  fsz, slotTxt, m_slotX + slotW - 4.0f);
        addText(m_slotX + 6.0f, m_slotY + 4.0f + PixelFont::CELL_H + 3.0f, "(or click to assign selected)", fsz, slotTxt, m_slotX + slotW - 4.0f);
        y += slotH + 4.0f;
    }

    if (reg.has<LightComponent>(selected)) {
        const auto& lc = reg.get<LightComponent>(selected);
        section("LIGHT");
        line(std::string("Type: ") + (lc.type == LightComponent::Type::Directional ? "Directional" : "Point"), keyCol);
        glm::vec4 cs = {lc.color.r, lc.color.g, lc.color.b, 1.0f};
        addQuad(px, y, 10.0f, 10.0f, cs);
        addOutline(px, y, 10.0f, 10.0f, 1.0f, slotBorder);
        addText(px + 16.0f, y + 1.0f, "Color", fsz, keyCol, maxX);
        y += LH + 3.0f;
        line("Intensity " + fnum(lc.intensity), keyCol);
        if (lc.type == LightComponent::Type::Point) line("Radius    " + fnum(lc.radius), keyCol);
    }

    if (sceneHasAnim) drawAnim();

    // Delete button (also bound to the Del key).
    {
        y += 10.0f;
        const float bw = width - 16.0f, bh = 16.0f;
        m_deleteRect = {px, y, bw, bh};
        m_hasDelete  = true;
        bool delHover = cursorActive && mx >= px && mx < px + bw && my >= y && my < y + bh;
        if (delHover) m_overButton = true;
        addQuad(px, y, bw, bh, delHover ? delHi : delBg);
        addOutline(px, y, bw, bh, 1.0f, delBorder);
        addText(px + 8.0f, y + std::floor((bh - PixelFont::CELL_H) * 0.5f), "Delete Entity", fsz, delTxt, px + bw - 4.0f);
    }

    if (m_vertices.size() > VERT_CAP || m_indices.size() > IDX_CAP) return;  // keep last good frame
    if (!m_vertices.empty()) {
        m_vertexBuffer.uploadData(m_allocator, m_vertices.data(), m_vertices.size() * sizeof(UIVertex));
        m_indexBuffer.uploadData(m_allocator, m_indices.data(), m_indices.size() * sizeof(uint32_t));
        m_indexCount = (uint32_t)m_indices.size();
    } else {
        m_indexCount = 0;
    }
}

void Inspector::draw(VkCommandBuffer cmd) {
    if (!m_visible || m_indexCount == 0) return;
    VkBuffer buffers[] = { m_vertexBuffer.getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

bool Inspector::hitMaterialSlot(double mx, double my) const {
    return m_visible && m_hasSlot
        && mx >= m_slotX && mx < m_slotX + m_slotW
        && my >= m_slotY && my < m_slotY + m_slotH;
}

bool Inspector::handleMouseButton(int button, int action) {
    if (!m_visible || button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return false;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    if (mx < m_x0 || mx >= m_x0 + m_w || my < m_top || my >= m_top + m_h) return false;  // not over panel

    // Begin a transform drag-scrub if a number field was pressed.
    if (m_hasFields) {
        for (int i = 0; i < 9; ++i) {
            const Rect& r = m_fieldRect[i];
            if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) {
                m_scrubField = i; m_scrubLastX = mx; m_scrubDirty = false; return true;
            }
        }
    }
    if (m_hasAnimBtn && mx >= m_animBtnRect.x && mx < m_animBtnRect.x + m_animBtnRect.w
                     && my >= m_animBtnRect.y && my < m_animBtnRect.y + m_animBtnRect.h) {
        if (m_onAnimToggle) m_onAnimToggle();
        return true;
    }
    if (m_hasDelete && mx >= m_deleteRect.x && mx < m_deleteRect.x + m_deleteRect.w
                    && my >= m_deleteRect.y && my < m_deleteRect.y + m_deleteRect.h) {
        if (m_onDelete) m_onDelete();
        return true;
    }
    if (hitMaterialSlot(mx, my) && m_onAssign) m_onAssign();
    return true;   // consume clicks anywhere on the panel
}

void Inspector::handleRelease() { m_scrubField = -1; }

void Inspector::triggerDelete() { if (m_onDelete) m_onDelete(); }

// ── geometry helpers ─────────────────────────────────────────────────────────

void Inspector::addQuad(float x, float y, float w, float h, const glm::vec4& color) {
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

void Inspector::addOutline(float x, float y, float w, float h, float t, const glm::vec4& color) {
    addQuad(x, y, w, t, color);             // top
    addQuad(x, y + h - t, w, t, color);     // bottom
    addQuad(x, y, t, h, color);             // left
    addQuad(x + w - t, y, t, h, color);     // right
}

void Inspector::addGlyph(float x, float y, char c, float s, const glm::vec4& color) {
    const uint8_t* rows = PixelFont::glyphRows(c);
    if (!rows) return;
    for (int r = 0; r < PixelFont::CELL_H; ++r) {
        uint8_t bits = rows[r];
        for (int col = 0; col < PixelFont::CELL_W; ++col)
            if (bits & (1 << (PixelFont::CELL_W - 1 - col)))
                addQuad(x + col * s, y + r * s, s, s, color);
    }
}

float Inspector::addText(float x, float y, const std::string& text, float s, const glm::vec4& color, float maxX) {
    float cx = x;
    for (char c : text) {
        if (cx + PixelFont::CELL_W * s > maxX) break;
        addGlyph(cx, y, c, s, color);
        cx += PixelFont::ADVANCE * s;
    }
    return cx - x;
}

} // namespace Nyx
