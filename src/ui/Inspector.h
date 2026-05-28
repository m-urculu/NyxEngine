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

    // Fired when the user clicks the material slot (assign from the content
    // browser's current selection) — the Engine owns the actual assignment.
    void setAssignRequestCallback(std::function<void()> cb) { m_onAssign = std::move(cb); }

    // Fired when the user deletes the selected entity (button or Del key).
    void setDeleteCallback(std::function<void()> cb) { m_onDelete = std::move(cb); }

    // Fired once when a transform drag-scrub first changes a value, so the Engine
    // can push an undo snapshot of the pre-edit state.
    void setBeginEditCallback(std::function<void()> cb) { m_onBeginEdit = std::move(cb); }

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
    bool         m_leftEdgeHighlight = false;   // mint resize-edge strip

    // Current rect + the material slot's screen rect (for hit-testing).
    float m_x0 = 0.0f, m_w = 0.0f, m_top = 0.0f, m_h = 0.0f;
    bool  m_hasSlot = false;
    float m_slotX = 0.0f, m_slotY = 0.0f, m_slotW = 0.0f, m_slotH = 0.0f;

    std::function<void()> m_onAssign;
    std::function<void()> m_onDelete;
    std::function<void()> m_onBeginEdit;
    std::function<void()> m_onEdit;
    std::function<void()> m_onEndEdit;
    std::function<void()> m_onAnimToggle;

    // Drag-scrub number fields for Transform: 0..2 = pos xyz, 3..5 = rotation (euler
    // degrees) xyz, 6..8 = scale xyz.
    struct Rect { float x = 0, y = 0, w = 0, h = 0; };
    Rect   m_fieldRect[9] = {};
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

    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    bool     m_buffersInitialized = false;
    uint32_t m_indexCount = 0;
    std::vector<UIVertex> m_vertices;
    std::vector<uint32_t> m_indices;

    void  addQuad(float x, float y, float w, float h, const glm::vec4& color);
    void  addOutline(float x, float y, float w, float h, float t, const glm::vec4& color);
    void  addGlyph(float x, float y, char c, float s, const glm::vec4& color);
    float addText(float x, float y, const std::string& text, float s, const glm::vec4& color, float maxX);
};

} // namespace Nyx
