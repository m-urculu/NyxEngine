#pragma once

// Engine.h — Main engine class with ECS, fixed timestep, and resource management

#include "Window.h"
#include "renderer/VulkanContext.h"
#include "renderer/Swapchain.h"
#include "renderer/Pipeline.h"
#include "renderer/Renderer.h"
#include "renderer/Descriptors.h"
#include "renderer/ResourceCache.h"
#include "renderer/ShadowMap.h"
#include "ui/UIPipeline.h"
#include "renderer/ImagePipeline.h"
#include "renderer/MaterialPreviewPipeline.h"
#include "ui/TitleBar.h"
#include "ui/ContentBrowser.h"
#include "ui/Console.h"
#include "ui/CodeEditor.h"
#include "ui/SceneHierarchy.h"
#include "ui/Inspector.h"
#include "ui/Gizmo.h"
#include "scene/Camera.h"
#include "core/Time.h"
#include "ecs/Registry.h"

#include <memory>
#include <vector>
#include <string>
#include <utility>
#include <iosfwd>
#include <functional>

namespace Nyx {

// Runtime animation: keyframe tracks retargeted from glTF nodes onto entities.
struct AnimChannelRT {
    Entity                 target = NULL_ENTITY;
    int                    path   = 0;   // 0 = translation, 1 = rotation, 2 = scale
    int                    interp = 0;   // 0 = linear, 1 = step
    std::vector<float>     times;
    std::vector<glm::vec4> values;       // T/S use xyz; R uses xyzw quaternion
};
struct AnimClipRT {
    std::string                name;
    float                      duration = 0.0f;
    float                      time     = 0.0f;
    bool                       loop     = true;
    std::vector<AnimChannelRT> channels;
};

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // onStatus (optional) — called as each major init phase begins, with a
    // short human-readable label and a progress estimate in [0,1] (or < 0 if
    // the previous bar fill should be kept). Used by the splash screen in
    // main() to keep the user informed; safe to leave empty.
    using StatusFn = std::function<void(const std::string& message, float progress)>;
    void init(StatusFn onStatus = {});
    void run();

private:
    std::unique_ptr<Window> m_window;
    VulkanContext            m_vulkanContext;
    Swapchain                m_swapchain;
    Descriptors              m_descriptors;
    Pipeline                 m_pipeline;
    Renderer                 m_renderer;
    ResourceCache            m_resourceCache;
    ShadowMap                m_shadowMap;
    UIPipeline               m_uiPipeline;
    ImagePipeline            m_imagePipeline;
    MaterialPreviewPipeline  m_matPreviewPipeline;
    TitleBar                 m_titleBar;
    ContentBrowser           m_contentBrowser;
    Console                  m_console;
    CodeEditor               m_editor;
    SceneHierarchy           m_hierarchy;
    Inspector                m_inspector;
    Gizmo                    m_gizmo;
    Camera                   m_camera;
    Time                     m_time;
    Registry                 m_registry;

    // Active project root (content lives under here; the engine source does not).
    std::string m_projectPath = "projects/Sandbox";

    // Editor selection (Scene Hierarchy → Inspector). Right dock has a runtime
    // width — user drags its inner (left) edge to resize. Clamped to a sensible
    // range; the 5-px grab zone sits on the edge in screen space.
    Entity m_selectedEntity = NULL_ENTITY;
    float  m_rightDockWidth   = 250.0f;
    bool   m_rightDockResizing = false;
    static constexpr float RIGHT_DOCK_MIN  = 180.0f;
    static constexpr float RIGHT_DOCK_MAX  = 600.0f;
    static constexpr float RIGHT_DOCK_GRAB = 5.0f;
    bool   overRightDockEdge() const;   // true when cursor is on (or over) the grab zone

    // ── Viewport picking + translation gizmo ───────────────────────────────────
    void   updateGizmo(float winW, float winH, bool cursorActive);  // per-frame: geometry + drag
    void   onViewportPress(double mx, double my);                   // viewport left-click: gizmo grab or pick
    Entity pickEntity(double mx, double my, float winW, float winH);// ray-cast nearest mesh

