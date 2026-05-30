#include "ui/Inspector.h"
#include "ui/PixelFont.h"
#include "ecs/Registry.h"
#include "ecs/components/MeshComponent.h"
#include "ecs/components/MaterialComponent.h"
#include "ecs/components/LightComponent.h"
#include "ecs/components/EnvironmentComponent.h"
#include "ecs/components/TransformComponent.h"
#include "renderer/Mesh.h"

#include <algorithm>
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
    if (reg.has<EnvironmentComponent>(e)) return "Environment";
    if (reg.has<LightComponent>(e))
        return reg.get<LightComponent>(e).type == LightComponent::Type::Directional
                   ? "Directional Light" : "Point Light";
    if (reg.has<MeshComponent>(e)) return "Mesh " + std::to_string(e);
    return "Entity " + std::to_string(e);
}

// Picker layout — chosen so the popup fits inside the 180px-min right dock.
//   width  = PAD + SV + GAP + HUEW + PAD                          = 164
//   height = PAD + SV + 6 + ALPHAH + 6 + ROWH + 4 + ROWH + PAD    = ~206
constexpr float PICKER_SV    = 128.0f;
constexpr float PICKER_HUEW  = 16.0f;
constexpr float PICKER_GAP   = 4.0f;
constexpr float PICKER_PAD   = 8.0f;
constexpr float PICKER_ALPHA_H = 14.0f;
constexpr float PICKER_ROWH  = 14.0f;
constexpr int   PICKER_SV_CELLS = 8;   // SV-square subdivision (smooths the bilinear gradient)

glm::vec3 hsvToRgb(float h, float s, float v) {
    if (s <= 0.0f) return glm::vec3(v);
    float hh = h;
    while (hh < 0.0f)   hh += 360.0f;
    while (hh >= 360.0f) hh -= 360.0f;
    hh /= 60.0f;
    int   i  = (int)std::floor(hh);
    float ff = hh - i;
    float p  = v * (1.0f - s);
    float q  = v * (1.0f - s * ff);
    float t  = v * (1.0f - s * (1.0f - ff));
    switch (i) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}
// Per-field scrub speed (value/px) and per-field clamp range. Tuned so a small
// drag tweaks the value at a useful precision and never crosses sensible bounds.
// Magnitude-aware scrub step. Default (small-magnitude) field stays at the
// per-field baseline (0.02 units/px for position, etc.); once the value crosses
// 1000 the step jumps to 1/px (so the last 3 digits of a 4-digit number are the
// scrubbable range), then to 1000/px above 10k (thousand range), and so on.
// Without this, dragging a position from 0 to 50000 takes thousands of pixels.
float scrubStepPerPx(float value, float baseStep) {
    float mag = std::fabs(value);
    if (mag >= 1000000.0f) return std::max(baseStep, 100000.0f);
    if (mag >= 100000.0f)  return std::max(baseStep,  10000.0f);
    if (mag >= 10000.0f)   return std::max(baseStep,   1000.0f);   // 5-digit → thousand range
    if (mag >= 1000.0f)    return std::max(baseStep,      1.0f);   // 4-digit → last 3 digits
    return baseStep;
}

