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
#include "renderer/PointShadowMap.h"
#include "renderer/UniformTypes.h"
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

    // editor.prefs is read before m_camera.init(), so a saved pose is parked
    // here and applied to the camera immediately after init runs.
    struct PendingCameraPose {
        bool      has = false;
        glm::vec3 position{};
        float     yaw = 0.0f, pitch = 0.0f, fov = 45.0f;
    };
    PendingCameraPose        m_pendingCameraPose;

    // Window position parsed from editor.prefs before the window is created.
    // Size is fed straight into the Window ctor (so the swapchain comes up at the
    // right resolution); position is applied via Window::setPosition right after
    // creation. -1 means "no saved value, let the OS pick".
    struct PendingWindow { int x = -1, y = -1, w = 1280, h = 720; bool maximized = false; };
    PendingWindow            m_pendingWindow;

    // Point-light shadow maps. Up to MAX_POINT_SHADOWS cube maps live for the
    // engine's lifetime; each frame, enabled point lights are assigned to
    // free slots. m_pointShadowJobs queues the render work for the Renderer.
    std::array<PointShadowMap, MAX_POINT_SHADOWS> m_pointShadows;
    std::vector<PointShadowJob>                   m_pointShadowJobs;

    // Tear down and re-init one shadow slot at a new per-face resolution.
    // Used when a light's shadowResolution crosses a tier boundary. The cube
    // view changes, so the global descriptor binding (set 0, binding 2) is
    // also rewritten with the updated view list.
    void rebuildPointShadowSlot(int slot, uint32_t resolution);
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
    bool   m_rightDockCollapsed = false;          // Ctrl+B toggles
    static constexpr float RIGHT_DOCK_MIN            = 180.0f;
    static constexpr float RIGHT_DOCK_MAX            = 600.0f;
    static constexpr float RIGHT_DOCK_GRAB           = 5.0f;
    static constexpr float RIGHT_DOCK_COLLAPSED_W    = 18.0f;   // thin rail width when collapsed
    bool   overRightDockEdge() const;   // true when cursor is on (or over) the grab zone

    // Hierarchy ↔ Inspector vertical split inside the right dock. Stored in
    // pixels (clamped to the available dock height at layout time); dragging
    // the horizontal separator updates it. The 4-px grab zone straddles the
    // separator the same way the right-dock edge does.
    float  m_hierarchyHeight   = 320.0f;
    bool   m_hierSplitResizing = false;
    static constexpr float HIER_SPLIT_MIN  = 80.0f;
    static constexpr float HIER_SPLIT_GRAB = 4.0f;
    bool   overHierSplitEdge() const;
    void   toggleRightDockCollapsed() { m_rightDockCollapsed = !m_rightDockCollapsed; }

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

    // Singleton "Environment" entity — holds the per-scene aesthetic settings
    // (sky gradient, bloom, exposure, ambient). Auto-created if a loaded scene
    // doesn't have one. Persisted through .scene save/load.
    Entity m_environmentEntity = NULL_ENTITY;
    Entity ensureEnvironmentEntity();   // create on demand or return existing

    void handleResize();
    // Drive one full frame: refresh window dims, recreate swapchain if size
    // changed, build the UBO, draw, and recover from VK_ERROR_OUT_OF_DATE.
    // Called from the WndProc during a modal Win32 resize loop so the window
    // contents update in real time while the user drags an edge.
    void renderOneFrame();
    void fixedUpdate(float dt);

    // Pivot for camera orbit/zoom: centroid of the current selection's world
    // positions, or a point in front of the camera when nothing is selected.
    glm::vec3 selectionPivot();

    // World-space AABB centre across an arbitrary entity set — same logic as
    // selectionPivot() but parameterised so commands like Group can ask for the
    // centre of a specific subset of entities rather than the live selection.
    glm::vec3 entitiesPivot(const std::vector<Entity>& ents);

    // Compute one entity's world-space AABB and call Camera::frame() with its
    // centre and bounding-sphere radius. Hierarchy double-click hits this.
    void      frameEntity(Entity e);

    // Attach a sphere mesh + tinted material to a Point light so it shows up
    // visually in the viewport. Idempotent — does nothing if the gizmo (or
    // any other mesh) is already present. The colour is baked into the
    // material UBO at allocation time; changing LightComponent.color later
    // won't retint the gizmo until the light is respawned.
    void      ensurePointLightGizmo(Entity e);
    // Walk every entity and call ensurePointLightGizmo. Cheap enough to run
    // after every scene load.
    void      attachLightGizmos();
    // Rewrite every point-light entity's TransformComponent.scale each frame
    // so the gizmo sphere stays a constant ~size in screen pixels regardless
    // of camera distance / FOV. Runs before TransformSystem::update.
    void      updateLightGizmoScales();
    // Re-upload each point-light gizmo's MaterialParams to its host-visible
    // UBO so the sphere visually tracks LightComponent.color changes made
    // through the inspector / picker. Cheap (one memcpy per light).
    void      syncLightGizmoColors();
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

    // Switch the engine to a different project. Saves the current project's
    // scene + prefs, clears all in-memory state (scene, undo history, open
    // editor tabs, selection), points m_projectPath at newPath, loads the
    // new project's scene/prefs/undo, and persists the choice in the user's
    // %APPDATA%\Nyx\last_project.txt so the next launch reopens it.
    void  switchProject(const std::string& newPath);

    // "Save As" → copy the entire current project (scenes + assets + prefs) into
    // a new sibling folder the user picks, then switch to it. Saves the current
    // project first so the copy is up to date.
    void  saveProjectAs();

    // Undo/redo (Ctrl+Z / Ctrl+Shift+Z): snapshot the whole scene before each mutating
    // action; undo restores the most recent snapshot, redo re-applies it. Covers
    // delete/spawn/paste/duplicate/transform-edit/material-assign uniformly.
    void  pushUndo();   // call before a mutating action (clears the redo stack)
    void  undo();
    void  redo();

    // Group the currently-selected entities under a new empty parent entity at
    // their centroid. Each child's position is re-expressed in the new parent's
    // space so its world transform is preserved. Bound to Ctrl+G.
    void  groupSelected();

    // For each "group" entity in the current selection (an empty parent with no
    // Mesh/Light/Environment), reparent its children to its own parent (or to
    // root) preserving world transforms, then destroy the now-empty group.
    void  ungroupSelected();

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

    // Persist / restore editor layout state (collapse flags + panel sizes) so
    // it survives a restart. Written to <projectPath>/editor.prefs.
    void  loadEditorPrefs();
    void  saveEditorPrefs();
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
        enum class Kind { Transform, Spawn, Delete, Snapshot, Color, Scalar, Group } kind = Kind::Snapshot;
        std::string path;                                          // backing file

        // Kind::Group — delta-style record of "create empty parent + reparent
        // selected". Undo destroys the group and restores each child's old
        // parent + local TRS; redo recreates the group and re-applies the
        // shifts. Only the affected entities are touched (no scene reload).
        Entity      groupEntity   = NULL_ENTITY;
        glm::vec3   groupPosition{};                               // world centroid the group was spawned at
        struct ChildReparent {
            Entity    entity;
            Entity    oldParent;
            glm::vec3 oldPosition;
            glm::quat oldRotation;
            glm::vec3 oldScale;
            Entity    newParent;
            glm::vec3 newPosition;
        };
        std::vector<ChildReparent> childReparents;

        // Kind::Transform — entity IDs + old/new TRS for each changed entity.
        struct TRS { Entity e; glm::vec3 pos; glm::quat rot; glm::vec3 scl; };
        std::vector<TRS> oldTransforms;
        std::vector<TRS> newTransforms;

        // Kind::Spawn / Delete — affected entities and the serialized form needed to
        // recreate them (covers both single-spawn and multi-spawn like paste/dup).
        std::vector<Entity> entities;                              // current registry ids; updated on each apply
        std::string         serialized;                            // text round-trippable through readEntities

        // Kind::Color — single-field delta of a color-bearing component.
        Entity                 colorEntity = NULL_ENTITY;
        Inspector::PickerField colorTarget = Inspector::PickerField::MaterialBase;
        glm::vec4              oldColor    {1.0f};
        glm::vec4              newColor    {1.0f};

        // Kind::Scalar — single-field delta of a numeric / enum component field.
        Entity                 scalarEntity = NULL_ENTITY;
        Inspector::ScalarField scalarTarget = Inspector::ScalarField::EnvSkyIntensity;
        float                  oldScalar   = 0.0f;
        float                  newScalar   = 0.0f;

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

    // In-progress color edit (Inspector picker). Captured between begin/end:
    // nullopt when no edit is active.
    bool                    m_pendingColorActive = false;
    Entity                  m_pendingColorEntity = NULL_ENTITY;
    Inspector::PickerField  m_pendingColorTarget = Inspector::PickerField::MaterialBase;
    glm::vec4               m_pendingColorOld    {1.0f};

    void beginColorUndo(Entity e, Inspector::PickerField t);
    void endColorUndo  (Entity e, Inspector::PickerField t);

    glm::vec4 readColorTarget (Entity e, Inspector::PickerField t) const;
    void      writeColorTarget(Entity e, Inspector::PickerField t, const glm::vec4& c);

    // In-progress scalar edit (Inspector scrub / tonemap button).
    bool                    m_pendingScalarActive = false;
    Entity                  m_pendingScalarEntity = NULL_ENTITY;
    Inspector::ScalarField  m_pendingScalarTarget = Inspector::ScalarField::EnvSkyIntensity;
    float                   m_pendingScalarOld    = 0.0f;

    void beginScalarUndo(Entity e, Inspector::ScalarField t);
    void endScalarUndo  (Entity e, Inspector::ScalarField t);

    float readScalarTarget (Entity e, Inspector::ScalarField t) const;
    void  writeScalarTarget(Entity e, Inspector::ScalarField t, float v);

    // Re-upload an entity's MaterialComponent params into its existing GPU-only
    // material UBO. Used after a live inspector edit of a material scalar
    // (subsurface) since that UBO is written via staging, not per frame.
    void  reuploadMaterialParams(Entity e);

    void pushSpawnAction(const std::vector<Entity>& created);      // call after entities are spawned
    void pushDeleteAction(const std::vector<Entity>& toDelete);    // call BEFORE the entities are destroyed
};

} // namespace Nyx