    bool       m_gizmoVisible = false;          // last frame's gizmo screen geometry (for hit-test)
    glm::vec2  m_gizmoOrigin{0.0f};
    glm::vec2  m_gizmoTip[3]{};
    float      m_gizmoWorldLen = 1.0f;
    int        m_gizmoAxis = -1;                // axis being dragged, -1 = not dragging
    std::vector<std::pair<Entity, glm::vec3>> m_gizmoStartPositions; // per-selected start pos at drag begin
    glm::dvec2 m_gizmoStartMouse{0.0};
    glm::vec2  m_gizmoDragAxisDir{0.0f};        // fixed screen-space axis dir during the drag
    float      m_gizmoDragWorldPerPx = 0.0f;

    // Viewport rubber-band marquee (click-drag in the 3D view to select objects).
    bool       m_vpLeftDown = false;            // a viewport press is pending (click vs marquee)
    bool       m_vpMarquee  = false;            // drag passed the threshold → marquee
    bool       m_vpAdditive = false;            // Shift/Ctrl held at press → add to selection
    glm::dvec2 m_vpPressPos{0.0};

    // Rebind the selected entity's material to a dropped/picked .png or .mat asset.
    void assignMaterialToSelected(const std::string& assetPath);

    // Orbiting child entity for demo
    Entity m_orbitEntity = NULL_ENTITY;
    float  m_orbitAngle  = 0.0f;

    // Light entities
    Entity m_sunEntity        = NULL_ENTITY;
    Entity m_pointLightEntity = NULL_ENTITY;
    Entity m_pointLightEntity2 = NULL_ENTITY;

    void handleResize();
    void fixedUpdate(float dt);

    // Pivot for camera orbit/zoom: centroid of the current selection's world
    // positions, or a point in front of the camera when nothing is selected.
    glm::vec3 selectionPivot();
    void updateUniformBuffer(uint32_t currentFrame);
    void buildDemoScene();
    // Load every primitive of a glTF as entities at `position`; returns the first.
    Entity loadGltfScene(const std::string& filepath, const glm::vec3& position = {0.0f, 1.5f, -2.0f},
                         float rootScale = 1.0f);

    // Helper to create an entity with Transform + Mesh + Material. `source` is the
    // mesh-source descriptor (see MeshComponent.source) and `albedoPath` the texture
    // path — both persisted so the entity round-trips through scene save/load.
    Entity createMeshEntity(Mesh* mesh, Texture* texture,
                            const glm::vec3& position = {0,0,0},
                            const glm::vec3& scale = {1,1,1},
                            const glm::vec4& baseColorFactor = {1,1,1,1},
                            float metallic = 0.0f,
                            float roughness = 0.5f,
                            const std::string& source = "",
                            const std::string& albedoPath = "",
                            Texture* normalTex     = nullptr,
                            Texture* metalRoughTex = nullptr,
                            Texture* occlusionTex  = nullptr,
                            float alphaCutoff      = 0.0f);

    // Rebuild a mesh from its source descriptor (prim:* / obj:* / gltf:*#i).
    Mesh* resolveMesh(const std::string& source);

    // glTF transform-animation playback (channels drive entity TransformComponents).
    std::vector<AnimClipRT> m_animClips;
    bool                    m_animPlaying = true;
    void updateAnimation(float dt);
    void updateSkins();   // upload per-skin joint matrices (set 2) each fixed step

    // Add a model file (.obj/.gltf/.glb) to the scene as one or more entities,
    // selecting the (first) new entity. Used by drag-from-content-browser.
    void  spawnModel(const std::string& path, const glm::vec3& position = {0,0,0});

    // Entity clipboard commands acting on the Scene Hierarchy's current selection.
    void  deleteSelection();
    void  copySelection();
    void  cutSelection();
    void  pasteClipboard();
    void  duplicateSelection();   // copy+paste in place without touching the clipboard

    // Save to the current scene path (or a default); bound to FILE>Save Scene + Ctrl+S.
    void  saveCurrentScene();