// Pretty-print a value for the text-edit field: trim trailing zeros and a
// dangling decimal point so "1.500" → "1.5", "1.000" → "1", "1234.0" → "1234".
std::string formatEditValue(float v) {
    char buf[32];
    if (std::fabs(v) >= 1e6f) { std::snprintf(buf, sizeof(buf), "%g", v); return buf; }
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if    (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s.empty() ? std::string("0") : s;
}

float scalarSpeed(Inspector::ScalarField f) {
    using SF = Inspector::ScalarField;
    switch (f) {
        case SF::EnvSkyIntensity:   return 0.02f;
        case SF::EnvBloomThreshold: return 0.02f;
        case SF::EnvBloomKnee:      return 0.01f;
        case SF::EnvBloomStrength:  return 0.005f;
        case SF::EnvExposure:       return 0.05f;
        case SF::LightIntensity:    return 0.05f;
        case SF::LightRadius:       return 0.1f;
        case SF::LightShadowResolution: return 8.0f;   // ~32 px to step a tier
        default: return 0.01f;
    }
}
float scalarClamp(Inspector::ScalarField f, float v) {
    using SF = Inspector::ScalarField;
    switch (f) {
        case SF::EnvSkyIntensity:
        case SF::EnvBloomThreshold:
        case SF::LightIntensity:    return std::max(0.0f, v);
        case SF::LightRadius:       return std::max(0.01f, v);
        case SF::LightShadowResolution: {
            // Snap to nearest power of 2 in [128, 2048] so the renderer only
            // rebuilds the cube map when crossing a real tier boundary.
            float clamped = std::min(2048.0f, std::max(128.0f, v));
            int log2v = (int)std::round(std::log2(clamped));
            log2v = std::min(11, std::max(7, log2v));
            return static_cast<float>(1 << log2v);
        }
        case SF::EnvBloomKnee:
        case SF::EnvBloomStrength:  return std::min(1.0f, std::max(0.0f, v));
        case SF::EnvExposure:       return std::min(10.0f, std::max(-10.0f, v));
        default: return v;
    }
}

void rgbToHsv(const glm::vec3& rgb, float& h, float& s, float& v) {
    float mx = std::max({rgb.r, rgb.g, rgb.b});
    float mn = std::min({rgb.r, rgb.g, rgb.b});
    v = mx;
    if (mx <= 0.0f) { s = 0.0f; h = 0.0f; return; }
    float d = mx - mn;
    s = d / mx;
    if (d <= 0.0f) { h = 0.0f; return; }
    if      (mx == rgb.r) h = 60.0f * std::fmod(((rgb.g - rgb.b) / d) + 6.0f, 6.0f);
    else if (mx == rgb.g) h = 60.0f * (((rgb.b - rgb.r) / d) + 2.0f);
    else                  h = 60.0f * (((rgb.r - rgb.g) / d) + 4.0f);
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
    m_swatches.clear();
    m_scalarFields.clear();
    m_lastSelected = selected;

    // Close the popup if the selection moved out from under it.
    if (m_pickerOpen && selected != m_pickerEntity) {
        m_pickerOpen      = false;
        m_pickerDrag      = PickerDrag::None;
        m_pickerEditDirty = false;
    }

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

    // Drain a pending transform commit from a click-to-type text edit. Goes
    // through the regular Kind::Transform undo path so the typed edit shows
    // up in undo just like a scrub.
    if (m_transformCommitPending && m_transformCommitEntity == selected
        && reg.has<TransformComponent>(selected)) {
        if (m_onBeginEdit) m_onBeginEdit();
        const int field = m_transformCommitField;
        const float v   = m_transformCommitValue;
        // Apply to every selected entity (group edit). Text edits use the
        // typed value absolutely on each — easier to reason about than a
        // group-delta and matches what most editors do for typed input.
        std::vector<Entity> targets;
        targets.reserve(m_selectionGroup.size() + 1);
        for (Entity e : m_selectionGroup)
            if (e != NULL_ENTITY && reg.has<TransformComponent>(e)) targets.push_back(e);
        if (targets.empty()) targets.push_back(selected);

        for (Entity e : targets) {
            auto& te = reg.get<TransformComponent>(e);
            if (field < 3) {
                te.position[field] = v;
            } else if (field < 6) {
                int a = field - 3;
                if (e == selected) {
                    m_euler[a] = v;
                    te.rotation = glm::quat(glm::radians(m_euler));
                } else {
                    // Other entities: replace their axis-a Euler with v while
                    // keeping their other-axis rotations as-is.
                    glm::vec3 eul = glm::degrees(glm::eulerAngles(te.rotation));
                    eul[a] = v;
                    te.rotation = glm::quat(glm::radians(eul));
                }
            } else if (field < 9) {
                int a = field - 6;
                te.scale[a] = std::max(0.01f, v);
            } else {
                float s = std::max(0.01f, v);
                te.scale = glm::vec3(s, s, s);
            }
        }
        if (m_onEdit)    m_onEdit();
        if (m_onEndEdit) m_onEndEdit();
    }
    m_transformCommitPending = false;

    // ── Color picker: drain "just opened" → init HSV from component, then
    //     apply any in-progress drag (SV/Hue/Alpha or one of the RGBA scrubs).
    if (m_pickerOpen && m_pickerJustOpened && m_pickerEntity == selected) {
        glm::vec4 cur = readFieldColor(reg, selected, m_pickerField);
        rgbToHsv(glm::vec3(cur), m_pickerHue, m_pickerSat, m_pickerVal);
        m_pickerAlpha = cur.a;
        m_pickerJustOpened = false;
    }
    // Clear stale force-apply if the popup is closed.
    if (!m_pickerOpen) m_pickerForceApply = false;

    // A change this frame can come from (a) a press that staged a new value in
    // handleMouseButton (m_pickerForceApply), or (b) cursor motion while a
    // drag is active. Either path commits through the same undo callbacks.
    bool changedThisFrame = false;
    if (m_pickerForceApply && m_pickerOpen && m_pickerEntity == selected) {
        changedThisFrame   = true;
        m_pickerForceApply = false;
    }
    if (m_pickerDrag != PickerDrag::None) {
        bool down = cursorActive
                 && glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (!down || !m_pickerOpen || m_pickerEntity != selected) {
            m_pickerDrag = PickerDrag::None;
        } else {
            auto clamp01 = [](float v) { return std::min(1.0f, std::max(0.0f, v)); };
            if (m_pickerDrag == PickerDrag::SV) {
                const Rect& r = m_pickerSVRect;
                float ns = clamp01(((float)mx - r.x) / r.w);
                float nv = clamp01(1.0f - ((float)my - r.y) / r.h);
                if (ns != m_pickerSat || nv != m_pickerVal) {
                    m_pickerSat = ns; m_pickerVal = nv; changedThisFrame = true;
                }
            } else if (m_pickerDrag == PickerDrag::Hue) {
                const Rect& r = m_pickerHueRect;
                float nh = clamp01(((float)my - r.y) / r.h) * 359.999f;
                if (nh != m_pickerHue) { m_pickerHue = nh; changedThisFrame = true; }
            } else if (m_pickerDrag == PickerDrag::Alpha) {
                const Rect& r = m_pickerAlphaRect;
                float na = clamp01(((float)mx - r.x) / r.w);
                if (na != m_pickerAlpha) { m_pickerAlpha = na; changedThisFrame = true; }
            } else {
                // RGBA scrub: 1 unit per pixel over 0..255 → 0..1
                float d = (float)(mx - m_pickerDragLastX);
                m_pickerDragLastX = mx;
                if (d != 0.0f) {
                    glm::vec3 rgb = hsvToRgb(m_pickerHue, m_pickerSat, m_pickerVal);
                    int idx = (int)m_pickerDrag - (int)PickerDrag::ScrubR;
                    float delta = d * (1.0f / 255.0f);
                    if (idx < 3) {
                        rgb[idx] = clamp01(rgb[idx] + delta);
                        rgbToHsv(rgb, m_pickerHue, m_pickerSat, m_pickerVal);
                    } else {
                        m_pickerAlpha = clamp01(m_pickerAlpha + delta);
                    }
                    changedThisFrame = true;
                }
            }
        }
    }

    // Commit edits through the color undo path. onBeginColorEdit fires once
    // at the start of an interaction so the engine can push a full-scene
    // snapshot of pre-edit state (transform begin/end only captures TRS, so
    // it can't restore non-transform component edits). onEdit marks dirty.
    // No "end" callback is needed — the snapshot at begin is self-contained.
    if (changedThisFrame && m_pickerOpen && m_pickerEntity == selected) {
        if (!m_pickerEditDirty) {
            if (m_onBeginColorEdit) m_onBeginColorEdit(m_pickerEntity, m_pickerField);
            m_pickerEditDirty = true;
        }
        glm::vec3 rgb = hsvToRgb(m_pickerHue, m_pickerSat, m_pickerVal);
        writeFieldColor(reg, selected, m_pickerField, rgb, m_pickerAlpha);
        if (m_onEdit) m_onEdit();
    }
    if (m_pickerDrag == PickerDrag::None && m_pickerEditDirty) {
        if (m_onEndColorEdit) m_onEndColorEdit(m_pickerEntity, m_pickerField);
        m_pickerEditDirty = false;
    }

    // ── Scalar drag-scrub + tonemap force-apply ─────────────────────────────
    // Drives sky intensity, bloom threshold/knee/strength, exposure, and the
    // tonemap mode. Each interaction (one press → release) writes a single
    // ScalarField undo delta through the begin/end callback pair.
    auto writeScalar = [&](Entity e, ScalarField f, float val) {
        switch (f) {
            case ScalarField::LightIntensity:
                if (reg.has<LightComponent>(e)) reg.get<LightComponent>(e).intensity = val;
                return;
            case ScalarField::LightRadius:
                if (reg.has<LightComponent>(e)) reg.get<LightComponent>(e).radius = val;
                return;
            case ScalarField::LightCastsShadows:
                if (reg.has<LightComponent>(e))
                    reg.get<LightComponent>(e).castsShadows = (std::round(val) != 0.0f);
                return;
            case ScalarField::LightShadowResolution:
                if (reg.has<LightComponent>(e))
                    reg.get<LightComponent>(e).shadowResolution = (int)std::round(val);
                return;
            default: break;
        }
        if (!reg.has<EnvironmentComponent>(e)) return;
        auto& ec = reg.get<EnvironmentComponent>(e);
        switch (f) {
            case ScalarField::EnvSkyIntensity:   ec.skyIntensity  = val; break;
            case ScalarField::EnvBloomThreshold: ec.bloomThreshold = val; break;
            case ScalarField::EnvBloomKnee:      ec.bloomKnee      = val; break;
            case ScalarField::EnvBloomStrength:  ec.bloomStrength  = val; break;
            case ScalarField::EnvExposure:       ec.exposure       = val; break;
            case ScalarField::EnvTonemapper: {
                int idx = std::min(2, std::max(0, (int)std::round(val)));
                ec.tonemapper = static_cast<EnvironmentComponent::Tonemapper>(idx);
                break;
            }
            default: break;
        }
    };
    auto readScalar = [&](Entity e, ScalarField f) -> float {
        switch (f) {
            case ScalarField::LightIntensity:
                return reg.has<LightComponent>(e) ? reg.get<LightComponent>(e).intensity : 0.0f;
            case ScalarField::LightRadius:
                return reg.has<LightComponent>(e) ? reg.get<LightComponent>(e).radius : 0.0f;
            case ScalarField::LightCastsShadows:
                return (reg.has<LightComponent>(e) && reg.get<LightComponent>(e).castsShadows) ? 1.0f : 0.0f;
            case ScalarField::LightShadowResolution:
                return reg.has<LightComponent>(e) ? (float)reg.get<LightComponent>(e).shadowResolution : 512.0f;
            default: break;
        }
        if (!reg.has<EnvironmentComponent>(e)) return 0.0f;
        const auto& ec = reg.get<EnvironmentComponent>(e);
        switch (f) {
            case ScalarField::EnvSkyIntensity:   return ec.skyIntensity;
            case ScalarField::EnvBloomThreshold: return ec.bloomThreshold;
            case ScalarField::EnvBloomKnee:      return ec.bloomKnee;
            case ScalarField::EnvBloomStrength:  return ec.bloomStrength;
            case ScalarField::EnvExposure:       return ec.exposure;
            case ScalarField::EnvTonemapper:     return (float)(uint32_t)ec.tonemapper;
            default: break;
        }
        return 0.0f;
    };

    if (m_scalarScrubActive && m_scalarScrubEntity == selected) {
        bool down = cursorActive
                 && glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

        // Force-apply (tonemap button) runs once even if released same frame.
        if (m_scalarForceApply) {
            float cur = readScalar(selected, m_scalarScrubField);
            if (cur != m_scalarForceValue) {
                if (!m_scalarScrubDirty) {
                    if (m_onBeginScalarEdit) m_onBeginScalarEdit(selected, m_scalarScrubField);
                    m_scalarScrubDirty = true;
                }
                writeScalar(selected, m_scalarScrubField, m_scalarForceValue);
                if (m_onEdit) m_onEdit();
            }
            m_scalarForceApply = false;
        }

        // Continuous drag-scrub for the numeric fields.
        if (down && m_scalarScrubField != ScalarField::EnvTonemapper) {
            float d = (float)(mx - m_scalarScrubLastX);
            m_scalarScrubLastX = mx;
            if (d != 0.0f) {
                float cur = readScalar(selected, m_scalarScrubField);
                float nv  = scalarClamp(m_scalarScrubField,
                                        cur + d * scrubStepPerPx(cur, scalarSpeed(m_scalarScrubField)));
                if (nv != cur) {
                    if (!m_scalarScrubDirty) {
                        if (m_onBeginScalarEdit) m_onBeginScalarEdit(selected, m_scalarScrubField);
                        m_scalarScrubDirty = true;
                    }
                    writeScalar(selected, m_scalarScrubField, nv);
                    if (m_onEdit) m_onEdit();
                }
            }
        }

        if (!down) {
            if (m_scalarScrubDirty) {
                if (m_onEndScalarEdit) m_onEndScalarEdit(selected, m_scalarScrubField);
                m_scalarScrubDirty = false;
            }
            m_scalarScrubActive = false;
        }
    }

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

                // Build the list of targets: every selected entity that has a
                // transform. Falls back to just the primary when nothing else
                // is in the hierarchy selection.
                std::vector<Entity> targets;
                targets.reserve(m_selectionGroup.size() + 1);
                for (Entity e : m_selectionGroup)
                    if (e != NULL_ENTITY && reg.has<TransformComponent>(e)) targets.push_back(e);
                if (targets.empty()) targets.push_back(selected);

                // Rotation scrubs apply the same world-axis delta quaternion
                // to every target (each rotates about its own origin). Update
                // m_euler from the primary's pre-rotation so the displayed
                // value matches what the user is dragging.
                glm::quat deltaQ{1, 0, 0, 0};
                int rotAxis = -1;
                if (m_scrubField >= 3 && m_scrubField < 6) {
                    rotAxis = m_scrubField - 3;
                    m_euler[rotAxis] += d * 0.5f;
                    glm::vec3 worldAxis{0};
                    worldAxis[rotAxis] = 1.0f;
                    deltaQ = glm::angleAxis(glm::radians(d * 0.5f), worldAxis);
                }

                for (Entity e : targets) {
                    auto& t = reg.get<TransformComponent>(e);
                    // Local-space pivot: the entity's own bbox centre.
                    glm::vec3 c(0.0f);
                    if (reg.has<MeshComponent>(e)) {
                        const Mesh* m = reg.get<MeshComponent>(e).mesh;
                        if (m) c = (m->boundsMin() + m->boundsMax()) * 0.5f;
                    }

                    if (m_scrubField < 3) {
                        // Step scales with magnitude so big positions don't take a
                        // marathon drag to cross.
                        t.position[m_scrubField] += d * scrubStepPerPx(t.position[m_scrubField], 0.02f);
                    } else if (m_scrubField < 6) {
                        glm::quat rOld = t.rotation;
                        t.rotation     = deltaQ * t.rotation;
                        glm::vec3 scaled = t.scale * c;
                        t.position += rOld * scaled - t.rotation * scaled;
                    } else if (m_scrubField < 9) {
                        int a = m_scrubField - 6;
                        glm::vec3 sOld = t.scale;
                        t.scale[a] = std::max(0.01f, t.scale[a] + d * 0.01f);
                        t.position += t.rotation * ((sOld - t.scale) * c);
                    } else {
                        glm::vec3 sOld = t.scale;
                        float mult = 1.0f + d * 0.005f;
                        if (mult < 0.01f) mult = 0.01f;
                        t.scale = glm::vec3(
                            std::max(0.01f, t.scale.x * mult),
                            std::max(0.01f, t.scale.y * mult),
                            std::max(0.01f, t.scale.z * mult));
                        t.position += t.rotation * ((sOld - t.scale) * c);
                    }
                }
                if (m_onEdit) m_onEdit();   // mark scene dirty so auto-save catches the new value
            }
        }
    }

    auto line    = [&](const std::string& s, const glm::vec4& c) { addText(px, y, s, fsz, c, maxX); y += LH; };
    auto section = [&](const std::string& s) { y += 5.0f; addText(px, y, s, fsz, sectionCol, maxX); y += LH + 2.0f; };

    // Scrubbable single-value row: "Label  [value]". Hovered = pointer cursor;
    // active drag = mint background; text-edit = mint border + typed buffer +
    // underscore cursor. Hit rect cached for handleMouseButton.
    auto scalarRow = [&](const char* label, float value, ScalarField field) {
        const float lw = 90.0f;
        const float fh = 11.0f;
        const float fx = px + lw;
        const float fw = (x0 + width - 8.0f) - fx;
        addText(px, y + 2.0f, label, fsz, keyCol, maxX);
        m_scalarFields.push_back({{fx, y, fw, fh}, field, value});
        bool hov     = cursorActive && mx >= fx && mx < fx + fw && my >= y && my < y + fh;
        bool active  = m_scalarScrubActive && m_scalarScrubField == field;
        bool editing = m_textEditActive    && m_textEditField    == field;
        if (hov) m_overButton = true;
        addQuad(fx, y, fw, fh, (active || editing) ? fieldActive : fieldBg);
        addOutline(fx, y, fw, fh, 1.0f, editing ? mint : slotBorder);
        if (editing) {
            std::string display = m_textEditBuffer + "_";   // cheap blink-less cursor
            addText(fx + 4.0f, y + 2.0f, display, fsz, valCol, fx + fw - 2.0f);
        } else {
            addText(fx + 4.0f, y + 2.0f, fnum(value, 3), fsz, valCol, fx + fw - 2.0f);
        }
        y += fh + 3.0f;
    };

    // 3-button tonemap picker. Active mode = mint border + dark mint background.
    auto tonemapPicker = [&](EnvironmentComponent::Tonemapper current) {
        const char* labels[3] = {"ACES", "REINHARD", "NONE"};
        const float rh = 14.0f;
        const float bw = ((x0 + width - 8.0f) - px - 4.0f) / 3.0f;   // 3 buttons, 2px gaps
        for (int i = 0; i < 3; ++i) {
            const float bx = px + i * (bw + 2.0f);
            m_tonemapBtnRects[i] = {bx, y, bw, rh};
            bool hov      = cursorActive && mx >= bx && mx < bx + bw && my >= y && my < y + rh;
            bool selected = static_cast<int>(current) == i;
            if (hov) m_overButton = true;
            glm::vec4 bg     = selected ? slotHiBg : (hov ? glm::vec4{0.13f, 0.16f, 0.22f, 1.0f} : fieldBg);
            glm::vec4 border = selected ? mint     : slotBorder;
            addQuad(bx, y, bw, rh, bg);
            addOutline(bx, y, bw, rh, 1.0f, border);
            addText(bx + 4.0f, y + 3.0f, labels[i], fsz, valCol, bx + bw - 2.0f);
        }
        y += rh + 3.0f;
    };

    // Single button that flips a bool. Mint when on, slot bg when off. Click
    // routes through the scalar scrub framework so the change goes through
    // the same undo path as other inspector edits.
    auto toggleButton = [&](const char* label, bool on, ScalarField field, Rect& outRect) {
        const float rh = 14.0f;
        const float bx = px;
        const float bw = (x0 + width - 8.0f) - bx;
        outRect = {bx, y, bw, rh};
        bool hov = cursorActive && mx >= bx && mx < bx + bw && my >= y && my < y + rh;
        if (hov) m_overButton = true;
        glm::vec4 bg     = on ? slotHiBg : (hov ? glm::vec4{0.13f, 0.16f, 0.22f, 1.0f} : fieldBg);
        glm::vec4 border = on ? mint     : slotBorder;
        addQuad(bx, y, bw, rh, bg);
        addOutline(bx, y, bw, rh, 1.0f, border);
        std::string text = std::string(label) + (on ? "  ON" : "  OFF");
        addText(bx + 6.0f, y + 3.0f, text, fsz, valCol, bx + bw - 2.0f);
        (void)field;   // hit testing lives in handleMouseButton via the stored Rect
        y += rh + 3.0f;
    };

    // Color swatch + label that registers a clickable rect so handleMouseButton
    // can open the palette popup over it. Hovered swatches get a mint border.
    auto colorSwatch = [&](const char* label, const glm::vec3& c, PickerField field) {
        const float sx = px, sy = y, sw = 10.0f, sh = 10.0f;
        addQuad(sx, sy, sw, sh, {c.r, c.g, c.b, 1.0f});
        bool hov = cursorActive && mx >= sx && mx < sx + sw && my >= sy && my < sy + sh;
        addOutline(sx, sy, sw, sh, 1.0f, hov ? mint : slotBorder);
        if (hov) m_overButton = true;
        m_swatches.push_back({sx, sy, sw, sh, field});
        addText(px + 16.0f, y + 1.0f, label, fsz, keyCol, maxX);
        y += LH + 3.0f;
    };

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
                m_fieldRect[idx]  = {fx, y, fw, fh};
                m_fieldValue[idx] = v[a];
                if (cursorActive && mx >= fx && mx < fx + fw && my >= y && my < y + fh)
                    m_overButton = true;   // scrubbable transform field — pointer cursor
                bool editing = m_textEditActive && m_textEditTransformField == idx;
                addQuad(fx, y, fw, fh, (m_scrubField == idx || editing) ? fieldActive : fieldBg);
                addOutline(fx, y, fw, fh, 1.0f, editing ? mint : slotBorder);
                if (editing) {
                    std::string display = m_textEditBuffer + "_";
                    addText(fx + 3.0f, y + 2.0f, display, fsz, valCol, fx + fw - 2.0f);
                } else {
                    addText(fx + 3.0f, y + 2.0f, fnum(v[a]), fsz, valCol, fx + fw - 2.0f);
                }
            }
            y += fh + 3.0f;
        };
        fieldRow("Pos", tc.position, 0);
        // Point lights are dimensionless emitters at a position; rotation and
        // scale would have no effect on lighting and the scale row is owned by
        // the camera-relative gizmo anyway. Hide them so the inspector only
        // shows the controls that actually drive light placement.
        const bool isPointLight = reg.has<LightComponent>(selected)
                               && reg.get<LightComponent>(selected).type == LightComponent::Type::Point;
        if (!isPointLight) {
            fieldRow("Rot", m_euler, 3);
            // Scl row gets four fields instead of three: x / y / z and a
            // uniform-scale field that multiplies all three together. Useful
            // for resizing imported models without breaking xyz ratios.
            const float scfw  = std::floor((availW - 6.0f) / 4.0f);   // 4 fields, 3 × 2px gaps
            addText(px, y + 2.0f, "Scl", fsz, keyCol, maxX);
            for (int a = 0; a < 3; ++a) {
                float fx  = fx0 + a * (scfw + 2.0f);
                int   idx = 6 + a;
                m_fieldRect[idx]  = {fx, y, scfw, fh};
                m_fieldValue[idx] = tc.scale[a];
                if (cursorActive && mx >= fx && mx < fx + scfw && my >= y && my < y + fh)
                    m_overButton = true;
                bool editing = m_textEditActive && m_textEditTransformField == idx;
                addQuad(fx, y, scfw, fh, (m_scrubField == idx || editing) ? fieldActive : fieldBg);
                addOutline(fx, y, scfw, fh, 1.0f, editing ? mint : slotBorder);
                if (editing) {
                    std::string display = m_textEditBuffer + "_";
                    addText(fx + 3.0f, y + 2.0f, display, fsz, valCol, fx + scfw - 2.0f);
                } else {
                    addText(fx + 3.0f, y + 2.0f, fnum(tc.scale[a]), fsz, valCol, fx + scfw - 2.0f);
                }
            }
            // Uniform-scale field — index 9. Display = average so non-uniform
            // scales still read sensibly; drag scrubs multiplicatively (keeps
            // xyz ratios); typing sets all three components to that value.
            float ufx     = fx0 + 3 * (scfw + 2.0f);
            m_fieldRect[9]  = {ufx, y, scfw, fh};
            m_fieldValue[9] = (tc.scale.x + tc.scale.y + tc.scale.z) / 3.0f;
            if (cursorActive && mx >= ufx && mx < ufx + scfw && my >= y && my < y + fh)
                m_overButton = true;
            bool uEditing = m_textEditActive && m_textEditTransformField == 9;
            addQuad(ufx, y, scfw, fh, (m_scrubField == 9 || uEditing) ? fieldActive : fieldBg);
            addOutline(ufx, y, scfw, fh, 1.0f, uEditing ? mint : slotBorder);
            if (uEditing) {
                std::string display = m_textEditBuffer + "_";
                addText(ufx + 3.0f, y + 2.0f, display, fsz, valCol, ufx + scfw - 2.0f);
            } else {
                float avg = (tc.scale.x + tc.scale.y + tc.scale.z) / 3.0f;
                addText(ufx + 3.0f, y + 2.0f, fnum(avg), fsz, valCol, ufx + scfw - 2.0f);
            }
            y += fh + 3.0f;
        }
        m_hasFields = true;
    }

    // Lights carry an auto-generated material for the gizmo sphere; hide the
    // material UI on lights so the inspector shows only light-relevant props.
    if (reg.has<MaterialComponent>(selected) && !reg.has<LightComponent>(selected)) {
        const auto& mat = reg.get<MaterialComponent>(selected);
        section("MATERIAL");

        colorSwatch("Base Color",
                    glm::vec3(mat.baseColorFactor.r, mat.baseColorFactor.g, mat.baseColorFactor.b),
                    PickerField::MaterialBase);

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
        colorSwatch("Color", lc.color, PickerField::LightColor);
        scalarRow("Intensity", lc.intensity, ScalarField::LightIntensity);
        if (lc.type == LightComponent::Type::Point) {
            scalarRow("Radius", lc.radius, ScalarField::LightRadius);
            m_shadowToggleOn = lc.castsShadows;
            toggleButton("Casts Shadows", lc.castsShadows,
                         ScalarField::LightCastsShadows, m_shadowToggleRect);
            if (lc.castsShadows) {
                scalarRow("Shadow Res", (float)lc.shadowResolution,
                          ScalarField::LightShadowResolution);
            }
        } else {
            m_shadowToggleRect = {};   // not present on this entity
        }
    } else {
        m_shadowToggleRect = {};
    }

    if (reg.has<EnvironmentComponent>(selected)) {
        const auto& v = reg.get<EnvironmentComponent>(selected);

        section("SKY / IBL");
        colorSwatch("Sky Top",     v.skyTop,     PickerField::EnvSkyTop);
        colorSwatch("Sky Horizon", v.skyHorizon, PickerField::EnvSkyHorizon);
        colorSwatch("Sky Ground",  v.skyGround,  PickerField::EnvSkyGround);
        scalarRow("Intensity",  v.skyIntensity,  ScalarField::EnvSkyIntensity);

        section("AMBIENT");
        colorSwatch("Color", v.ambient, PickerField::EnvAmbient);

        section("BLOOM");
        scalarRow("Threshold", v.bloomThreshold, ScalarField::EnvBloomThreshold);
        scalarRow("Knee",      v.bloomKnee,      ScalarField::EnvBloomKnee);
        scalarRow("Strength",  v.bloomStrength,  ScalarField::EnvBloomStrength);

        section("TONEMAP");
        tonemapPicker(v.tonemapper);
        scalarRow("Exposure",  v.exposure,       ScalarField::EnvExposure);
    }

    if (sceneHasAnim) drawAnim();

    // Color picker popup — SV square + hue strip + alpha slider + RGBA scrubs.
    // NOTE: drawn AFTER the Delete button below so it overlays every section.
    auto drawPicker = [&]() {
    if (m_pickerOpen) {
        const float W = PICKER_PAD + PICKER_SV + PICKER_GAP + PICKER_HUEW + PICKER_PAD;
        const float H = PICKER_PAD + PICKER_SV + 6.0f + PICKER_ALPHA_H + 6.0f
                      + PICKER_ROWH + 4.0f + PICKER_ROWH + PICKER_PAD;

        float pxp = m_pickerX, pyp = m_pickerY;
        if (pxp + W > x0 + width - 4.0f) pxp = x0 + width - 4.0f - W;
        if (pyp + H > top + height - 4.0f) pyp = top + height - 4.0f - H;
        if (pxp < x0 + 4.0f) pxp = x0 + 4.0f;
        if (pyp < top + HEADER_H + 2.0f) pyp = top + HEADER_H + 2.0f;
        m_pickerX = pxp; m_pickerY = pyp; m_pickerW = W; m_pickerH = H;

        addQuad(pxp + 2.0f, pyp + 2.0f, W, H, {0.0f, 0.0f, 0.0f, 0.45f});
        addQuad(pxp, pyp, W, H, headerBg);
        addOutline(pxp, pyp, W, H, 1.0f, slotBorder);

        // ── SV square (sat=X, val=1−Y at fixed hue) ─────────────────────
        float svx = pxp + PICKER_PAD;
        float svy = pyp + PICKER_PAD;
        m_pickerSVRect = {svx, svy, PICKER_SV, PICKER_SV};
        glm::vec3 hueRgb = hsvToRgb(m_pickerHue, 1.0f, 1.0f);
        // Subdivide into N×N cells so the triangulated gradient looks bilinear.
        float cellW = PICKER_SV / PICKER_SV_CELLS;
        float cellH = PICKER_SV / PICKER_SV_CELLS;
        auto svCorner = [&](float s, float v) {
            glm::vec3 c = v * ((1.0f - s) * glm::vec3(1.0f) + s * hueRgb);
            return glm::vec4(c, 1.0f);
        };
        for (int j = 0; j < PICKER_SV_CELLS; ++j) {
            for (int i = 0; i < PICKER_SV_CELLS; ++i) {
                float s0 = (float)i / PICKER_SV_CELLS;
                float s1 = (float)(i + 1) / PICKER_SV_CELLS;
                float v0 = 1.0f - (float)j / PICKER_SV_CELLS;
                float v1 = 1.0f - (float)(j + 1) / PICKER_SV_CELLS;
                addGradientQuad(svx + i*cellW, svy + j*cellH, cellW, cellH,
                                svCorner(s0, v0), svCorner(s1, v0),
                                svCorner(s1, v1), svCorner(s0, v1));
            }
        }
        // SV cursor (small ring with black + white outlines for contrast on any bg)
        float scx = svx + m_pickerSat * PICKER_SV;
        float scy = svy + (1.0f - m_pickerVal) * PICKER_SV;
        addOutline(scx - 4.0f, scy - 4.0f, 8.0f, 8.0f, 1.0f, {0.0f, 0.0f, 0.0f, 1.0f});
        addOutline(scx - 3.0f, scy - 3.0f, 6.0f, 6.0f, 1.0f, {1.0f, 1.0f, 1.0f, 1.0f});

        // ── Hue strip (vertical) ─────────────────────────────────────────
        float huex = svx + PICKER_SV + PICKER_GAP;
        float huey = svy;
        m_pickerHueRect = {huex, huey, PICKER_HUEW, PICKER_SV};
        const glm::vec4 hueStops[7] = {
            {1,0,0,1}, {1,1,0,1}, {0,1,0,1}, {0,1,1,1},
            {0,0,1,1}, {1,0,1,1}, {1,0,0,1}
        };
        float segH = PICKER_SV / 6.0f;
        for (int i = 0; i < 6; ++i) {
            addGradientQuad(huex, huey + i*segH, PICKER_HUEW, segH,
                            hueStops[i], hueStops[i], hueStops[i+1], hueStops[i+1]);
        }
        addOutline(huex, huey, PICKER_HUEW, PICKER_SV, 1.0f, slotBorder);
        float hmy = huey + (m_pickerHue / 360.0f) * PICKER_SV;
        addQuad(huex - 2.0f, hmy - 1.0f, PICKER_HUEW + 4.0f, 2.0f, {0.0f, 0.0f, 0.0f, 1.0f});

        // ── Alpha slider (current color, 0 → 1 over a checkerboard) ──────
        float alphaY = svy + PICKER_SV + 6.0f;
        float alphaW = PICKER_SV + PICKER_GAP + PICKER_HUEW;
        m_pickerAlphaRect = {svx, alphaY, alphaW, PICKER_ALPHA_H};
        const float ch = 7.0f;
        int cCols = (int)std::ceil(alphaW / ch);
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < cCols; ++i) {
                glm::vec4 cc = (((i + j) & 1) == 0)
                    ? glm::vec4{0.32f, 0.32f, 0.32f, 1.0f}
                    : glm::vec4{0.58f, 0.58f, 0.58f, 1.0f};
                float qx = svx + i * ch;
                float qy = alphaY + j * (PICKER_ALPHA_H * 0.5f);
                float qw = std::min(ch, svx + alphaW - qx);
                float qh = PICKER_ALPHA_H * 0.5f;
                if (qw > 0.0f) addQuad(qx, qy, qw, qh, cc);
            }
        }
        glm::vec3 curRgb = hsvToRgb(m_pickerHue, m_pickerSat, m_pickerVal);
        glm::vec4 a0{curRgb.r, curRgb.g, curRgb.b, 0.0f};
        glm::vec4 a1{curRgb.r, curRgb.g, curRgb.b, 1.0f};
        addGradientQuad(svx, alphaY, alphaW, PICKER_ALPHA_H, a0, a1, a1, a0);
        addOutline(svx, alphaY, alphaW, PICKER_ALPHA_H, 1.0f, slotBorder);
        float amx = svx + m_pickerAlpha * alphaW;
        addQuad(amx - 1.0f, alphaY - 2.0f, 2.0f, PICKER_ALPHA_H + 4.0f, {0.0f, 0.0f, 0.0f, 1.0f});

        // ── RGBA channel scrub fields ────────────────────────────────────
        float chRowY = alphaY + PICKER_ALPHA_H + 6.0f;
        float chTotalW = PICKER_SV + PICKER_GAP + PICKER_HUEW;
        float chFW = (chTotalW - 6.0f) * 0.25f;     // 4 fields, 3 × 2px gaps
        int   rgba8[4] = {
            (int)std::round(curRgb.r * 255.0f),
            (int)std::round(curRgb.g * 255.0f),
            (int)std::round(curRgb.b * 255.0f),
            (int)std::round(m_pickerAlpha * 255.0f),
        };
        const char* chLabels[4] = {"R", "G", "B", "A"};
        for (int i = 0; i < 4; ++i) {
            float fx = svx + i * (chFW + 2.0f);
            m_pickerChanRect[i] = {fx, chRowY, chFW, PICKER_ROWH};
            bool hov = cursorActive && mx >= fx && mx < fx + chFW
                                    && my >= chRowY && my < chRowY + PICKER_ROWH;
            if (hov) m_overButton = true;
            PickerDrag ds = (PickerDrag)((int)PickerDrag::ScrubR + i);
            addQuad(fx, chRowY, chFW, PICKER_ROWH, (m_pickerDrag == ds) ? fieldActive : fieldBg);
            addOutline(fx, chRowY, chFW, PICKER_ROWH, 1.0f, slotBorder);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%s %3d", chLabels[i], rgba8[i]);
            addText(fx + 3.0f, chRowY + 3.0f, buf, fsz, valCol, fx + chFW - 2.0f);
        }

        // ── Hex readout ─────────────────────────────────────────────────
        float hxY = chRowY + PICKER_ROWH + 4.0f;
        char hexBuf[24];
        std::snprintf(hexBuf, sizeof(hexBuf), "HEX %02X%02X%02X%02X",
                      rgba8[0], rgba8[1], rgba8[2], rgba8[3]);
        addText(svx, hxY + 3.0f, hexBuf, fsz, keyCol, svx + chTotalW);
    }
    };

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

    // Picker last — its geometry must sit on top of the Delete button and any
    // section that happens to be rendered after it.
    drawPicker();

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

    // Color picker takes precedence while open. SV / Hue / Alpha presses also
    // apply the click position immediately (via m_pickerForceApply) so a
    // single click without drag still produces an undoable edit. Clicks
    // outside the popup dismiss it.
    if (m_pickerOpen) {
        if (mx >= m_pickerX && mx < m_pickerX + m_pickerW
         && my >= m_pickerY && my < m_pickerY + m_pickerH) {
            auto hit = [&](const Rect& r) {
                return mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h;
            };
            m_pickerEditDirty = false;
            auto clamp01 = [](float v) { return std::min(1.0f, std::max(0.0f, v)); };
            if (hit(m_pickerSVRect)) {
                m_pickerDrag = PickerDrag::SV;
                float ns = clamp01(((float)mx - m_pickerSVRect.x) / m_pickerSVRect.w);
                float nv = clamp01(1.0f - ((float)my - m_pickerSVRect.y) / m_pickerSVRect.h);
                if (ns != m_pickerSat || nv != m_pickerVal) {
                    m_pickerSat = ns; m_pickerVal = nv; m_pickerForceApply = true;
                }
                return true;
            }
            if (hit(m_pickerHueRect)) {
                m_pickerDrag = PickerDrag::Hue;
                float nh = clamp01(((float)my - m_pickerHueRect.y) / m_pickerHueRect.h) * 359.999f;
                if (nh != m_pickerHue) { m_pickerHue = nh; m_pickerForceApply = true; }
                return true;
            }
            if (hit(m_pickerAlphaRect)) {
                m_pickerDrag = PickerDrag::Alpha;
                float na = clamp01(((float)mx - m_pickerAlphaRect.x) / m_pickerAlphaRect.w);
                if (na != m_pickerAlpha) { m_pickerAlpha = na; m_pickerForceApply = true; }
                return true;
            }
            for (int i = 0; i < 4; ++i) {
                if (hit(m_pickerChanRect[i])) {
                    m_pickerDrag = (PickerDrag)((int)PickerDrag::ScrubR + i);
                    m_pickerDragLastX = mx;
                    return true;
                }
            }
            return true;   // consume non-control clicks inside the popup
        }
        m_pickerOpen       = false;
        m_pickerDrag       = PickerDrag::None;
        m_pickerForceApply = false;
        return true;       // swallow the dismiss click
    }

    // Scalar scrub field hit → start a drag-scrub for that field. Press
    // position is captured so handleRelease can tell click (→ text edit) from
    // drag (→ scrub end).
    for (const auto& s : m_scalarFields) {
        if (mx >= s.rect.x && mx < s.rect.x + s.rect.w
         && my >= s.rect.y && my < s.rect.y + s.rect.h) {
            m_scalarScrubActive = true;
            m_scalarScrubField  = s.field;
            m_scalarScrubEntity = m_lastSelected;
            m_scalarScrubLastX  = mx;
            m_scalarScrubDirty  = false;
            m_scalarPressX      = mx;
            m_scalarPressY      = my;
            return true;
        }
    }
    // Shadow toggle button → flip the bool through the scalar force-apply path.
    // Uses the value snapshotted by the most recent layout (m_shadowToggleOn).
    if (m_shadowToggleRect.w > 0.0f
        && mx >= m_shadowToggleRect.x && mx < m_shadowToggleRect.x + m_shadowToggleRect.w
        && my >= m_shadowToggleRect.y && my < m_shadowToggleRect.y + m_shadowToggleRect.h
        && m_lastSelected != NULL_ENTITY) {
        m_scalarScrubActive = true;
        m_scalarScrubField  = ScalarField::LightCastsShadows;
        m_scalarScrubEntity = m_lastSelected;
        m_scalarScrubDirty  = false;
        m_scalarForceApply  = true;
        m_scalarForceValue  = m_shadowToggleOn ? 0.0f : 1.0f;
        return true;
    }

    // Tonemap button click → one-shot apply via the scrub framework (force-apply).
    for (int i = 0; i < 3; ++i) {
        const Rect& r = m_tonemapBtnRects[i];
        if (r.w <= 0.0f) continue;
        if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) {
            m_scalarScrubActive = true;
            m_scalarScrubField  = ScalarField::EnvTonemapper;
            m_scalarScrubEntity = m_lastSelected;
            m_scalarScrubDirty  = false;
            m_scalarForceApply  = true;
            m_scalarForceValue  = (float)i;
            return true;
        }
    }

    // Swatch click opens the picker anchored just below the swatch.
    for (const auto& s : m_swatches) {
        if (mx >= s.x && mx < s.x + s.w && my >= s.y && my < s.y + s.h) {
            m_pickerOpen       = true;
            m_pickerJustOpened = true;
            m_pickerField      = s.field;
            m_pickerEntity     = m_lastSelected;
            m_pickerX          = s.x;
            m_pickerY          = s.y + s.h + 2.0f;
            return true;
        }
    }

    // Begin a transform drag-scrub if a number field was pressed.
    if (m_hasFields) {
        for (int i = 0; i < 10; ++i) {
            const Rect& r = m_fieldRect[i];
            if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) {
                m_scrubField = i; m_scrubLastX = mx; m_scrubDirty = false;
                m_transformPressX = mx; m_transformPressY = my;
                return true;
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

void Inspector::handleRelease() {
    // Close the transform scrub undo group. Without this, the next update()
    // sees m_scrubField == -1 and skips the end-edit branch, so the engine's
    // open undo entry from onBeginEdit never gets committed.
    if (m_scrubField >= 0) {
        const int   field    = m_scrubField;
        const bool  didScrub = m_scrubDirty;
        const Entity entity  = m_eulerEntity;   // set during the scrub frame

        m_scrubField = -1;
        if (m_scrubDirty) {
            if (m_onEndEdit) m_onEndEdit();
            m_scrubDirty = false;
        }

        // Click-without-drag on a transform field → enter text-edit mode for
        // that field. Same threshold as the scalar fields. Buffer is pre-filled
        // with the current value (formatted) so the user can edit the digits
        // they want instead of retyping from scratch.
        if (!didScrub) {
            double mx = 0.0, my = 0.0;
            glfwGetCursorPos(m_window, &mx, &my);
            if (std::fabs(mx - m_transformPressX) < 3.0
             && std::fabs(my - m_transformPressY) < 3.0) {
                m_textEditActive          = true;
                m_textEditTransformField  = field;
                m_textEditTransformEntity = entity;
                // Seed the buffer with the value sampled last update() so the
                // user edits the digits instead of typing from scratch.
                m_textEditBuffer = formatEditValue(m_fieldValue[field]);
            }
        }
    }
    // Picker: end-edit pairs with the begin callback so the engine can fill in
    // the post-edit color and push the delta action.
    if (m_pickerDrag != PickerDrag::None) {
        m_pickerDrag = PickerDrag::None;
        if (m_pickerEditDirty) {
            if (m_onEndColorEdit) m_onEndColorEdit(m_pickerEntity, m_pickerField);
            m_pickerEditDirty = false;
        }
    }
    // Scalar scrub: close out. If the press never moved (true click) and no
    // scrub or tonemap force-apply ran, switch into text-edit on that field.
    if (m_scalarScrubActive) {
        const bool didScrub      = m_scalarScrubDirty;
        const bool didForceApply = m_scalarForceApply;
        const ScalarField field  = m_scalarScrubField;
        const Entity entity      = m_scalarScrubEntity;

        if (didScrub && m_onEndScalarEdit) m_onEndScalarEdit(entity, field);
        m_scalarScrubDirty  = false;
        m_scalarScrubActive = false;
        m_scalarForceApply  = false;

        if (!didScrub && !didForceApply && field != ScalarField::EnvTonemapper) {
            double mx = 0.0, my = 0.0;
            glfwGetCursorPos(m_window, &mx, &my);
            if (std::fabs(mx - m_scalarPressX) < 3.0
             && std::fabs(my - m_scalarPressY) < 3.0) {
                m_textEditActive = true;
                m_textEditField  = field;
                m_textEditEntity = entity;
                // Seed buffer with the value rendered this frame so the user
                // can edit instead of retyping.
                float seed = 0.0f;
                for (const auto& sf : m_scalarFields)
                    if (sf.field == field) { seed = sf.value; break; }
                m_textEditBuffer = formatEditValue(seed);
            }
        }
    }
}

glm::vec4 Inspector::readFieldColor(Registry& reg, Entity e, PickerField field) const {
    if (e == NULL_ENTITY) return glm::vec4(1.0f);
    switch (field) {
        case PickerField::MaterialBase:
            if (reg.has<MaterialComponent>(e)) return reg.get<MaterialComponent>(e).baseColorFactor;
            break;
        case PickerField::LightColor:
            if (reg.has<LightComponent>(e)) return glm::vec4(reg.get<LightComponent>(e).color, 1.0f);
            break;
        case PickerField::EnvSkyTop:
            if (reg.has<EnvironmentComponent>(e)) return glm::vec4(reg.get<EnvironmentComponent>(e).skyTop, 1.0f);
            break;
        case PickerField::EnvSkyHorizon:
            if (reg.has<EnvironmentComponent>(e)) return glm::vec4(reg.get<EnvironmentComponent>(e).skyHorizon, 1.0f);
            break;
        case PickerField::EnvSkyGround:
            if (reg.has<EnvironmentComponent>(e)) return glm::vec4(reg.get<EnvironmentComponent>(e).skyGround, 1.0f);
            break;
        case PickerField::EnvAmbient:
            if (reg.has<EnvironmentComponent>(e)) return glm::vec4(reg.get<EnvironmentComponent>(e).ambient, 1.0f);
            break;
    }
    return glm::vec4(1.0f);
}

void Inspector::writeFieldColor(Registry& reg, Entity e, PickerField field,
                                const glm::vec3& rgb, float alpha) const {
    if (e == NULL_ENTITY) return;
    switch (field) {
        case PickerField::MaterialBase:
            if (reg.has<MaterialComponent>(e)) {
                auto& mc = reg.get<MaterialComponent>(e);
                mc.baseColorFactor = glm::vec4(rgb, alpha);
            }
            break;
        case PickerField::LightColor:
            if (reg.has<LightComponent>(e)) reg.get<LightComponent>(e).color = rgb;
            break;
        case PickerField::EnvSkyTop:
        case PickerField::EnvSkyHorizon:
        case PickerField::EnvSkyGround:
        case PickerField::EnvAmbient:
            if (reg.has<EnvironmentComponent>(e)) {
                auto& ec = reg.get<EnvironmentComponent>(e);
                if      (field == PickerField::EnvSkyTop)     ec.skyTop     = rgb;
                else if (field == PickerField::EnvSkyHorizon) ec.skyHorizon = rgb;
                else if (field == PickerField::EnvSkyGround)  ec.skyGround  = rgb;
                else                                          ec.ambient    = rgb;
            }
            break;
    }
}

void Inspector::triggerDelete() { if (m_onDelete) m_onDelete(); }

bool Inspector::handleChar(unsigned int cp) {
    if (!m_textEditActive) return false;
    // Allow digits, decimal point, sign. Discard everything else so the input
    // can't be poisoned by stray characters from layout switches or paste.
    if (cp >= 32 && cp < 127) {
        char c = static_cast<char>(cp);
        bool ok = (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+';
        if (ok && m_textEditBuffer.size() < 16) m_textEditBuffer.push_back(c);
    }
    return true;
}

bool Inspector::handleKey(int key, int action, int mods) {
    (void)mods;
    if (!m_textEditActive) return false;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return true;
    if (key == GLFW_KEY_BACKSPACE) {
        if (!m_textEditBuffer.empty()) m_textEditBuffer.pop_back();
        return true;
    }
    if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
        commitTextEdit();
        return true;
    }
    if (key == GLFW_KEY_ESCAPE) {
        m_textEditActive = false;
        m_textEditBuffer.clear();
        return true;
    }
    return true;   // swallow other keys to avoid Ctrl+S / undo while typing
}

void Inspector::commitTextEdit() {
    if (!m_textEditActive) return;
    if (!m_textEditBuffer.empty()) {
        try {
            float v = std::stof(m_textEditBuffer);
            if (m_textEditTransformField >= 0) {
                // Defer to next update() so we can grab Registry and route the
                // change through onBeginEdit / onEdit / onEndEdit (Kind::Transform).
                m_transformCommitPending = true;
                m_transformCommitField   = m_textEditTransformField;
                m_transformCommitValue   = v;
                m_transformCommitEntity  = m_textEditTransformEntity;
            } else {
                v = scalarClamp(m_textEditField, v);
                // Reuse the scrub force-apply pipeline so the change goes through
                // the same begin/edit/end callbacks scrub does — one undo entry.
                m_scalarScrubActive = true;
                m_scalarScrubField  = m_textEditField;
                m_scalarScrubEntity = m_textEditEntity;
                m_scalarScrubDirty  = false;
                m_scalarForceApply  = true;
                m_scalarForceValue  = v;
            }
        } catch (...) {
            // Malformed input — discard silently.
        }
    }
    m_textEditActive         = false;
    m_textEditTransformField = -1;
    m_textEditBuffer.clear();
}

void Inspector::commitTextEditOnExternalClick() {
    if (m_textEditActive) commitTextEdit();
}

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

void Inspector::addGradientQuad(float x, float y, float w, float h,
                                const glm::vec4& tl, const glm::vec4& tr,
                                const glm::vec4& br, const glm::vec4& bl) {
    const glm::vec2 z2{0.0f, 0.0f};
    const glm::vec4 z4{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t base = (uint32_t)m_vertices.size();
    m_vertices.push_back({{x,     y},     tl, z2, z4, z4});
    m_vertices.push_back({{x + w, y},     tr, z2, z4, z4});
    m_vertices.push_back({{x + w, y + h}, br, z2, z4, z4});
    m_vertices.push_back({{x,     y + h}, bl, z2, z4, z4});
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
