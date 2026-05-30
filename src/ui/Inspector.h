#pragma once

// Inspector.h — Right-docked panel (bottom half) showing the selected entity's
// components: Transform (read-only), Material (base color / metallic / roughness
// / albedo) with a droppable slot, and Light info. Dragging a .png or .mat from
// the content browser onto the material slot rebinds the entity's material.

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

class Inspector {
public:
    // Identifies which color-bearing component field the picker is editing.
    // Public so the Engine can name it when packing per-color undo deltas.
    enum class PickerField : uint8_t {
        MaterialBase, LightColor,
        EnvSkyTop, EnvSkyHorizon, EnvSkyGround, EnvAmbient,
    };

    // Identifies which scalar (numeric / enum) component field the inspector
    // is scrubbing or button-toggling. Public so the engine can pack scalar
    // undo deltas the same way it does for colors.
    enum class ScalarField : uint8_t {
        EnvSkyIntensity,
        EnvBloomThreshold,
        EnvBloomKnee,
        EnvBloomStrength,
        EnvExposure,
        EnvTonemapper,    // discrete (0=ACES, 1=Reinhard, 2=None); driven by buttons, not scrubbed
        LightIntensity,
        LightRadius,
        LightCastsShadows,        // bool stored as 0/1
        LightShadowResolution,    // snapped to power-of-2 in 128..2048
        MaterialSubsurface,       // 0..1; reuploads the material UBO on change
    };

    static constexpr float HEADER_H = 20.0f;

    void init(VmaAllocator allocator, GLFWwindow* window);
    void cleanup(VmaAllocator allocator);

    // dragActive = a compatible (.png/.mat) row is being dragged from the content
    // browser, so the material slot highlights when the cursor is over it.
    void update(Registry& registry, Entity selected, bool dragActive,
                float x0, float width, float top, float height, bool cursorActive,
                bool sceneHasAnim, bool animPlaying, bool selectedAnimated);
    void draw(VkCommandBuffer cmd);

    bool handleMouseButton(int button, int action);   // slot click / field scrub / delete
    void handleRelease();                              // ends a transform drag-scrub
    void triggerDelete();                              // Del key → delete-request callback

    // Text-input keyboard routing. capturesKeyboard() returns true while a
    // scrub field is in click-to-type mode, so Input.cpp forwards typed chars
    // / Enter / Esc / Backspace to us instead of the editor or scene shortcuts.
    bool capturesKeyboard() const { return m_textEditActive; }
    bool handleChar(unsigned int codepoint);
    bool handleKey(int key, int action, int mods);
    // Called by Input.cpp on any left-press so a click elsewhere commits the
    // pending typed value before the next panel processes the click.
    void commitTextEditOnExternalClick();

    // Fired when the user clicks the material slot (assign from the content
    // browser's current selection) — the Engine owns the actual assignment.
    void setAssignRequestCallback(std::function<void()> cb) { m_onAssign = std::move(cb); }

    // Fired when the user deletes the selected entity (button or Del key).
    void setDeleteCallback(std::function<void()> cb) { m_onDelete = std::move(cb); }

    // Fired once when a transform drag-scrub first changes a value, so the Engine
    // can push an undo snapshot of the pre-edit state.
    void setBeginEditCallback(std::function<void()> cb) { m_onBeginEdit = std::move(cb); }

    // Color-edit begin/end pair. Engine builds a delta undo action that stores
    // the entity, target field, and old/new RGBA — no scene reload on undo.
    void setBeginColorEditCallback(std::function<void(Entity, PickerField)> cb) { m_onBeginColorEdit = std::move(cb); }
    void setEndColorEditCallback  (std::function<void(Entity, PickerField)> cb) { m_onEndColorEdit   = std::move(cb); }

    // Scalar-edit begin/end pair (sky intensity, bloom threshold/knee/strength,
    // exposure, tonemapper). Engine builds a per-field old/new float delta.
    void setBeginScalarEditCallback(std::function<void(Entity, ScalarField)> cb) { m_onBeginScalarEdit = std::move(cb); }
    void setEndScalarEditCallback  (std::function<void(Entity, ScalarField)> cb) { m_onEndScalarEdit   = std::move(cb); }

    // Fired on every value change during inspector edits — Engine uses it to mark
    // the scene dirty so the auto-save catches the final value after the edit ends.
    void setOnEditCallback(std::function<void()> cb) { m_onEdit = std::move(cb); }