    // Undo/redo (Ctrl+Z / Ctrl+Shift+Z): snapshot the whole scene before each mutating
    // action; undo restores the most recent snapshot, redo re-applies it. Covers
    // delete/spawn/paste/duplicate/transform-edit/material-assign uniformly.
    void  pushUndo();   // call before a mutating action (clears the redo stack)
    void  undo();
    void  redo();

    // Scene serialization (readable line-based .scene). loadScene clears first.
    // Status hook used during scene load to stream per-asset progress to the
    // splash. Set by init(); cleared once the splash is dismissed.
    StatusFn m_statusFn;
    // Active sub-progress range while a scene/model is loading. base + (idx/total)*span
    // gives the bar fill; the parsers (readEntities, loadGltfScene) tick `idx` as each
    // asset is instantiated and emit a label naming what just loaded.
    bool   m_loadProgressActive = false;
    int    m_loadProgressIndex  = 0;
    int    m_loadProgressTotal  = 0;
    float  m_loadProgressBase   = 0.0f;
    float  m_loadProgressSpan   = 0.0f;
    void   reportLoadProgress(const std::string& label);

    void  saveScene(const std::string& path);
    void  loadScene(const std::string& path);
    void  clearScene();
    void  writeEntities(std::ostream& os, const std::vector<Entity>& ents);        // serialize entity blocks
    std::vector<Entity> readEntities(std::istream& is, const glm::vec3& posOffset); // instantiate; returns new ids
    std::vector<Entity> sceneEntities();        // all entities (Transform∪Mesh∪Light)
    std::string         snapshotScene();        // serialize the whole scene to a string
    void                restoreScene(const std::string& snap);  // clear + rebuild from a snapshot
    std::string m_currentScenePath;   // last saved/loaded scene (Save Scene target)
    std::string m_entityClipboard;    // serialized entities for copy/cut/paste

    // Persistent undo history — every mutation pushes an UndoAction (delta-based for
    // the common transform/spawn/delete cases, full snapshot fallback for material
    // changes). Each action gets a file in <projectPath>/scenes/.history/ so undo
    // survives close/reopen.
    struct UndoAction {
        enum class Kind { Transform, Spawn, Delete, Snapshot } kind = Kind::Snapshot;
        std::string path;                                          // backing file

        // Kind::Transform — entity IDs + old/new TRS for each changed entity.
        struct TRS { Entity e; glm::vec3 pos; glm::quat rot; glm::vec3 scl; };
        std::vector<TRS> oldTransforms;
        std::vector<TRS> newTransforms;

        // Kind::Spawn / Delete — affected entities and the serialized form needed to
        // recreate them (covers both single-spawn and multi-spawn like paste/dup).
        std::vector<Entity> entities;                              // current registry ids; updated on each apply
        std::string         serialized;                            // text round-trippable through readEntities

        // Kind::Snapshot — content lives in the file at `path`.
    };
    std::vector<UndoAction> m_undoStack;
    std::vector<UndoAction> m_redoStack;
    size_t                  m_historyCounter = 0;   // monotonic file-name counter
    bool                    m_sceneDirty     = false;
    static constexpr size_t MAX_HISTORY = 100;      // cap on undo steps (oldest .history/ file evicted past this)

    // In-progress transform edit captured between begin/endTransformUndo (gizmo drag,
    // inspector scrub). nullopt when not actively editing.
    bool        m_pendingTransformActive = false;
    std::vector<UndoAction::TRS> m_pendingTransformOld;

    std::string nextActionPath(const std::string& ext);
    void        writeActionFile(const UndoAction& a) const;
    bool        readActionFile(UndoAction& out, const std::string& path) const;
    void        applyAction(UndoAction& a, bool forward);          // forward=redo, !forward=undo
    void        pushAction(UndoAction&& a);                        // common stack push + persist + evict
    void        loadUndoHistoryFromDisk();

    void beginTransformUndo(const std::vector<Entity>& ents);      // capture pre-mutation TRS
    void endTransformUndo();                                       // capture post-mutation TRS, push action

    void pushSpawnAction(const std::vector<Entity>& created);      // call after entities are spawned
    void pushDeleteAction(const std::vector<Entity>& toDelete);    // call BEFORE the entities are destroyed
};

} // namespace Nyx