    // Fired once when a scrub finishes (mouse release) so the Engine can commit a
    // single delta-based undo entry for the whole interaction.
    void setEndEditCallback(std::function<void()> cb) { m_onEndEdit = std::move(cb); }

    // Fired when the Play/Pause animation button is clicked.
    void setAnimToggleCallback(std::function<void()> cb) { m_onAnimToggle = std::move(cb); }

    // True if (mx,my) is over the material slot AND the selection has a material;
    // used by the Engine to resolve a cross-panel drag-drop from the content browser.
    bool hitMaterialSlot(double mx, double my) const;

    void setVisible(bool v) { m_visible = v; }
    bool isVisible() const { return m_visible; }

    // Engine sets the current selection group each frame so transform scrubs /
    // text-edits apply to every selected entity, not just the primary one.
    void setSelectionGroup(const std::vector<Entity>& sel) { m_selectionGroup = sel; }

    // Engine sets this each frame so the right-dock resize edge gets a mint
    // highlight strip drawn on the panel's left side while hovered/dragged.
    void setLeftEdgeHighlight(bool h) { m_leftEdgeHighlight = h; }

    // True when the cursor is over a clickable element (delete button, anim
    // play/pause, material slot, transform scrub field). Engine flips to the
    // pointer cursor based on this.
    bool wantsPointerCursor() const { return m_overButton; }

private:
    // Buffer capacities (1px-quad text adds up). Over-capacity geometry is dropped
    // rather than overrunning the GPU buffer.
    static constexpr uint32_t VERT_CAP = 24576;
    static constexpr uint32_t IDX_CAP  = 36864;   // 1.5 * VERT_CAP

    GLFWwindow*  m_window    = nullptr;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    bool         m_visible   = true;
    bool         m_overButton = false;   // set each update(); see wantsPointerCursor()
    std::vector<Entity> m_selectionGroup; // refreshed by Engine each frame
    bool         m_leftEdgeHighlight = false;   // mint resize-edge strip

    // Current rect + the material slot's screen rect (for hit-testing).
    float m_x0 = 0.0f, m_w = 0.0f, m_top = 0.0f, m_h = 0.0f;
    bool  m_hasSlot = false;
    float m_slotX = 0.0f, m_slotY = 0.0f, m_slotW = 0.0f, m_slotH = 0.0f;

    std::function<void()> m_onAssign;
    std::function<void()> m_onDelete;
    std::function<void()> m_onBeginEdit;
    std::function<void(Entity, PickerField)> m_onBeginColorEdit;
    std::function<void(Entity, PickerField)> m_onEndColorEdit;
    std::function<void(Entity, ScalarField)> m_onBeginScalarEdit;
    std::function<void(Entity, ScalarField)> m_onEndScalarEdit;
    std::function<void()> m_onEdit;
    std::function<void()> m_onEndEdit;
    std::function<void()> m_onAnimToggle;

    // Drag-scrub number fields for Transform: 0..2 = pos xyz, 3..5 = rotation (euler
    // degrees) xyz, 6..8 = scale xyz.
    struct Rect { float x = 0, y = 0, w = 0, h = 0; };
    // Indices 0..8 = pos/rot/scl xyz scrubs; index 9 = uniform-scale field on
    // the Scl row that multiplies all three components together.
    Rect   m_fieldRect[10] = {};
    float  m_fieldValue[10] = {};      // sampled each frame so handleRelease can seed text-edit
    bool   m_hasFields    = false;
    int    m_scrubField   = -1;        // field being dragged, -1 = none
    double m_scrubLastX   = 0.0;
    bool   m_scrubDirty   = false;     // a value actually changed this drag (undo pushed)
    // Euler-angle edit cache (degrees), refreshed from the quaternion when the
    // selection changes, so rotation scrubbing doesn't jump on quat round-trip.
    glm::vec3 m_euler       = {0, 0, 0};
    Entity    m_eulerEntity = NULL_ENTITY;
    Rect   m_deleteRect   = {};
    bool   m_hasDelete    = false;
    Rect   m_animBtnRect  = {};
    bool   m_hasAnimBtn   = false;

    // Color picker — click a swatch to open a popup with an SV square, hue
    // strip, alpha slider, and four drag-scrub RGBA channels (live edits go
    // straight to the component, same begin/edit/end pattern transforms use).
    enum class PickerDrag : uint8_t {
        None, SV, Hue, Alpha, ScrubR, ScrubG, ScrubB, ScrubA,
    };
    struct ColorSwatch { float x, y, w, h; PickerField field; };
    std::vector<ColorSwatch> m_swatches;

    Entity      m_lastSelected     = NULL_ENTITY;     // entity laid out this frame
    bool        m_pickerOpen       = false;
    bool        m_pickerJustOpened = false;           // request HSV init from component
    Entity      m_pickerEntity     = NULL_ENTITY;
    PickerField m_pickerField      = PickerField::MaterialBase;
    float       m_pickerX = 0, m_pickerY = 0, m_pickerW = 0, m_pickerH = 0;

    // HSV+A kept decomposed so hue survives S=0 / V=0 and dragging the SV
    // square doesn't reset the hue marker as the color rounds to RGB.
    float       m_pickerHue   = 0.0f;     // 0..360
    float       m_pickerSat   = 0.0f;     // 0..1
    float       m_pickerVal   = 1.0f;     // 0..1
    float       m_pickerAlpha = 1.0f;     // 0..1

    Rect        m_pickerSVRect       = {};
    Rect        m_pickerHueRect      = {};
    Rect        m_pickerAlphaRect    = {};
    Rect        m_pickerChanRect[4]  = {};   // R, G, B, A scrub fields

    PickerDrag  m_pickerDrag       = PickerDrag::None;
    double      m_pickerDragLastX  = 0.0;
    bool        m_pickerEditDirty  = false;
    bool        m_pickerForceApply = false;   // set on press; update() routes it through undo

    // Scalar drag-scrub + tonemap 3-button picker. Layout refreshed each frame
    // alongside the section text so they sit beside their numeric values.
    struct ScalarFieldRect { Rect rect; ScalarField field; float value; };
    std::vector<ScalarFieldRect> m_scalarFields;
    bool        m_scalarScrubActive = false;
    ScalarField m_scalarScrubField  = ScalarField::EnvSkyIntensity;
    Entity      m_scalarScrubEntity = NULL_ENTITY;
    double      m_scalarScrubLastX  = 0.0;
    bool        m_scalarScrubDirty  = false;
    bool        m_scalarForceApply  = false;   // set on tonemap-button click → one-shot apply
    float       m_scalarForceValue  = 0.0f;

    Rect        m_tonemapBtnRects[3] = {};   // ACES / Reinhard / None
    Rect        m_shadowToggleRect   = {};   // point-light "Casts Shadows" button
    bool        m_shadowToggleOn     = false; // current value snapshotted by layout

    // Click-to-type text edit. Activates when a press + release happens
    // within ~3 px and no scrub motion mutated the value. Either a scalar
    // field (env) or a transform field (pos/rot/scl index 0..8) is active —
    // m_textEditTransformField >= 0 means a transform field is the target.
    bool        m_textEditActive = false;
    ScalarField m_textEditField  = ScalarField::EnvSkyIntensity;
    Entity      m_textEditEntity = NULL_ENTITY;
    int         m_textEditTransformField  = -1;        // 0..8 (Pos/Rot/Scl × xyz)
    Entity      m_textEditTransformEntity = NULL_ENTITY;
    std::string m_textEditBuffer;
    double      m_scalarPressX   = 0.0;
    double      m_scalarPressY   = 0.0;
    double      m_transformPressX = 0.0;
    double      m_transformPressY = 0.0;

    // Deferred transform commit (handleKey has no Registry); drained in update().
    bool        m_transformCommitPending = false;
    int         m_transformCommitField   = -1;
    float       m_transformCommitValue   = 0.0f;
    Entity      m_transformCommitEntity  = NULL_ENTITY;

    void commitTextEdit();

    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    bool     m_buffersInitialized = false;
    uint32_t m_indexCount = 0;
    std::vector<UIVertex> m_vertices;
    std::vector<uint32_t> m_indices;

    void  addQuad(float x, float y, float w, float h, const glm::vec4& color);
    void  addGradientQuad(float x, float y, float w, float h,
                          const glm::vec4& tl, const glm::vec4& tr,
                          const glm::vec4& br, const glm::vec4& bl);
    void  addOutline(float x, float y, float w, float h, float t, const glm::vec4& color);
    void  addGlyph(float x, float y, char c, float s, const glm::vec4& color);
    float addText(float x, float y, const std::string& text, float s, const glm::vec4& color, float maxX);

    glm::vec4 readFieldColor(Registry& reg, Entity e, PickerField field) const;
    void      writeFieldColor(Registry& reg, Entity e, PickerField field,
                              const glm::vec3& rgb, float alpha) const;
};

} // namespace Nyx
