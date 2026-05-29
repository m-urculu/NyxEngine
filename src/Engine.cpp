#include "Engine.h"
#include "Logger.h"
#include "Input.h"
#include "renderer/Vertex.h"
#include "renderer/UniformTypes.h"
#include "renderer/Buffer.h"
#include "renderer/ObjLoader.h"
#include "renderer/GltfLoader.h"
#include "renderer/Texture.h"
#include "renderer/Mesh.h"
#include "ecs/components/TransformComponent.h"
#include "ecs/components/MeshComponent.h"
#include "ecs/components/MaterialComponent.h"
#include "ecs/components/LightComponent.h"
#include "ecs/components/SkinComponent.h"
#include "ecs/components/EnvironmentComponent.h"
#include "ecs/systems/TransformSystem.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <set>
#include <limits>
#include <unordered_map>

namespace Nyx {

namespace {
// Image extensions that can be dropped straight onto a material slot (used as albedo).
bool isAssignableImage(const std::string& ext) {
    static const std::set<std::string> img = {
        ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".psd", ".hdr", ".pic"
    };
    return img.count(ext) > 0;
}
bool isModelFile(const std::string& ext) {
    static const std::set<std::string> m = {".obj", ".gltf", ".glb"};
    return m.count(ext) > 0;
}
std::string toLowerExt(const std::string& path) {
    std::string e = std::filesystem::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return e;
}

// ── Procedural primitives (white vertex color; material tints them). Wound
//    counter-clockwise outward to match the back-face-culling pipeline (verified
//    against the existing floor quad's +Y winding). Vertex = {pos, normal, color, uv}.
void makePlane(std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    const glm::vec3 c{1.0f, 1.0f, 1.0f};
    v = {
        {{-0.5f, 0.0f, -0.5f}, {0,1,0}, c, {0,0}},
        {{ 0.5f, 0.0f, -0.5f}, {0,1,0}, c, {1,0}},
        {{ 0.5f, 0.0f,  0.5f}, {0,1,0}, c, {1,1}},
        {{-0.5f, 0.0f,  0.5f}, {0,1,0}, c, {0,1}},
    };
    // Winding must give an upward geometric normal (matches the {0,1,0} vertex normal)
    // so the front face points up and back-face culling keeps the plane visible from
    // above. The original {0,1,2, 2,3,0} order produced a downward normal — the plane
    // was visible only from below and transparent from above.
    idx = {0, 2, 1, 2, 0, 3};
}
// UV sphere — used as the visible gizmo for point lights so they aren't an
// invisible dot in the scene view. Diameter ~0.4 (small enough to read as a
// gizmo but big enough to hover/click for selection); shaded by the regular
// PBR pipeline so it picks up the surrounding lighting.
void makeSphere(std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    const int   LATS = 8;
    const int   LONS = 12;
    const float R    = 0.2f;
    constexpr float kPi = 3.14159265358979323846f;
    const glm::vec3 col{1.0f, 1.0f, 1.0f};
    for (int i = 0; i <= LATS; ++i) {
        float t   = (float)i / (float)LATS;
        float phi = kPi * t;                          // 0..π
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int j = 0; j <= LONS; ++j) {
            float u   = (float)j / (float)LONS;
            float th  = (2.0f * kPi) * u;              // 0..2π
            glm::vec3 n{sp * std::cos(th), cp, sp * std::sin(th)};
            v.push_back({R * n, n, col, {u, t}});
        }
    }
    for (int i = 0; i < LATS; ++i) {
        for (int j = 0; j < LONS; ++j) {
            uint32_t a = (uint32_t)(i       * (LONS + 1) + j);
            uint32_t b = (uint32_t)((i + 1) * (LONS + 1) + j);
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
}
void makeCube(std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    const glm::vec3 c{1.0f, 1.0f, 1.0f};
    struct Face { glm::vec3 n; glm::vec3 p[4]; };
    const Face faces[6] = {
        {{ 0, 1, 0}, {{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}}}, // +Y
        {{ 0,-1, 0}, {{-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f}}}, // -Y
        {{ 1, 0, 0}, {{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f}}}, // +X
        {{-1, 0, 0}, {{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{-0.5f, 0.5f, 0.5f},{-0.5f,-0.5f, 0.5f}}}, // -X
        {{ 0, 0, 1}, {{-0.5f,-0.5f, 0.5f},{-0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f}}}, // +Z
        {{ 0, 0,-1}, {{-0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f},{-0.5f, 0.5f,-0.5f}}}, // -Z
    };
    const glm::vec2 uv[4] = {{0,0},{1,0},{1,1},{0,1}};
    for (const Face& f : faces) {
        uint32_t base = (uint32_t)v.size();
        for (int k = 0; k < 4; ++k) v.push_back({f.p[k], f.n, c, uv[k]});
        idx.insert(idx.end(), {base, base+1, base+2, base+2, base+3, base});
    }
}

// ── Viewport math (picking + gizmo projection) ───────────────────────────────
bool worldToScreen(const glm::vec3& w, const glm::mat4& vp, float sw, float sh, glm::vec2& out) {
    glm::vec4 c = vp * glm::vec4(w, 1.0f);
    if (c.w <= 1e-4f) return false;                       // at/behind the camera
    out.x = (c.x / c.w * 0.5f + 0.5f) * sw;
    out.y = (c.y / c.w * 0.5f + 0.5f) * sh;               // proj is Y-flipped for Vulkan
    return true;
}
void screenToRay(double mx, double my, const glm::mat4& vp, float sw, float sh, glm::vec3& o, glm::vec3& dir) {
    float nx = (float)(mx / sw) * 2.0f - 1.0f;
    float ny = (float)(my / sh) * 2.0f - 1.0f;
    glm::mat4 inv = glm::inverse(vp);
    glm::vec4 np = inv * glm::vec4(nx, ny, 0.0f, 1.0f);
    glm::vec4 fp = inv * glm::vec4(nx, ny, 1.0f, 1.0f);
    np /= np.w; fp /= fp.w;
    o   = glm::vec3(np);
    dir = glm::normalize(glm::vec3(fp) - glm::vec3(np));
}
// Slab test. o/d are in the AABB's local space; because d is the (un-normalized)
// transformed world direction, the returned t equals the world-space hit distance.
bool rayAABB(const glm::vec3& o, const glm::vec3& d, const glm::vec3& mn, const glm::vec3& mx, float& tHit) {
    float t0 = -1e30f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < mn[i] || o[i] > mx[i]) return false;
        } else {
            float inv = 1.0f / d[i];
            float ta = (mn[i] - o[i]) * inv, tb = (mx[i] - o[i]) * inv;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta); t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
    }
    if (t1 < 0.0f) return false;
    tHit = (t0 >= 0.0f) ? t0 : t1;
    return true;
}
float distToSeg(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
    glm::vec2 ab = b - a;
    float l2 = ab.x * ab.x + ab.y * ab.y;
    float t = (l2 > 1e-6f) ? glm::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
    glm::vec2 q = a + ab * t, e = p - q;
    return std::sqrt(e.x * e.x + e.y * e.y);
}
} // namespace

Engine::Engine() = default;

Engine::~Engine() {
    m_renderer.waitIdle(m_vulkanContext.getDevice());

    // Persist editor layout (collapse + sizes) before tearing things down.
    saveEditorPrefs();

    m_editor.cleanup(m_vulkanContext.getAllocator());
    m_gizmo.cleanup(m_vulkanContext.getAllocator());
    m_inspector.cleanup(m_vulkanContext.getAllocator());
    m_hierarchy.cleanup(m_vulkanContext.getAllocator());
    m_console.cleanup(m_vulkanContext.getAllocator());
    m_contentBrowser.cleanup(m_vulkanContext.getAllocator());
    m_titleBar.cleanup(m_vulkanContext.getAllocator());
    m_matPreviewPipeline.cleanup(m_vulkanContext.getDevice());
    m_imagePipeline.cleanup(m_vulkanContext.getDevice());
    m_uiPipeline.cleanup(m_vulkanContext.getDevice());
    m_resourceCache.cleanup(m_vulkanContext);
    m_renderer.cleanup(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());
    m_shadowMap.cleanup(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());
    for (auto& sm : m_pointShadows)
        sm.cleanup(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());
    m_pipeline.cleanup(m_vulkanContext.getDevice());
    m_descriptors.cleanup(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());
    m_swapchain.cleanup(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());
    m_vulkanContext.cleanup();

    LOG_INFO("Engine shut down");
}

void Engine::init(StatusFn onStatus) {
    m_statusFn = onStatus;   // lower layers (loadScene/loadGltfScene) tick into this
    auto status = [&](const char* s, float p) { if (onStatus) onStatus(s, p); };

    Logger::init();
    LOG_INFO("=== Nyx v0.3.0 (Phase 3B) ===");

    status("Opening window...", 0.02f);
    m_window = std::make_unique<Window>("Nyx Engine", 1280, 720);

    status("Initialising Vulkan...", 0.08f);
    m_vulkanContext.init(m_window->getHandle());

    status("Creating swapchain...", 0.18f);
    m_swapchain.init(m_vulkanContext, m_window->getWidth(), m_window->getHeight());

    m_descriptors.init(m_vulkanContext);

    status("Building pipelines...", 0.25f);
    m_pipeline.init(m_vulkanContext, m_swapchain.getExtent(), m_swapchain.getImageFormat(),
                     VK_FORMAT_R16G16B16A16_SFLOAT,                       // HDR scene attachment
                     m_swapchain.getDepthBuffer().getFormat(),
                     m_descriptors.getGlobalLayout(), m_descriptors.getMaterialLayout(),
                     m_descriptors.getJointLayout());

    // Shadow map: depth-only render target rendered each frame from the sun's POV.
    // The sampler is bound into set 0 binding 1 (one-time write — image is stable).
    // 1024² is a good single-character / small-scene size — quartering 2048² cuts the
    // shadow-pass rasterization cost ~4× with imperceptible quality loss for a tight
    // ortho volume; bump to 2048+ when scenes grow large.
    status("Preparing shadow map...", 0.38f);
    m_shadowMap.init(m_vulkanContext, m_descriptors.getGlobalLayout(), 1024);
    m_descriptors.setShadowMap(m_vulkanContext.getDevice(),
                               m_shadowMap.getView(), m_shadowMap.getSampler());

    // Point-light shadow cube map pool. Each slot starts at the default
    // resolution; gets rebuilt at the requested per-light tier when assigned.
    status("Preparing point shadow maps...", 0.40f);
    for (auto& sm : m_pointShadows) sm.init(m_vulkanContext, m_descriptors.getGlobalLayout());
    {
        VkImageView views[MAX_POINT_SHADOWS]{};
        for (int s = 0; s < MAX_POINT_SHADOWS; ++s) views[s] = m_pointShadows[s].getCubeView();
        m_descriptors.setPointShadowMaps(m_vulkanContext.getDevice(),
                                         views, m_pointShadows[0].getSampler());
    }
    // Prime each cube map (transitions every face to SHADER_READ_ONLY_OPTIMAL
    // so the descriptor binding's expected layout is correct before any light
    // ever writes to it).
    {
        VkCommandBuffer cmd = m_vulkanContext.beginSingleTimeCommands();
        for (auto& sm : m_pointShadows) sm.prime(cmd);
        m_vulkanContext.endSingleTimeCommands(cmd);
    }

    status("Setting up renderer...", 0.45f);
    m_renderer.init(m_vulkanContext, m_swapchain, m_pipeline);

    m_resourceCache.init(m_vulkanContext);

    // UI/image/material-preview pipelines draw AFTER composite in the composite RP,
    // so they target the swapchain (LDR sRGB) not the HDR scene.
    status("Loading editor UI...", 0.55f);
    m_uiPipeline.init(m_vulkanContext, m_pipeline.getCompositeRenderPass());
    m_imagePipeline.init(m_vulkanContext, m_pipeline.getCompositeRenderPass());
    m_matPreviewPipeline.init(m_vulkanContext, m_pipeline.getCompositeRenderPass());
    m_titleBar.init(m_vulkanContext.getAllocator(), m_window->getHandle());
    m_contentBrowser.init(m_vulkanContext.getAllocator(), m_window->getHandle(), m_projectPath);
    m_console.init(m_vulkanContext.getAllocator(), m_window->getHandle());
    m_editor.init(m_vulkanContext, m_window->getHandle(), &m_imagePipeline, &m_matPreviewPipeline);
    m_hierarchy.init(m_vulkanContext.getAllocator(), m_window->getHandle());
    m_inspector.init(m_vulkanContext.getAllocator(), m_window->getHandle());
    m_gizmo.init(m_vulkanContext.getAllocator());

    // Restore the last session's layout (collapse states + panel sizes) so the
    // user picks up where they left off. Failures are silent — first launch on
    // a project has no editor.prefs.
    loadEditorPrefs();

    // Dev console commands (help / clear are built in).
    const glm::vec4 errCol{0.96f, 0.42f, 0.40f, 1.0f};
    m_console.registerCommand("ver", "print engine version", [this](const std::vector<std::string>&) {
        m_console.print("Nyx Engine v0.3.0");
    });
    m_console.registerCommand("echo", "echo <text> - print text", [this](const std::vector<std::string>& a) {
        std::string s; for (size_t i = 0; i < a.size(); ++i) { if (i) s += ' '; s += a[i]; }
        m_console.print(s);
    });
    auto quitFn = [this](const std::vector<std::string>&) { glfwSetWindowShouldClose(m_window->getHandle(), GLFW_TRUE); };
    m_console.registerCommand("quit", "close the engine", quitFn);
    m_console.registerCommand("exit", "close the engine", quitFn);
    m_console.registerCommand("cam.fov", "cam.fov [deg] - get/set camera field of view",
                              [this, errCol](const std::vector<std::string>& a) {
        if (a.empty()) { m_console.print("fov = " + std::to_string(static_cast<int>(m_camera.getFov()))); return; }
        try {
            float f = std::clamp(std::stof(a[0]), 10.0f, 170.0f);
            m_camera.setFov(f);
            m_console.print("fov set to " + std::to_string(static_cast<int>(f)));
        } catch (...) { m_console.print("usage: cam.fov <degrees>", errCol); }
    });
    m_console.registerCommand("anim", "anim [play|pause] - toggle glTF animation playback",
                              [this](const std::vector<std::string>& a) {
        if      (!a.empty() && a[0] == "play")  m_animPlaying = true;
        else if (!a.empty() && a[0] == "pause") m_animPlaying = false;
        else                                    m_animPlaying = !m_animPlaying;
        m_console.print(std::string("animation ") + (m_animPlaying ? "playing" : "paused")
                        + " (" + std::to_string((int)m_animClips.size()) + " clip(s))");
    });

    // Every clicked file becomes a closable tab in the editor: code/text → editable
    // tab, image → flat preview, .mat → material sphere, everything else → a binary
    // info card (never gibberish text).
    m_contentBrowser.setFileOpenCallback([this](const std::string& path) {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        static const std::set<std::string> imageExt = {
            ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".psd", ".hdr", ".pic"
        };
        static const std::set<std::string> textExt = {
            ".txt", ".md", ".markdown", ".cpp", ".cxx", ".cc", ".c", ".h", ".hpp", ".hh", ".inl",
            ".glsl", ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese", ".rgen", ".rchit",
            ".json", ".gltf", ".obj", ".mtl", ".xml", ".yaml", ".yml", ".toml", ".ini", ".cfg",
            ".lua", ".py", ".js", ".ts", ".cmake", ".log", ".scm", ".csv", ".sh", ".bat", ".ps1", ""
        };
        if      (ext == ".scene")     loadScene(path);          // load a saved scene
        else if (ext == ".mat")       m_editor.openMaterial(path);
        else if (imageExt.count(ext)) m_editor.openImage(path);
        else if (textExt.count(ext))  m_editor.openFile(path);
        else                          m_editor.openBinary(path);
    });
    m_contentBrowser.setPathRemovedCallback([this](const std::string& path) {
        m_editor.closePath(path);
    });
    // File-menu (project) actions the engine owns.
    m_contentBrowser.setFileMenuCallback([this](const std::string& action) {
        if      (action == "save")    m_editor.save();
        else if (action == "saveall") m_editor.saveAll();
        else if (action == "savescene") saveCurrentScene();
        else if (action == "exit")    glfwSetWindowShouldClose(m_window->getHandle(), GLFW_TRUE);
        else if (action == "openproject") {
            std::string dir = m_window->openFolderDialog("Open Project Folder");
            if (!dir.empty()) m_contentBrowser.setProject(dir);
        }
    });

    // Scene Hierarchy → selection drives the Inspector; commands act on the selection.
    m_hierarchy.setSelectCallback([this](Entity e) { m_selectedEntity = e; });
    m_hierarchy.setCommandCallback([this](SceneHierarchy::Command c) {
        switch (c) {
            case SceneHierarchy::Command::Delete:    deleteSelection();    break;
            case SceneHierarchy::Command::Copy:      copySelection();      break;
            case SceneHierarchy::Command::Cut:       cutSelection();       break;
            case SceneHierarchy::Command::Paste:     pasteClipboard();     break;
            case SceneHierarchy::Command::Duplicate: duplicateSelection(); break;
        }
    });
    // Double-click in the hierarchy → camera frames the entity (Maya/Unity "F").
    m_hierarchy.setActivateCallback([this](Entity e) { frameEntity(e); });
    // ◀ button in the hierarchy header collapses the right sidebar (same toggle
    // as Ctrl+B and the title-bar button).
    m_hierarchy.setCollapseCallback([this]() { toggleRightDockCollapsed(); });
    // Inspector material slot: click assigns the content browser's current selection;
    // dragging a .png/.mat from the content browser onto the slot assigns that path.
    m_inspector.setAssignRequestCallback([this]() {
        assignMaterialToSelected(m_contentBrowser.selectedPath());
    });
    m_inspector.setDeleteCallback([this]() { deleteSelection(); });
    // Inspector scrubs commit as TRANSFORM deltas: begin captures pre-scrub TRS of the
    // currently-selected entity, end commits old/new pair as one undo entry, on-edit
    // ticks just keep the auto-save flag fresh. The inspector only edits the single
    // primary entity (m_selectedEntity); using m_hierarchy.selection() here would
    // capture stale or unrelated entities (e.g. a viewport-pick after a hierarchy
    // multi-select) and record a no-op delta — making undo skip the actual edit.
    m_inspector.setBeginEditCallback([this]() {
        std::vector<Entity> sel;
        if (m_selectedEntity != NULL_ENTITY) sel.push_back(m_selectedEntity);
        beginTransformUndo(sel);
    });
    m_inspector.setOnEditCallback([this]() { m_sceneDirty = true; });
    m_inspector.setEndEditCallback([this]() { endTransformUndo(); });
    // Color edits get a per-field delta: begin captures (entity, target,
    // pre-edit color), end captures the post-edit color and pushes a single
    // small UndoAction::Kind::Color. Undo writes the old color back to just
    // that one field — no scene reload, no other state touched.
    m_inspector.setBeginColorEditCallback([this](Entity e, Inspector::PickerField t) {
        beginColorUndo(e, t);
    });
    m_inspector.setEndColorEditCallback([this](Entity e, Inspector::PickerField t) {
        endColorUndo(e, t);
    });
    m_inspector.setBeginScalarEditCallback([this](Entity e, Inspector::ScalarField t) {
        beginScalarUndo(e, t);
    });
    m_inspector.setEndScalarEditCallback([this](Entity e, Inspector::ScalarField t) {
        endScalarUndo(e, t);
    });
    m_inspector.setAnimToggleCallback([this]() { m_animPlaying = !m_animPlaying; });
    m_contentBrowser.setExternalDropCallback([this](const std::string& path, double mx, double my) {
        // A model dropped anywhere outside the browser → add it to the scene.
        // An image/.mat dropped on the Inspector's material slot → assign it.
        if (isModelFile(toLowerExt(path)))        spawnModel(path);
        else if (m_inspector.hitMaterialSlot(mx, my)) assignMaterialToSelected(path);
    });

    Input::init(m_window->getHandle(), m_window.get());
    Input::setTitleBar(&m_titleBar);
    Input::setContentBrowser(&m_contentBrowser);
    Input::setConsole(&m_console);
    Input::setEditor(&m_editor);
    Input::setSceneHierarchy(&m_hierarchy);
    Input::setInspector(&m_inspector);
    Input::setSaveSceneCallback([this]() { saveCurrentScene(); });
    Input::setUndoCallback([this]() { undo(); });
    Input::setRedoCallback([this]() { redo(); });
    Input::setViewportPressCallback([this](double mx, double my) { onViewportPress(mx, my); });
    Input::setViewportZoomCallback([this](double sy) { m_camera.dolly(static_cast<float>(sy), selectionPivot()); });
    Input::setRightDockResizeCallback([this]() {
        if (!overRightDockEdge()) return false;
        m_rightDockResizing = true;
        return true;
    });
    Input::setHierSplitResizeCallback([this]() {
        if (!overHierSplitEdge()) return false;
        m_hierSplitResizing = true;
        return true;
    });
    Input::setToggleRightDockCallback([this]() { toggleRightDockCollapsed(); });

    float aspect = static_cast<float>(m_window->getWidth()) / static_cast<float>(m_window->getHeight());
    m_camera.init({0.0f, 2.0f, 6.0f}, aspect);

    m_time.init();

    // Load the project's saved scene if one exists; otherwise seed the demo scene
    // (lights + sample cubes) + mount the gladiator fresh so a brand-new project
    // isn't empty. The saved scene is authoritative — once you Save Scene, your
    // saved state is what loads, including the gladiator with full PBR maps and
    // any transforms you've applied (.scene round-trips materials losslessly).
    {
        std::string projectScene = m_projectPath + "/scenes/scene.scene";
        if (std::filesystem::exists(projectScene)) {
            status("Loading scene...", 0.65f);
            loadScene(projectScene);
        } else {
            status("Building demo scene...", 0.65f);
            buildDemoScene();
            std::string gp = m_projectPath + "/assets/models/Gladiator/Gladiator.gltf";
            if (std::filesystem::exists(gp)) {
                status("Loading model: Gladiator...", 0.75f);
                loadGltfScene(gp, {0.0f, 0.0f, -2.0f}, 1.5f);
            }
        }
    }

    // Resume the persistent undo history from disk so the user can keep undoing
    // changes from a previous session. (Must run AFTER loadScene — clearScene wipes
    // the in-memory stacks.)
    status("Restoring undo history...", 0.96f);
    loadUndoHistoryFromDisk();

    status("Ready.", 1.0f);
    m_statusFn = nullptr;   // splash is about to close
    LOG_INFO("Engine initialized successfully!");
}

void Engine::run() {
    LOG_INFO("Entering main loop");

    // Wire the WndProc's live-resize hook so dragging a window edge repaints
    // continuously instead of waiting for the modal resize loop to exit.
    Window::setLiveResizeCallback([this]() { renderOneFrame(); });

    // Reveal the main window now that the editor is fully initialised. Created
    // hidden in Window — see GLFW_VISIBLE hint — so the splash owns the screen
    // through startup.
    m_window->show();

    while (!m_window->shouldClose()) {
        m_window->pollEvents();

        Input::update();
        m_time.update();

        // Fixed-timestep game logic
        while (m_time.shouldTick()) {
            fixedUpdate(Time::FIXED_DT);
            m_time.consumeTick();
        }

        if (m_window->wasResized()) {
            // Defer swapchain recreation while title bar resize is active
            // (glfwSetWindowSize fires many resize events per drag)
            if (!m_titleBar.isResizing()) {
                m_window->resetResizedFlag();
                handleResize();
                continue;
            }
        }

        // Title bar visibility and interaction
        m_titleBar.setVisible(!m_window->isFullscreen());
        bool cursorFree = !Input::isCursorCaptured();
        m_titleBar.update(static_cast<float>(m_window->getWidth()),
                          static_cast<float>(m_window->getHeight()), cursorFree,
                          m_time.getDeltaTime(), m_renderer.getCurrentFrame(),
                          m_window->isFullscreen() ? 0.0f : m_rightDockWidth);
        if (cursorFree) {
            m_titleBar.handleDragResize();
        }

        // Content browser (left dock) — hidden in fullscreen like the title bar
        m_contentBrowser.setVisible(!m_window->isFullscreen());
        m_contentBrowser.update(static_cast<float>(m_window->getWidth()),
                                static_cast<float>(m_window->getHeight()), cursorFree);

        bool  fs   = m_window->isFullscreen();
        float winW = static_cast<float>(m_window->getWidth());
        float winH = static_cast<float>(m_window->getHeight());
        // Apply in-progress dock drag before laying out anything that depends on
        // the width. Drag stops automatically when the user releases LMB.
        if (m_rightDockResizing) {
            bool down = glfwGetMouseButton(m_window->getHandle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (!down) m_rightDockResizing = false;
            else {
                double mx = 0.0, my = 0.0;
                glfwGetCursorPos(m_window->getHandle(), &mx, &my);
                m_rightDockWidth = std::clamp(winW - static_cast<float>(mx),
                                              RIGHT_DOCK_MIN, RIGHT_DOCK_MAX);
                // Drag the edge back from the right of the window → auto-uncollapse.
                m_rightDockCollapsed = false;
            }
        }
        if (m_hierSplitResizing) {
            bool down = glfwGetMouseButton(m_window->getHandle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (!down) m_hierSplitResizing = false;
            else {
                double mx = 0.0, my = 0.0;
                glfwGetCursorPos(m_window->getHandle(), &mx, &my);
                m_hierarchyHeight = static_cast<float>(my) - TitleBar::BAR_HEIGHT;
            }
        }
        float rightDockW = fs                       ? 0.0f
                          : m_rightDockCollapsed     ? RIGHT_DOCK_COLLAPSED_W
                                                     : m_rightDockWidth;
        float midRight   = winW - rightDockW;   // editor + console stop before the right dock

        // Console (bottom dock) — tiles between the left sidebar and the right dock.
        m_console.setVisible(!fs);
        m_console.update(midRight, winH, m_contentBrowser.currentWidth());

        // Central area — the tabbed editor (text files + image/material/binary
        // previews are all tabs in it).
        {
            float left   = m_contentBrowser.currentWidth();
            float top    = TitleBar::BAR_HEIGHT;
            float bottom = m_console.isVisible() ? (winH - m_console.currentHeight()) : winH;
            m_editor.setVisible(!fs);
            m_editor.update(left, top, midRight, bottom);
        }

        // Right dock — Scene Hierarchy (top) over Inspector (bottom).
        {
            m_hierarchy.setVisible(!fs);
            m_inspector.setVisible(!fs && !m_rightDockCollapsed);
            m_hierarchy.setCollapsedRail(m_rightDockCollapsed);
            float dockX   = winW - rightDockW;
            float dockTop = TitleBar::BAR_HEIGHT;
            float availH  = winH - dockTop;
            // When collapsed, the hierarchy panel owns the whole vertical strip
            // and draws a rail. When expanded, the horizontal separator splits
            // it between Hierarchy and Inspector.
            float hierH   = availH;
            float inspTop = dockTop + hierH;
            if (!m_rightDockCollapsed) {
                float maxHier = std::max(HIER_SPLIT_MIN, availH - HIER_SPLIT_MIN);
                hierH = std::clamp(m_hierarchyHeight, HIER_SPLIT_MIN, maxHier);
                m_hierarchyHeight = hierH;
                inspTop = dockTop + hierH;
            }

            // Highlight the right-dock resize edge on both sub-panels while
            // the cursor is over it (or while the user is actively dragging).
            bool edgeHi = cursorFree && overRightDockEdge();
            m_hierarchy.setLeftEdgeHighlight(edgeHi);
            m_inspector.setLeftEdgeHighlight(edgeHi);

            m_hierarchy.update(m_registry, dockX, rightDockW, dockTop, hierH, cursorFree);

            bool dragCompat = false;
            if (m_contentBrowser.isDraggingFile()) {
                std::string ext = std::filesystem::path(m_contentBrowser.draggedPath()).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                dragCompat = (ext == ".mat") || isAssignableImage(ext);
            }
            bool selAnim = false;
            for (const auto& clip : m_animClips) { for (const auto& ch : clip.channels) if (ch.target == m_selectedEntity) { selAnim = true; break; } if (selAnim) break; }
            m_inspector.update(m_registry, m_selectedEntity, dragCompat, dockX, rightDockW, inspTop, winH - inspTop, cursorFree,
                               !m_animClips.empty(), m_animPlaying, selAnim);
        }

        // Translation gizmo on the selected entity + drag handling (viewport).
        updateGizmo(winW, winH, cursorFree);

        // Cursor resolution. Priority: window-edge resize > panel-edge resize >
        // hand-pointer over clickable UI > default arrow. Last writer wins so we
        // pick the highest-priority value first, then fall back through.
        if (cursorFree) {
            int shape = m_titleBar.windowResizeCursor();
            if (shape == 0) {
                if      (m_contentBrowser.overResizeEdge()) shape = GLFW_RESIZE_EW_CURSOR;
                else if (m_console.overResizeEdge())        shape = GLFW_RESIZE_NS_CURSOR;
                else if (overRightDockEdge())               shape = GLFW_RESIZE_EW_CURSOR;
                else if (overHierSplitEdge())               shape = GLFW_RESIZE_NS_CURSOR;
            }
            if (shape == 0) {
                if (m_titleBar.wantsPointerCursor()
                    || m_contentBrowser.wantsPointerCursor()
                    || m_inspector.wantsPointerCursor()
                    || m_console.wantsPointerCursor()
                    || m_hierarchy.wantsPointerCursor())
                    shape = GLFW_HAND_CURSOR;
            }
            m_titleBar.applyCursor(shape);
        }

        // Update UBO with current camera matrices
        updateUniformBuffer(m_renderer.getCurrentFrame());

        bool ok = m_renderer.drawFrame(m_vulkanContext, m_swapchain, m_pipeline,
                                        m_registry, m_descriptors, &m_shadowMap,
                                        m_pointShadows.data(),
                                        m_pointShadowJobs.data(), m_pointShadowJobs.size(),
                                        &m_uiPipeline, &m_titleBar, &m_contentBrowser, &m_console, &m_editor,
                                        &m_imagePipeline, &m_matPreviewPipeline,
                                        &m_hierarchy, &m_inspector, &m_gizmo);
        if (!ok) {
            handleResize();
        }

        // Persist any mutation made this frame. pushUndo flips m_sceneDirty true; the
        // save flushes the POST-mutation scene to scene.scene so the next launch sees
        // exactly the state you left.
        if (m_sceneDirty) {
            saveCurrentScene();
            m_sceneDirty = false;
        }
    }

    LOG_INFO("Main loop ended");
}

void Engine::fixedUpdate(float dt) {
    m_camera.update(dt);

    // Middle-mouse drag orbits the camera around the current selection.
    if (Input::isOrbiting())
        m_camera.orbit(selectionPivot(), Input::getMouseDeltaX(), Input::getMouseDeltaY());

    // Orbit the child entity around its parent
    if (m_orbitEntity != NULL_ENTITY && m_registry.has<TransformComponent>(m_orbitEntity)) {
        m_orbitAngle += dt * 1.5f; // radians per second
        auto& tc = m_registry.get<TransformComponent>(m_orbitEntity);
        float radius = 2.5f;
        tc.position = {radius * cosf(m_orbitAngle), 0.5f, radius * sinf(m_orbitAngle)};
    }

    updateAnimation(dt);                 // glTF transform animation → TransformComponents
    updateLightGizmoScales();            // point-light gizmos: scale ∝ camera distance
    syncLightGizmoColors();              // tint sphere material from LightComponent.color
    TransformSystem::update(m_registry); // recompute world matrices from the updated TRS
    updateSkins();                       // jointMatrix = jointWorld * inverseBind → joint UBOs
}

bool Engine::overRightDockEdge() const {
    if (m_rightDockResizing) return true;       // keep the cursor while dragging
    if (m_window->isFullscreen()) return false;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window->getHandle(), &mx, &my);
    float winW = static_cast<float>(m_window->getWidth());
    float dockX = winW - (m_rightDockCollapsed ? 0.0f : m_rightDockWidth);
    return std::fabs(mx - dockX) <= RIGHT_DOCK_GRAB && my >= TitleBar::BAR_HEIGHT;
}

bool Engine::overHierSplitEdge() const {
    if (m_hierSplitResizing) return true;
    if (m_window->isFullscreen() || m_rightDockCollapsed) return false;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window->getHandle(), &mx, &my);
    float winW = static_cast<float>(m_window->getWidth());
    float dockX = winW - m_rightDockWidth;
    if (mx < dockX) return false;
    // The separator sits at dockTop + m_hierarchyHeight, clamped at layout time.
    // Re-clamp here so the hit zone matches what was drawn.
    float dockTop  = TitleBar::BAR_HEIGHT;
    float availH   = std::max(0.0f, static_cast<float>(m_window->getHeight()) - dockTop);
    float maxHier  = std::max(HIER_SPLIT_MIN, availH - HIER_SPLIT_MIN);
    float splitY   = dockTop + std::clamp(m_hierarchyHeight, HIER_SPLIT_MIN, maxHier);
    return std::fabs(my - splitY) <= HIER_SPLIT_GRAB;
}

glm::vec3 Engine::selectionPivot() {
    // Centre of the **union world-space AABB** across all selected meshes. Averaging
    // per-primitive bbox-centres (the old behaviour) drifted off the visual middle
    // whenever a model was split into multiple prims at different positions — the
    // gladiator's centre sat at its feet, the hair-test rig sat between belt and
    // scalp. Union AABB matches what the eye expects regardless of prim layout.
    // Mirror the gizmo's selection source: hierarchy multi-selection, falling back
    // to the single primary entity.
    std::vector<Entity> ents(m_hierarchy.selection().begin(), m_hierarchy.selection().end());
    if (ents.empty() && m_selectedEntity != NULL_ENTITY) ents.push_back(m_selectedEntity);

    constexpr float INF = std::numeric_limits<float>::infinity();
    glm::vec3 mn{ INF,  INF,  INF};
    glm::vec3 mx{-INF, -INF, -INF};
    bool any = false;

    for (Entity e : ents) {
        if (!m_registry.has<TransformComponent>(e)) continue;
        const auto& tc = m_registry.get<TransformComponent>(e);
        if (m_registry.has<MeshComponent>(e) && m_registry.get<MeshComponent>(e).mesh) {
            const Mesh* mesh = m_registry.get<MeshComponent>(e).mesh;
            const glm::vec3& lmn = mesh->boundsMin();
            const glm::vec3& lmx = mesh->boundsMax();
            // Transform all 8 local-bbox corners and grow the world-space AABB.
            for (int i = 0; i < 8; ++i) {
                glm::vec3 c{ (i & 1) ? lmx.x : lmn.x,
                             (i & 2) ? lmx.y : lmn.y,
                             (i & 4) ? lmx.z : lmn.z };
                glm::vec3 w = glm::vec3(tc.worldMatrix * glm::vec4(c, 1.0f));
                mn = glm::min(mn, w);
                mx = glm::max(mx, w);
                any = true;
            }
        } else {
            // No mesh → use origin as a degenerate 0-size box at that point.
            glm::vec3 w = glm::vec3(tc.worldMatrix[3]);
            mn = glm::min(mn, w);
            mx = glm::max(mx, w);
            any = true;
        }
    }
    if (any) return (mn + mx) * 0.5f;

    // Nothing selected → orbit/zoom around a point in front of the camera.
    return m_camera.getPosition() + m_camera.getFront() * 6.0f;
}

void Engine::ensurePointLightGizmo(Entity e) {
    if (!m_registry.has<LightComponent>(e)) return;
    const auto& lc = m_registry.get<LightComponent>(e);
    if (lc.type != LightComponent::Type::Point) return;
    if (m_registry.has<MeshComponent>(e)) return;   // user-attached mesh wins

    // Sphere mesh + tinted material. The transform stays whatever the entity
    // already had (default scale 1 → radius 0.2 from makeSphere).
    Mesh* mesh = resolveMesh("prim:sphere");
    if (!mesh) return;
    MeshComponent mc{};
    mc.mesh   = mesh;
    mc.source = "prim:sphere";
    m_registry.assign<MeshComponent>(e, mc);

    if (!m_registry.has<MaterialComponent>(e)) {
        MaterialParams params{};
        params.baseColorFactor = glm::vec4(lc.color, 1.0f);
        params.metallic        = 0.0f;
        params.roughness       = 1.0f;
        Descriptors::MaterialMaps maps{};
        Texture* def = m_resourceCache.getDefaultTexture();
        maps.baseColor = def; maps.normal = def; maps.metalRough = def; maps.occlusion = def;
        MaterialComponent mat{};
        mat.texture         = nullptr;
        mat.baseColorFactor = glm::vec4(lc.color, 1.0f);
        mat.metallic        = 0.0f;
        mat.roughness       = 1.0f;
        // Host-visible UBO so syncLightGizmoColors can mutate the tint each
        // frame from LightComponent.color without re-allocating the set.
        mat.descriptorSet   = m_descriptors.allocateMaterialSet(
            m_vulkanContext, maps, params, /*hostVisible=*/true, &mat.materialUBO);
        m_registry.assign<MaterialComponent>(e, mat);
    }
    if (!m_registry.has<TransformComponent>(e)) {
        m_registry.assign<TransformComponent>(e, TransformComponent{});
    }
}

void Engine::attachLightGizmos() {
    auto& pool = m_registry.pool<LightComponent>();
    for (size_t i = 0; i < pool.size(); ++i) ensurePointLightGizmo(pool.getEntity(i));
}

void Engine::rebuildPointShadowSlot(int slot, uint32_t resolution) {
    if (slot < 0 || slot >= MAX_POINT_SHADOWS) return;
    if (m_pointShadows[slot].getResolution() == resolution) return;
    // The cube map is referenced by in-flight command buffers (sampling +
    // potentially shadow render), so wait before destroying it.
    m_renderer.waitIdle(m_vulkanContext.getDevice());
    m_pointShadows[slot].cleanup(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());
    m_pointShadows[slot].init(m_vulkanContext, m_descriptors.getGlobalLayout(), resolution);
    // Transition every face to SHADER_READ_ONLY so the new descriptor's
    // expected layout is valid even before the next render writes to it.
    VkCommandBuffer cmd = m_vulkanContext.beginSingleTimeCommands();
    m_pointShadows[slot].prime(cmd);
    m_vulkanContext.endSingleTimeCommands(cmd);
    // Re-write the cube map array binding with the updated view list.
    VkImageView views[MAX_POINT_SHADOWS]{};
    for (int s = 0; s < MAX_POINT_SHADOWS; ++s) views[s] = m_pointShadows[s].getCubeView();
    m_descriptors.setPointShadowMaps(m_vulkanContext.getDevice(),
                                     views, m_pointShadows[0].getSampler());
    LOG_INFO("Point shadow slot {} rebuilt at {}x{}", slot, resolution, resolution);
}

void Engine::syncLightGizmoColors() {
    auto& lp = m_registry.pool<LightComponent>();
    for (size_t i = 0; i < lp.size(); ++i) {
        const auto& lc = lp[i];
        if (lc.type != LightComponent::Type::Point) continue;
        Entity e = lp.getEntity(i);
        if (!m_registry.has<MaterialComponent>(e)) continue;
        auto& mat = m_registry.get<MaterialComponent>(e);
        if (!mat.materialUBO) continue;
        // Mirror the light's colour onto the component (for swatches / future
        // serialisation) and push the full MaterialParams to the host-visible
        // UBO so the shader sees the new tint this frame.
        mat.baseColorFactor = glm::vec4(lc.color, mat.baseColorFactor.a);
        MaterialParams params{};
        params.baseColorFactor = mat.baseColorFactor;
        params.metallic        = mat.metallic;
        params.roughness       = mat.roughness;
        mat.materialUBO->uploadData(m_vulkanContext.getAllocator(),
                                    &params, sizeof(MaterialParams));
    }
}

void Engine::updateLightGizmoScales() {
    // Keep the point-light gizmo at a constant on-screen pixel size by writing
    // an entity-local scale that cancels perspective foreshortening.
    //   pixelHalf = (meshRadius * scale) * focalLen / dist
    // Solve for scale at a target pixel size:
    //   scale = (pixelHalf * dist * 2 * tan(fov/2)) / (meshRadius * screenH)
    constexpr float MESH_RADIUS   = 0.2f;   // matches makeSphere
    constexpr float TARGET_PIXELS = 24.0f;  // diameter at any distance

    const int winH = m_window ? m_window->getHeight() : 0;
    if (winH <= 0) return;
    const float screenH = static_cast<float>(winH);
    const float halfFov = glm::radians(m_camera.getFov() * 0.5f);
    const glm::vec3 camPos = m_camera.getPosition();
    const float tanHalfFov = std::tan(halfFov);

    auto& lp = m_registry.pool<LightComponent>();
    for (size_t i = 0; i < lp.size(); ++i) {
        if (lp[i].type != LightComponent::Type::Point) continue;
        Entity e = lp.getEntity(i);
        if (!m_registry.has<TransformComponent>(e))   continue;
        auto& tc = m_registry.get<TransformComponent>(e);
        float dist = glm::length(glm::vec3(tc.worldMatrix[3]) - camPos);
        if (dist < 0.01f) dist = 0.01f;
        float s = (TARGET_PIXELS * 0.5f * dist * 2.0f * tanHalfFov) / (MESH_RADIUS * screenH);
        tc.scale = glm::vec3(s);
    }
}

void Engine::frameEntity(Entity e) {
    if (e == NULL_ENTITY || !m_registry.has<TransformComponent>(e)) return;
    const auto& tc = m_registry.get<TransformComponent>(e);

    glm::vec3 mn, mx;
    bool any = false;
    if (m_registry.has<MeshComponent>(e) && m_registry.get<MeshComponent>(e).mesh) {
        const Mesh* mesh = m_registry.get<MeshComponent>(e).mesh;
        const glm::vec3& lmn = mesh->boundsMin();
        const glm::vec3& lmx = mesh->boundsMax();
        for (int i = 0; i < 8; ++i) {
            glm::vec3 c{(i & 1) ? lmx.x : lmn.x,
                        (i & 2) ? lmx.y : lmn.y,
                        (i & 4) ? lmx.z : lmn.z};
            glm::vec3 w = glm::vec3(tc.worldMatrix * glm::vec4(c, 1.0f));
            if (!any) { mn = mx = w; any = true; }
            else      { mn = glm::min(mn, w); mx = glm::max(mx, w); }
        }
    } else {
        // No mesh (Light, Environment, ...) — frame on the entity's origin.
        mn = mx = glm::vec3(tc.worldMatrix[3]);
        any = true;
    }
    if (!any) return;

    const glm::vec3 center = (mn + mx) * 0.5f;
    const float     radius = glm::length(mx - mn) * 0.5f;
    m_camera.frame(center, radius);
}

// Upload per-skin joint matrices for the skinned pipeline. Runs after the
// TransformSystem so joint entities' worldMatrix is current. Per glTF, the
// skinned mesh node's own transform is ignored; joint world matrices carry the
// full placement (including the scene spawn offset on the skeleton root).
void Engine::updateSkins() {
    auto& skinPool = m_registry.pool<SkinComponent>();  // lazily creates an empty pool
    for (size_t i = 0; i < skinPool.size(); i++) {
        SkinComponent& sk = skinPool[i];
        if (!sk.jointBuffer || sk.joints.empty()) continue;

        size_t count = std::min(sk.joints.size(), (size_t)MAX_JOINTS);
        std::array<glm::mat4, MAX_JOINTS> matrices;
        for (size_t j = 0; j < count; j++) {
            glm::mat4 jointWorld(1.0f);
            if (sk.joints[j] != NULL_ENTITY && m_registry.has<TransformComponent>(sk.joints[j]))
                jointWorld = m_registry.get<TransformComponent>(sk.joints[j]).worldMatrix;
            matrices[j] = jointWorld * sk.inverseBind[j];
        }
        sk.jointBuffer->uploadData(m_vulkanContext.getAllocator(), matrices.data(),
                                   sizeof(glm::mat4) * count);
    }
}

void Engine::renderOneFrame() {
    // Re-query the window's current framebuffer size — during a modal Win32
    // resize loop, getWidth/getHeight is updated by the GLFW callback chained
    // off WM_SIZE in our WndProc, which fires synchronously before this
    // function. Skip on minimised (0 × 0) windows.
    int w = m_window->getWidth();
    int h = m_window->getHeight();
    if (w <= 0 || h <= 0) return;

    VkExtent2D current = m_swapchain.getExtent();
    if (static_cast<uint32_t>(w) != current.width
        || static_cast<uint32_t>(h) != current.height) {
        handleResize();
    }

    updateUniformBuffer(m_renderer.getCurrentFrame());

    bool ok = m_renderer.drawFrame(m_vulkanContext, m_swapchain, m_pipeline,
                                    m_registry, m_descriptors, &m_shadowMap,
                                    m_pointShadows.data(),
                                    m_pointShadowJobs.data(), m_pointShadowJobs.size(),
                                    &m_uiPipeline, &m_titleBar, &m_contentBrowser, &m_console, &m_editor,
                                    &m_imagePipeline, &m_matPreviewPipeline,
                                    &m_hierarchy, &m_inspector, &m_gizmo);
    if (!ok) handleResize();
}

void Engine::handleResize() {
    int width = m_window->getWidth();
    int height = m_window->getHeight();
    if (width == 0 || height == 0) return;

    LOG_INFO("Handling resize to {}x{}", width, height);

    m_renderer.waitIdle(m_vulkanContext.getDevice());

    m_swapchain.recreate(m_vulkanContext, width, height);
    m_pipeline.recreate(m_vulkanContext, m_swapchain.getExtent(), m_swapchain.getImageFormat(),
                         VK_FORMAT_R16G16B16A16_SFLOAT,
                         m_swapchain.getDepthBuffer().getFormat(),
                         m_descriptors.getGlobalLayout(), m_descriptors.getMaterialLayout(),
                         m_descriptors.getJointLayout());
    m_uiPipeline.recreate(m_vulkanContext, m_pipeline.getCompositeRenderPass());
    m_imagePipeline.recreate(m_vulkanContext, m_pipeline.getCompositeRenderPass());
    m_matPreviewPipeline.recreate(m_vulkanContext, m_pipeline.getCompositeRenderPass());
    m_renderer.recreateFramebuffers(m_vulkanContext, m_swapchain, m_pipeline);

    m_camera.setAspectRatio(static_cast<float>(width) / static_cast<float>(height));
}

void Engine::updateUniformBuffer(uint32_t currentFrame) {
    UniformBufferObject ubo{};
    ubo.view       = m_camera.getViewMatrix();
    ubo.projection = m_camera.getProjectionMatrix();

    ubo.cameraPosition = glm::vec4(m_camera.getPosition(), 1.0f);

    // Pull sky + ambient from the singleton EnvironmentComponent. Auto-creates
    // the entity on first call so existing scenes (no env entity saved yet)
    // still get sensible defaults from the component's in-class initialisers.
    const EnvironmentComponent& env =
        m_registry.get<EnvironmentComponent>(ensureEnvironmentEntity());
    ubo.ambientColor = glm::vec4(env.ambient, 1.0f);
    ubo.skyTop       = glm::vec4(env.skyTop,     1.0f);
    ubo.skyHorizon   = glm::vec4(env.skyHorizon, env.skyIntensity);
    ubo.skyGround    = glm::vec4(env.skyGround,  1.0f);

    // Sun-shadow light-space matrix: orthographic projection looking along the
    // directional sun's direction. Volume is sized to cover the typical scene (about
    // the gladiator + small surroundings). With GLM_FORCE_DEPTH_ZERO_TO_ONE the proj
    // returns Vulkan-compatible z [0,1]; we Y-flip so shadow UVs match the renderer's
    // convention.
    // Resolve the sun direction. m_sunEntity gets set in buildDemoScene but a
    // .scene file load doesn't tag a specific entity as the sun — fall back to
    // the first directional light in the registry so loaded scenes still cast.
    glm::vec3 sunDir(-0.5f, -1.0f, -0.3f);
    if (m_sunEntity != NULL_ENTITY && m_registry.has<LightComponent>(m_sunEntity)) {
        sunDir = m_registry.get<LightComponent>(m_sunEntity).direction;
    } else {
        auto& lightPool = m_registry.pool<LightComponent>();
        for (size_t i = 0; i < lightPool.size(); ++i) {
            if (lightPool[i].type == LightComponent::Type::Directional) {
                sunDir = lightPool[i].direction;
                break;
            }
        }
    }
    if (glm::length(sunDir) < 1e-4f) sunDir = glm::vec3(-0.5f, -1.0f, -0.3f);
    sunDir = glm::normalize(sunDir);
    // Guard the lookAt: a sun pointing straight down (or up) is parallel to the
    // world-up axis, which makes glm::lookAt produce NaNs (gimbal lock). Pick a
    // perpendicular up axis when that's the case.
    glm::vec3 lightPos = -sunDir * 20.0f;                       // 20 units back along the light
    glm::vec3 upAxis   = (std::fabs(sunDir.y) > 0.999f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                        : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::mat4 lightView = glm::lookAt(lightPos, glm::vec3(0.0f), upAxis);
    glm::mat4 lightProj = glm::ortho(-8.0f, 8.0f, -8.0f, 8.0f, 0.1f, 50.0f);
    lightProj[1][1] *= -1.0f;                                   // Vulkan Y-flip
    ubo.lightSpace = lightProj * lightView;

    // Pack lights from ECS into UBO
    int lightIndex = 0;
    auto& lightPool = m_registry.pool<LightComponent>();
    for (size_t i = 0; i < lightPool.size() && lightIndex < MAX_LIGHTS; i++) {
        Entity entity = lightPool.getEntity(i);
        const LightComponent& lc = lightPool[i];

        GpuLightData& gpu = ubo.lights[lightIndex];
        gpu.colorAndIntensity = glm::vec4(lc.color, lc.intensity);
        // params: x = radius, y = shadowIndex (-1 = none, else 0..MAX_POINT_SHADOWS-1).
        gpu.params = glm::vec4(lc.radius, -1.0f, 0.0f, 0.0f);

        if (lc.type == LightComponent::Type::Directional) {
            gpu.positionAndType = glm::vec4(glm::normalize(lc.direction), 0.0f);
        } else {
            // Point light — read position from TransformComponent
            glm::vec3 pos{0.0f};
            if (m_registry.has<TransformComponent>(entity)) {
                pos = m_registry.get<TransformComponent>(entity).position;
            }
            gpu.positionAndType = glm::vec4(pos, 1.0f);
        }

        lightIndex++;
    }
    ubo.lightCountAndPad = glm::ivec4(lightIndex, 0, 0, 0);

    // ── Point-light shadow jobs ──────────────────────────────────────────────
    // Assign each enabled point light to a cube map slot (first-come, first-served
    // up to MAX_POINT_SHADOWS), compute its 6 per-face view*projection matrices,
    // and stamp the slot index back into the UBO's lights array.
    m_pointShadowJobs.clear();
    int slot = 0;
    int uboIdx = 0;
    for (size_t i = 0; i < lightPool.size() && uboIdx < MAX_LIGHTS; ++i) {
        const auto& lc = lightPool[i];
        if (lc.type == LightComponent::Type::Point && lc.castsShadows && slot < MAX_POINT_SHADOWS) {
            Entity le = lightPool.getEntity(i);
            if (m_registry.has<TransformComponent>(le)) {
                // Resize the slot's cube if the light requests a different tier.
                uint32_t req = static_cast<uint32_t>(std::min(2048,
                    std::max(128, lc.shadowResolution)));
                if (m_pointShadows[slot].getResolution() != req)
                    rebuildPointShadowSlot(slot, req);

                glm::vec3 lp = m_registry.get<TransformComponent>(le).position;
                float farR = std::max(lc.radius, 0.5f);
                PointShadowJob job{};
                job.slot      = slot;
                job.lightPos  = lp;
                job.farRadius = farR;

                static const glm::vec3 dirs[6] = {
                    { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0},
                    { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1},
                };
                static const glm::vec3 ups[6] = {
                    { 0,-1, 0}, { 0,-1, 0}, { 0, 0, 1},
                    { 0, 0,-1}, { 0,-1, 0}, { 0,-1, 0},
                };
                glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, farR);
                for (int f = 0; f < 6; ++f)
                    job.viewProj[f] = proj * glm::lookAt(lp, lp + dirs[f], ups[f]);
                m_pointShadowJobs.push_back(job);

                ubo.lights[uboIdx].params.y = static_cast<float>(slot);
                ++slot;
            }
        }
        ++uboIdx;
    }

    m_descriptors.getUniformBuffer(currentFrame).uploadData(
        m_vulkanContext.getAllocator(), &ubo, sizeof(ubo));
}

Entity Engine::createMeshEntity(Mesh* mesh, Texture* texture,
                                 const glm::vec3& position, const glm::vec3& scale,
                                 const glm::vec4& baseColorFactor,
                                 float metallic, float roughness,
                                 const std::string& source, const std::string& albedoPath,
                                 Texture* normalTex, Texture* metalRoughTex, Texture* occlusionTex,
                                 float alphaCutoff) {
    Entity e = m_registry.createEntity();

    TransformComponent tc{};
    tc.position = position;
    tc.scale = scale;
    m_registry.assign<TransformComponent>(e, tc);

    MeshComponent mc{};
    mc.mesh   = mesh;
    mc.source = source;
    m_registry.assign<MeshComponent>(e, mc);

    MaterialParams params{};
    params.baseColorFactor   = baseColorFactor;
    params.metallic          = metallic;
    params.roughness         = roughness;
    params.hasNormalMap      = normalTex     ? 1.0f : 0.0f;
    params.hasMetalRoughMap  = metalRoughTex ? 1.0f : 0.0f;
    params.alphaCutoff       = alphaCutoff;

    // Absent maps fall back to the default white texture (occlusion white = 1.0;
    // normal/metalRough whites are ignored via the flags above).
    Texture* def = m_resourceCache.getDefaultTexture();
    Descriptors::MaterialMaps maps{};
    maps.baseColor  = texture     ? texture     : def;
    maps.normal     = normalTex   ? normalTex   : def;
    maps.metalRough = metalRoughTex ? metalRoughTex : def;
    maps.occlusion  = occlusionTex ? occlusionTex : def;

    MaterialComponent mat{};
    mat.texture = texture;
    mat.baseColorFactor = baseColorFactor;
    mat.metallic  = metallic;
    mat.roughness = roughness;
    mat.albedoPath = albedoPath;
    mat.albedoName = albedoPath.empty() ? "" : std::filesystem::path(albedoPath).filename().string();
    mat.alphaCutoff = alphaCutoff;
    mat.descriptorSet = m_descriptors.allocateMaterialSet(m_vulkanContext, maps, params);
    m_registry.assign<MaterialComponent>(e, mat);

    return e;
}

// Rebuild a mesh's GPU geometry from its source descriptor. Returns nullptr if the
// source is empty/unrecognised or the file is missing (caller substitutes a cube).
Mesh* Engine::resolveMesh(const std::string& source) {
    if (source.rfind("prim:cube", 0) == 0) {
        std::vector<Vertex> v; std::vector<uint32_t> i; makeCube(v, i);
        return m_resourceCache.getOrCreateMesh(m_vulkanContext, "prim:cube", v, i);
    }
    if (source.rfind("prim:plane", 0) == 0) {
        std::vector<Vertex> v; std::vector<uint32_t> i; makePlane(v, i);
        return m_resourceCache.getOrCreateMesh(m_vulkanContext, "prim:plane", v, i);
    }
    if (source.rfind("prim:sphere", 0) == 0) {
        std::vector<Vertex> v; std::vector<uint32_t> i; makeSphere(v, i);
        return m_resourceCache.getOrCreateMesh(m_vulkanContext, "prim:sphere", v, i);
    }
    if (source.rfind("obj:", 0) == 0) {
        std::string path = source.substr(4);
        if (!std::filesystem::exists(path)) { LOG_WARN("resolveMesh: missing OBJ {}", path); return nullptr; }
        return m_resourceCache.getOrCreateMeshFromOBJ(m_vulkanContext, path);
    }
    if (source.rfind("gltf:", 0) == 0) {
        std::string rest = source.substr(5);
        size_t hash = rest.find_last_of('#');
        std::string path = (hash == std::string::npos) ? rest : rest.substr(0, hash);
        int idx = (hash == std::string::npos) ? 0 : std::atoi(rest.substr(hash + 1).c_str());
        if (!std::filesystem::exists(path)) { LOG_WARN("resolveMesh: missing glTF {}", path); return nullptr; }
        std::vector<GltfMeshData> datas;
        try { datas = GltfLoader::load(path); }
        catch (const std::exception& ex) { LOG_ERROR("resolveMesh: glTF '{}' failed to load: {}", path, ex.what()); return nullptr; }
        if (idx < 0 || idx >= (int)datas.size()) { LOG_WARN("resolveMesh: glTF {} has no primitive {}", path, idx); return nullptr; }
        return m_resourceCache.getOrCreateMesh(m_vulkanContext, path + "#" + std::to_string(idx),
                                               datas[idx].vertices, datas[idx].indices);
    }
    return nullptr;
}

// Rebind the selected entity's material from a dropped/picked asset: a bare image
// (.png/…) becomes the albedo (params unchanged), a .mat supplies albedo + PBR
// params. Allocates a fresh descriptor set and swaps it into the MaterialComponent.
void Engine::assignMaterialToSelected(const std::string& assetPath) {
    namespace fs = std::filesystem;

    if (assetPath.empty())                                    { LOG_WARN("Assign material: no asset"); return; }
    if (m_selectedEntity == NULL_ENTITY)                      { LOG_WARN("Assign material: no entity selected"); return; }
    if (!m_registry.has<MaterialComponent>(m_selectedEntity)) { LOG_WARN("Assign material: entity {} has no material", m_selectedEntity); return; }

    std::string ext = fs::path(assetPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });

    MaterialComponent& mat = m_registry.get<MaterialComponent>(m_selectedEntity);

    // Begin from the entity's current params; the asset overrides what it carries.
    MaterialParams params{};
    params.baseColorFactor = mat.baseColorFactor;
    params.metallic        = mat.metallic;
    params.roughness       = mat.roughness;

    Texture*    tex = mat.texture ? mat.texture : m_resourceCache.getDefaultTexture();
    std::string displayName;
    std::string texPath;          // loadable albedo path persisted in the scene

    auto loadTexture = [&](const std::string& path) -> Texture* {
        try { return m_resourceCache.getOrCreateTexture(m_vulkanContext, path); }
        catch (const std::exception& ex) { LOG_ERROR("Assign material: failed to load '{}': {}", path, ex.what()); return nullptr; }
    };

    if (ext == ".mat") {
        std::ifstream f(assetPath);
        if (!f) { LOG_ERROR("Assign material: cannot open {}", assetPath); return; }
        std::string lineStr, albedoRel;
        while (std::getline(f, lineStr)) {
            std::istringstream ss(lineStr);
            std::string key; ss >> key;
            if      (key == "baseColor") { glm::vec4 c = params.baseColorFactor; ss >> c.r >> c.g >> c.b; if (!(ss >> c.a)) c.a = 1.0f; params.baseColorFactor = c; }
            else if (key == "metallic")  ss >> params.metallic;
            else if (key == "roughness") ss >> params.roughness;
            else if (key == "albedo")    std::getline(ss, albedoRel);    // rest of line = path
        }
        if (size_t s = albedoRel.find_first_not_of(" \t"); s != std::string::npos) albedoRel = albedoRel.substr(s);
        else albedoRel.clear();
        if (!albedoRel.empty()) {
            std::string full = fs::exists(albedoRel) ? albedoRel : (m_projectPath + "/" + albedoRel);
            if (Texture* t = loadTexture(full)) { tex = t; texPath = full; }
        }
        displayName = fs::path(assetPath).filename().string();
    } else if (isAssignableImage(ext)) {
        Texture* t = loadTexture(assetPath);
        if (!t) return;
        tex = t;
        texPath = assetPath;
        displayName = fs::path(assetPath).filename().string();
    } else {
        LOG_WARN("Assign material: '{}' is not a texture or .mat", assetPath);
        return;
    }

    if (!tex) tex = m_resourceCache.getDefaultTexture();

    pushUndo();
    // The current descriptor set may be referenced by in-flight command buffers, so
    // wait before swapping. (A fresh set is allocated; the old one stays in the pool
    // — acceptable for interactive editing, the pool is sized generously.)
    m_renderer.waitIdle(m_vulkanContext.getDevice());
    // Inspector assigns a base-color texture only; PBR maps fall back to default white.
    Texture* def = m_resourceCache.getDefaultTexture();
    Descriptors::MaterialMaps maps{}; maps.baseColor = tex; maps.normal = def; maps.metalRough = def; maps.occlusion = def;
    VkDescriptorSet newSet = m_descriptors.allocateMaterialSet(m_vulkanContext, maps, params);

    mat.texture         = tex;
    mat.baseColorFactor = params.baseColorFactor;
    mat.metallic        = params.metallic;
    mat.roughness       = params.roughness;
    mat.descriptorSet   = newSet;
    mat.albedoName      = displayName;
    mat.albedoPath      = texPath;

    LOG_INFO("Assigned material asset '{}' to entity {}", displayName, m_selectedEntity);
}

// ════════════════════════════════════════════════════════════════════════════
// VIEWPORT PICKING + TRANSLATION GIZMO
// ════════════════════════════════════════════════════════════════════════════

Entity Engine::pickEntity(double mx, double my, float winW, float winH) {
    glm::mat4 vp = m_camera.getProjectionMatrix() * m_camera.getViewMatrix();
    glm::vec3 ro, rd;
    screenToRay(mx, my, vp, winW, winH, ro, rd);

    Entity best = NULL_ENTITY;
    float  bestT = 1e30f;
    auto& meshPool = m_registry.pool<MeshComponent>();
    for (size_t i = 0; i < meshPool.size(); ++i) {
        Entity e = meshPool.getEntity(i);
        const MeshComponent& mc = meshPool[i];
        if (!mc.mesh || !m_registry.has<TransformComponent>(e)) continue;
        const auto& tc = m_registry.get<TransformComponent>(e);
        glm::mat4 invW = glm::inverse(tc.worldMatrix);
        glm::vec3 lo = glm::vec3(invW * glm::vec4(ro, 1.0f));
        glm::vec3 ld = glm::vec3(invW * glm::vec4(rd, 0.0f));   // un-normalized → t stays world-comparable

        // Cheap bbox reject: if the bbox is entirely missed or only entered
        // beyond the current best triangle hit, skip the triangle scan.
        float tBox;
        if (!rayAABB(lo, ld, mc.mesh->boundsMin(), mc.mesh->boundsMax(), tBox)) continue;
        if (tBox >= bestT) continue;

        // Precise per-triangle test — the nearest face wins regardless of
        // bbox size, so a small armor mesh in front of a large body mesh
        // gets picked when the cursor is over the armor's actual geometry.
        float tTri;
        if (!mc.mesh->rayHit(lo, ld, tTri)) continue;
        if (tTri < bestT) { bestT = tTri; best = e; }
    }
    return best;
}

void Engine::onViewportPress(double mx, double my) {
    GLFWwindow* w = m_window->getHandle();

    // 1) Grab a gizmo axis if the click landed near one → drag the whole selection.
    if (m_gizmoVisible && m_selectedEntity != NULL_ENTITY
        && m_registry.has<TransformComponent>(m_selectedEntity)) {
        int axis = -1; float bestD = 9.0f;   // px threshold
        for (int a = 0; a < 3; ++a) {
            float d = distToSeg({(float)mx, (float)my}, m_gizmoOrigin, m_gizmoTip[a]);
            if (d < bestD) { bestD = d; axis = a; }
        }
        if (axis >= 0) {
            glm::vec2 seg = m_gizmoTip[axis] - m_gizmoOrigin;
            float segLen = std::sqrt(seg.x * seg.x + seg.y * seg.y);
            if (segLen > 1.0f) {
                m_gizmoAxis           = axis;
                m_gizmoStartMouse     = {mx, my};
                m_gizmoDragAxisDir    = seg / segLen;
                m_gizmoDragWorldPerPx = m_gizmoWorldLen / segLen;

                // Capture each selected entity's start position; skip entities whose
                // parent is also selected (they move via the parent, no double-move).
                m_gizmoStartPositions.clear();
                std::vector<Entity> targets(m_hierarchy.selection().begin(), m_hierarchy.selection().end());
                if (targets.empty()) targets.push_back(m_selectedEntity);
                std::set<Entity> tset(targets.begin(), targets.end());
                for (Entity e : targets) {
                    if (!m_registry.has<TransformComponent>(e)) continue;
                    Entity par = m_registry.get<TransformComponent>(e).parent;
                    if (par != NULL_ENTITY && tset.count(par)) continue;
                    m_gizmoStartPositions.push_back({e, m_registry.get<TransformComponent>(e).position});
                }

                // Snapshot the pre-drag TRS of every captured entity for the delta-
                // based transform undo. endTransformUndo on release fills in the new
                // values and pushes a single TransformAction.
                std::vector<Entity> capt;
                for (const auto& [e, _] : m_gizmoStartPositions) capt.push_back(e);
                beginTransformUndo(capt);
                return;
            }
        }
    }

    // 2) Otherwise begin a viewport press — resolved as a click (ray-pick) or a
    //    rubber-band marquee on release (see updateGizmo).
    m_vpLeftDown = true;
    m_vpMarquee  = false;
    m_vpPressPos = {mx, my};
    m_vpAdditive = glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)   == GLFW_PRESS || glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT)   == GLFW_PRESS
                || glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(w, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
}

void Engine::updateGizmo(float winW, float winH, bool cursorActive) {
    GLFWwindow* w = m_window->getHandle();
    glm::mat4 vp = m_camera.getProjectionMatrix() * m_camera.getViewMatrix();
    const glm::vec3 axes[3] = {{1,0,0}, {0,1,0}, {0,0,1}};

    // ── Gizmo geometry, anchored at the selection's bounding-box centroid ──
    // (Same pivot the camera orbit/zoom uses — keeps every spatial control consistent.)
    // Shows when there's a selection (single primary OR hierarchy multi-selection).
    bool hasSel = (m_selectedEntity != NULL_ENTITY && m_registry.has<TransformComponent>(m_selectedEntity))
                  || !m_hierarchy.selection().empty();
    bool show = hasSel && !m_window->isFullscreen();
    glm::vec2 origin{0.0f}, tips[3]{};
    if (show) {
        glm::vec3 worldPos = selectionPivot();   // mesh-aware centroid (bbox centres)
        float dist = glm::length(m_camera.getPosition() - worldPos);
        // World-length proportional to distance keeps the projected gizmo at a
        // constant screen size. A previous `max(0.25, ...)` floor made the gizmo
        // grow visibly larger once the user zoomed within ~1.4 world units —
        // remove it so the screen size stays put at any zoom.
        m_gizmoWorldLen = dist * 0.18f;
        bool ok = worldToScreen(worldPos, vp, winW, winH, origin);
        for (int a = 0; a < 3 && ok; ++a)
            ok = worldToScreen(worldPos + axes[a] * m_gizmoWorldLen, vp, winW, winH, tips[a]);
        if (!ok) show = false;
    }
    m_gizmoVisible = show;
    if (show) { m_gizmoOrigin = origin; for (int a = 0; a < 3; ++a) m_gizmoTip[a] = tips[a]; }
    else        m_gizmoAxis = -1;

    int hoverAxis = -1;

    // ── Gizmo drag: move every captured entity by the same world delta ──
    if (m_gizmoAxis >= 0) {
        bool down = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (!down) {
            m_gizmoAxis = -1;
            endTransformUndo();        // commit the transform delta (push to undo stack + persist)
            m_sceneDirty = true;       // drag ended — flush the final position to scene.scene
        } else {
            double cx, cy; glfwGetCursorPos(w, &cx, &cy);
            glm::vec2 delta((float)(cx - m_gizmoStartMouse.x), (float)(cy - m_gizmoStartMouse.y));
            float along = glm::dot(delta, m_gizmoDragAxisDir) * m_gizmoDragWorldPerPx;
            for (auto& [e, sp] : m_gizmoStartPositions)
                if (m_registry.has<TransformComponent>(e))
                    m_registry.get<TransformComponent>(e).position = sp + axes[m_gizmoAxis] * along;
            hoverAxis = m_gizmoAxis;
            m_sceneDirty = true;       // also flag during the drag so partial saves catch crashes
        }
    } else if (show && cursorActive && !m_vpLeftDown) {
        double cx, cy; glfwGetCursorPos(w, &cx, &cy);
        float bestD = 9.0f;
        for (int a = 0; a < 3; ++a) { float d = distToSeg({(float)cx,(float)cy}, origin, tips[a]); if (d < bestD) { bestD = d; hoverAxis = a; } }
    }

    // ── Viewport marquee / click (begun in onViewportPress) ──
    bool marqueeActive = false;
    glm::vec2 mq0{0.0f}, mq1{0.0f};
    if (m_vpLeftDown) {
        bool down = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        double cx, cy; glfwGetCursorPos(w, &cx, &cy);
        if (down) {
            if (!m_vpMarquee && (std::fabs(cx - m_vpPressPos.x) > 4.0 || std::fabs(cy - m_vpPressPos.y) > 4.0))
                m_vpMarquee = true;
            if (m_vpMarquee) {
                marqueeActive = true;
                mq0 = {(float)std::min(m_vpPressPos.x, cx), (float)std::min(m_vpPressPos.y, cy)};
                mq1 = {(float)std::max(m_vpPressPos.x, cx), (float)std::max(m_vpPressPos.y, cy)};
            }
        } else {
            if (m_vpMarquee) {
                float x0 = (float)std::min(m_vpPressPos.x, cx), x1 = (float)std::max(m_vpPressPos.x, cx);
                float y0 = (float)std::min(m_vpPressPos.y, cy), y1 = (float)std::max(m_vpPressPos.y, cy);
                std::vector<Entity> hits;
                if (m_vpAdditive) hits = m_hierarchy.selection();   // add to the existing selection
                auto& mp = m_registry.pool<MeshComponent>();
                for (size_t i = 0; i < mp.size(); ++i) {
                    Entity e = mp.getEntity(i);
                    if (!m_registry.has<TransformComponent>(e)) continue;
                    const MeshComponent& mc = m_registry.get<MeshComponent>(e);
                    if (!mc.mesh) continue;
                    // Project the 8 world-space bbox corners and test the
                    // resulting screen-bbox against the marquee. Testing only
                    // worldMatrix[3] hit every entity whose pivot lives at the
                    // world origin (i.e. most of them).
                    const glm::vec3 mn = mc.mesh->boundsMin();
                    const glm::vec3 mxb = mc.mesh->boundsMax();
                    const glm::mat4& wm = m_registry.get<TransformComponent>(e).worldMatrix;
                    float sx0 =  std::numeric_limits<float>::infinity();
                    float sy0 =  std::numeric_limits<float>::infinity();
                    float sx1 = -std::numeric_limits<float>::infinity();
                    float sy1 = -std::numeric_limits<float>::infinity();
                    bool anyOnScreen = false;
                    for (int c = 0; c < 8; ++c) {
                        glm::vec3 corner((c & 1) ? mxb.x : mn.x,
                                         (c & 2) ? mxb.y : mn.y,
                                         (c & 4) ? mxb.z : mn.z);
                        glm::vec2 s;
                        if (!worldToScreen(glm::vec3(wm * glm::vec4(corner, 1.0f)),
                                           vp, winW, winH, s)) continue;
                        sx0 = std::min(sx0, s.x); sy0 = std::min(sy0, s.y);
                        sx1 = std::max(sx1, s.x); sy1 = std::max(sy1, s.y);
                        anyOnScreen = true;
                    }
                    if (!anyOnScreen) continue;
                    if (sx1 < x0 || sx0 > x1 || sy1 < y0 || sy0 > y1) continue;
                    if (std::find(hits.begin(), hits.end(), e) == hits.end())
                        hits.push_back(e);
                }
                m_hierarchy.setSelection(hits);
            } else {
                Entity hit = pickEntity(cx, cy, winW, winH);
                m_hierarchy.setSelection(hit != NULL_ENTITY ? std::vector<Entity>{hit} : std::vector<Entity>{});
            }
            m_vpLeftDown = false;
            m_vpMarquee  = false;
        }
    }

    // ── Selection outlines: a projected bounding-box wireframe per selected mesh ──
    std::vector<std::pair<glm::vec2, glm::vec2>> outlines;
    for (Entity e : m_hierarchy.selection()) {
        if (!m_registry.has<MeshComponent>(e) || !m_registry.has<TransformComponent>(e)) continue;
        const MeshComponent& mc = m_registry.get<MeshComponent>(e);
        if (!mc.mesh) continue;
        glm::vec3 mn = mc.mesh->boundsMin(), mxb = mc.mesh->boundsMax();
        const glm::mat4& wm = m_registry.get<TransformComponent>(e).worldMatrix;
        glm::vec2 sc[8]; bool okc[8];
        for (int i = 0; i < 8; ++i) {
            glm::vec3 corner((i & 1) ? mxb.x : mn.x, (i & 2) ? mxb.y : mn.y, (i & 4) ? mxb.z : mn.z);
            okc[i] = worldToScreen(glm::vec3(wm * glm::vec4(corner, 1.0f)), vp, winW, winH, sc[i]);
        }
        for (int i = 0; i < 8; ++i)
            for (int b : {1, 2, 4})
                if (!(i & b) && okc[i] && okc[i | b]) outlines.push_back({sc[i], sc[i | b]});
    }

    m_gizmo.update(m_gizmoVisible, m_gizmoOrigin, m_gizmoTip, hoverAxis, marqueeActive, mq0, mq1, outlines);
}

void Engine::buildDemoScene() {
    Texture* defaultTex = m_resourceCache.getDefaultTexture();

    // === Lights ===

    // Directional sun light
    {
        m_sunEntity = m_registry.createEntity();
        LightComponent lc{};
        lc.type      = LightComponent::Type::Directional;
        lc.color     = {1.0f, 1.0f, 0.95f};
        lc.intensity = 1.0f;
        lc.direction = glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f));
        m_registry.assign<LightComponent>(m_sunEntity, lc);
    }

    // Point light 1 — warm, near the cube
    {
        m_pointLightEntity = m_registry.createEntity();
        TransformComponent tc{};
        tc.position = {3.0f, 2.0f, 1.0f};
        m_registry.assign<TransformComponent>(m_pointLightEntity, tc);

        LightComponent lc{};
        lc.type      = LightComponent::Type::Point;
        lc.color     = {1.0f, 0.7f, 0.3f};
        lc.intensity = 2.0f;
        lc.radius    = 8.0f;
        m_registry.assign<LightComponent>(m_pointLightEntity, lc);
    }

    // Point light 2 — cool blue, opposite side
    {
        m_pointLightEntity2 = m_registry.createEntity();
        TransformComponent tc{};
        tc.position = {-3.0f, 1.5f, -2.0f};
        m_registry.assign<TransformComponent>(m_pointLightEntity2, tc);

        LightComponent lc{};
        lc.type      = LightComponent::Type::Point;
        lc.color     = {0.3f, 0.5f, 1.0f};
        lc.intensity = 1.5f;
        lc.radius    = 8.0f;
        m_registry.assign<LightComponent>(m_pointLightEntity2, lc);
    }

    // === Geometry ===

    const std::string cubeSrc = "obj:" + m_projectPath + "/assets/models/cube.obj";

    // 1. OBJ Cube
    Mesh* cubeMesh = resolveMesh(cubeSrc);
    Entity cube = createMeshEntity(cubeMesh, defaultTex, {0.0f, 0.5f, 0.0f}, {1,1,1}, {1,1,1,1}, 0.0f, 0.5f, cubeSrc);

    // 2. Orbiting child cube
    m_orbitEntity = createMeshEntity(cubeMesh, defaultTex, {2.5f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.5f}, {1,1,1,1}, 0.0f, 0.5f, cubeSrc);
    m_registry.get<TransformComponent>(m_orbitEntity).parent = cube;

    // 3. Floor — dark plane so highlights and bloom bleed have something to read
    //    against (light grey floor washed bloom out under the bright sky IBL).
    Mesh* planeMesh = resolveMesh("prim:plane");
    createMeshEntity(planeMesh, defaultTex, {0.0f, 0.0f, 0.0f}, {10.0f, 1.0f, 10.0f},
                     {0.10f, 0.10f, 0.11f, 1.0f}, 0.0f, 0.8f, "prim:plane");

    // (Note: the demo no longer auto-loads test glTFs like DamagedHelmet/BoxTextured —
    // those were a legacy "load any model in assets/" path that, after the gladiator
    // was added as a dedicated mount in init(), spawned the DamagedHelmet right inside
    // the gladiator's torso and looked like a chrome bubble in the chest. The dedicated
    // glTF mount in Engine::init handles the hero asset; ad-hoc test models can be
    // dragged in via the content browser.)

    // Add the sphere gizmo to every point light so they're visible in the viewport.
    attachLightGizmos();

    LOG_INFO("Demo scene built with {} entities", m_registry.pool<MeshComponent>().size());
}

Entity Engine::loadGltfScene(const std::string& filepath, const glm::vec3& position, float rootScale) {
    GltfImport imp;
    try {
        imp = GltfLoader::loadImport(filepath);   // throws if the file/buffers/URIs are missing
    } catch (const std::exception& ex) {
        LOG_ERROR("loadGltfScene: '{}' failed to load: {}", filepath, ex.what());
        return NULL_ENTITY;                        // abort the spawn instead of crashing
    }
    std::string baseDir = filepath.substr(0, filepath.find_last_of("/\\") + 1);

    // Build one entity from a flat primitive (geometry + base-color material).
    auto buildPrim = [&](int prim, const glm::vec3& pos, const glm::quat& rot, const glm::vec3& scl) -> Entity {
        const GltfMeshData& md = imp.primitives[prim];
        std::string key = filepath + "#" + std::to_string(prim);
        Mesh* mesh = m_resourceCache.getOrCreateMesh(m_vulkanContext, key, md.vertices, md.indices);
        Texture* tex = m_resourceCache.getDefaultTexture();
        std::string texPath;
        if (!md.baseColorTextureURI.empty()) {
            std::string p = baseDir + md.baseColorTextureURI;
            if (std::filesystem::exists(p)) { tex = m_resourceCache.getOrCreateTexture(m_vulkanContext, p); texPath = p; }
        }
        // PBR data maps — loaded as linear (srgb=false), nullptr when absent/missing.
        auto loadLinear = [&](const std::string& uri) -> Texture* {
            if (uri.empty()) return nullptr;
            std::string p = baseDir + uri;
            if (!std::filesystem::exists(p)) return nullptr;
            try { return m_resourceCache.getOrCreateTexture(m_vulkanContext, p, /*srgb=*/false); }
            catch (const std::exception& ex) { LOG_WARN("PBR map '{}': {}", p, ex.what()); return nullptr; }
        };
        Texture* nrmTex = loadLinear(md.normalTextureURI);
        Texture* mrTex  = loadLinear(md.metalRoughTextureURI);
        Texture* aoTex  = loadLinear(md.occlusionTextureURI);
        Entity e = createMeshEntity(mesh, tex, pos, scl, md.baseColorFactor, md.metallicFactor, md.roughnessFactor,
                                    "gltf:" + key, texPath, nrmTex, mrTex, aoTex, md.alphaCutoff);
        m_registry.get<TransformComponent>(e).rotation = rot;
        // Persist the PBR map paths so the full material round-trips through scene
        // save/load and undo (which serialize via writeEntities/readEntities).
        auto& bmat = m_registry.get<MaterialComponent>(e);
        if (nrmTex) bmat.normalPath     = baseDir + md.normalTextureURI;
        if (mrTex)  bmat.metalRoughPath = baseDir + md.metalRoughTextureURI;
        if (aoTex)  bmat.occlusionPath  = baseDir + md.occlusionTextureURI;
        return e;
    };

    // One entity per glTF node (roots offset to the spawn point; children keep local TRS).
    std::vector<Entity> nodeEntity(imp.nodes.size(), NULL_ENTITY);
    // Extra primitive entities (beyond the first) for each node — they share the
    // node's skin so every material chunk of a multi-material skinned mesh deforms.
    std::vector<std::vector<Entity>> nodeExtraPrims(imp.nodes.size());

    // Splash progress — track per-node so the bar advances visibly while the
    // model's mesh + texture uploads run. Only the startup path turns this on
    // (loadProgressActive); spawning a model into a live editor stays silent.
    bool localScope = false;
    if (m_statusFn && !m_loadProgressActive) {
        m_loadProgressActive = true;
        m_loadProgressIndex  = 0;
        m_loadProgressTotal  = (int)imp.nodes.size();
        m_loadProgressBase   = 0.75f;
        m_loadProgressSpan   = 0.20f;
        localScope = true;
    }

    Entity first = NULL_ENTITY;
    for (size_t i = 0; i < imp.nodes.size(); ++i) {
        const GltfNode& n = imp.nodes[i];
        if (m_loadProgressActive) {
            ++m_loadProgressIndex;
            std::string nm = n.name.empty() ? ("node " + std::to_string(i)) : n.name;
            reportLoadProgress("Loading: " + nm);
        }
        // Root nodes are offset to the spawn point and uniformly scaled (rootScale).
        // Children inherit the root scale via the transform hierarchy, so only roots
        // are touched — this keeps multi-root rigs (mesh + armature) consistent.
        glm::vec3 t   = n.translation;
        glm::vec3 scl = n.scale;
        if (n.parent < 0) { t = position + n.translation * rootScale; scl = n.scale * rootScale; }

        Entity e;
        if (n.firstPrim >= 0 && n.primCount > 0) {
            e = buildPrim(n.firstPrim, t, n.rotation, scl);
            for (int p = 1; p < n.primCount; ++p) {       // extra primitives → child entities
                Entity c = buildPrim(n.firstPrim + p, glm::vec3(0.0f), glm::quat(1,0,0,0), glm::vec3(1.0f));
                m_registry.get<TransformComponent>(c).parent = e;
                nodeExtraPrims[i].push_back(c);
            }
        } else {
            e = m_registry.createEntity();                 // mesh-less node (empty / pivot)
            TransformComponent tc{}; tc.position = t; tc.rotation = n.rotation; tc.scale = scl;
            m_registry.assign<TransformComponent>(e, tc);
        }
        nodeEntity[i] = e;
        if (first == NULL_ENTITY) first = e;
    }
    for (size_t i = 0; i < imp.nodes.size(); ++i) {        // parent links
        int par = imp.nodes[i].parent;
        if (par >= 0 && nodeEntity[i] != NULL_ENTITY && nodeEntity[par] != NULL_ENTITY)
            m_registry.get<TransformComponent>(nodeEntity[i]).parent = nodeEntity[par];
    }

    // Attach SkinComponents: a node that references a skin gets the joint entity
    // list + inverse-bind matrices + a joint-matrix UBO/descriptor set (set 2).
    for (size_t i = 0; i < imp.nodes.size(); ++i) {
        int skinIdx = imp.nodes[i].skin;
        if (skinIdx < 0 || skinIdx >= (int)imp.skins.size()) continue;
        if (nodeEntity[i] == NULL_ENTITY) continue;
        const GltfSkin& gs = imp.skins[skinIdx];
        if (gs.jointNodes.empty()) continue;

        if ((int)gs.jointNodes.size() > MAX_JOINTS)
            LOG_WARN("Skin on node '{}' has {} joints > MAX_JOINTS ({}). Joints beyond the cap "
                     "will deform incorrectly (exploded geometry). Reduce the rig's bone count or raise MAX_JOINTS.",
                     imp.nodes[i].name, (int)gs.jointNodes.size(), MAX_JOINTS);

        SkinComponent sk;
        sk.joints.reserve(gs.jointNodes.size());
        for (int jn : gs.jointNodes)
            sk.joints.push_back((jn >= 0 && jn < (int)nodeEntity.size()) ? nodeEntity[jn] : NULL_ENTITY);
        sk.inverseBind = gs.inverseBind;

        auto ja = m_descriptors.allocateJointSet(m_vulkanContext.getDevice(),
                                                 m_vulkanContext.getAllocator());
        sk.jointSet    = ja.set;
        sk.jointBuffer = ja.buffer;
        m_registry.assign<SkinComponent>(nodeEntity[i], std::move(sk));

        // A multi-material mesh is split into one entity per primitive; every
        // primitive of a skinned mesh must be skinned too, or the extra material
        // chunks render unskinned at the node transform (hovering above the body).
        // They share the one joint set/buffer; jointBuffer=nullptr means updateSkins
        // skips them (the owner above does the single upload).
        for (Entity c : nodeExtraPrims[i]) {
            SkinComponent share;
            share.jointSet    = ja.set;
            share.jointBuffer = nullptr;
            m_registry.assign<SkinComponent>(c, std::move(share));
        }
        LOG_INFO("Attached skin: {} joints on node '{}' ({} extra prims)",
                 (int)gs.jointNodes.size(), imp.nodes[i].name, (int)nodeExtraPrims[i].size());
    }

    // Retarget animation channels (node index → entity) into runtime clips.
    for (const GltfAnim& a : imp.animations) {
        AnimClipRT clip; clip.name = a.name; clip.duration = a.duration;
        for (const GltfAnimChannel& ch : a.channels) {
            if (ch.targetNode < 0 || ch.targetNode >= (int)nodeEntity.size()) continue;
            Entity tgt = nodeEntity[ch.targetNode];
            if (tgt == NULL_ENTITY) continue;
            AnimChannelRT c;
            c.target = tgt; c.path = ch.path; c.interp = ch.interp;
            c.times = ch.times; c.values = ch.values;
            clip.channels.push_back(std::move(c));
        }
        if (!clip.channels.empty()) {
            LOG_INFO("Animation '{}': {:.2f}s, {} channels", clip.name, clip.duration, (int)clip.channels.size());
            m_animClips.push_back(std::move(clip));
        }
    }
    if (localScope) m_loadProgressActive = false;
    return first;
}

// Advance + sample all playing clips into their target TransformComponents.
void Engine::updateAnimation(float dt) {
    if (!m_animPlaying) return;
    for (AnimClipRT& clip : m_animClips) {
        if (clip.duration <= 0.0f || clip.channels.empty()) continue;
        clip.time += dt;
        if (clip.loop) { if (clip.time > clip.duration) clip.time = std::fmod(clip.time, clip.duration); }
        else            clip.time = std::min(clip.time, clip.duration);
        float t = clip.time;

        for (AnimChannelRT& ch : clip.channels) {
            if (ch.times.empty() || !m_registry.has<TransformComponent>(ch.target)) continue;
            glm::vec4 v;
            if (t <= ch.times.front())      v = ch.values.front();
            else if (t >= ch.times.back())  v = ch.values.back();
            else {
                size_t i = 0;
                while (i + 1 < ch.times.size() && ch.times[i + 1] < t) ++i;
                float t0 = ch.times[i], t1 = ch.times[i + 1];
                float a = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
                if (ch.interp == 1) {                       // step
                    v = ch.values[i];
                } else if (ch.path == 1) {                  // rotation → slerp
                    glm::quat q0(ch.values[i].w, ch.values[i].x, ch.values[i].y, ch.values[i].z);
                    glm::quat q1(ch.values[i+1].w, ch.values[i+1].x, ch.values[i+1].y, ch.values[i+1].z);
                    glm::quat q = glm::slerp(q0, q1, a);
                    v = {q.x, q.y, q.z, q.w};
                } else {                                    // translation/scale → lerp
                    v = glm::mix(ch.values[i], ch.values[i + 1], a);
                }
            }
            auto& tc = m_registry.get<TransformComponent>(ch.target);
            if      (ch.path == 0) tc.position = glm::vec3(v);
            else if (ch.path == 2) tc.scale    = glm::vec3(v);
            else                   tc.rotation = glm::quat(v.w, v.x, v.y, v.z);
        }
    }
}

// Add a model file to the scene as one or more entities (one per glTF primitive),
// selecting the first. Used by dragging a model from the content browser.
void Engine::spawnModel(const std::string& path, const glm::vec3& position) {
    std::string ext = toLowerExt(path);
    Entity created = NULL_ENTITY;

    if (ext == ".obj") {
        std::string src = "obj:" + path;
        Mesh* mesh = resolveMesh(src);
        if (!mesh) { LOG_ERROR("spawnModel: failed to load {}", path); return; }
        created = createMeshEntity(mesh, m_resourceCache.getDefaultTexture(), position,
                                   {1,1,1}, {1,1,1,1}, 0.0f, 0.5f, src);
    } else if (ext == ".gltf" || ext == ".glb") {
        created = loadGltfScene(path, position);
    } else {
        LOG_WARN("spawnModel: '{}' is not a model (.obj/.gltf/.glb)", path);
        return;
    }

    if (created != NULL_ENTITY) {
        pushSpawnAction({created});            // delta-based: undo just destroys this entity
        m_hierarchy.setSelection({created});   // fires onSelect → m_selectedEntity
        LOG_INFO("Spawned '{}' as entity {}", std::filesystem::path(path).filename().string(), created);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// ENTITY CLIPBOARD COMMANDS (operate on the Scene Hierarchy's selection)
// ════════════════════════════════════════════════════════════════════════════

void Engine::deleteSelection() {
    const auto& sel = m_hierarchy.selection();
    std::vector<Entity> ents(sel.begin(), sel.end());
    if (ents.empty() && m_selectedEntity != NULL_ENTITY) ents.push_back(m_selectedEntity);
    if (ents.empty()) return;

    m_renderer.waitIdle(m_vulkanContext.getDevice());   // descriptor sets may be in flight
    pushDeleteAction(ents);            // single multi-entity action so one Ctrl+Z restores them all
    for (Entity e : ents) {
        if (e == m_orbitEntity)       m_orbitEntity       = NULL_ENTITY;
        if (e == m_sunEntity)         m_sunEntity         = NULL_ENTITY;
        if (e == m_pointLightEntity)  m_pointLightEntity  = NULL_ENTITY;
        if (e == m_pointLightEntity2) m_pointLightEntity2 = NULL_ENTITY;
        m_registry.destroyEntity(e);   // material set leaks until the next scene clear (pool sized for it)
    }
    m_selectedEntity = NULL_ENTITY;
    m_hierarchy.setSelection({});
    LOG_INFO("Deleted {} entity(ies)", ents.size());
}

void Engine::copySelection() {
    const auto& sel = m_hierarchy.selection();
    if (sel.empty()) return;
    std::vector<Entity> ents(sel.begin(), sel.end());
    std::ostringstream os;
    writeEntities(os, ents);
    m_entityClipboard = os.str();
    LOG_INFO("Copied {} entity(ies)", ents.size());
}

void Engine::cutSelection() {
    copySelection();
    deleteSelection();
}

void Engine::pasteClipboard() {
    if (m_entityClipboard.empty()) return;
    std::istringstream is(m_entityClipboard);
    std::vector<Entity> created = readEntities(is, glm::vec3(0.5f, 0.0f, 0.5f));  // nudge so copies don't overlap
    if (!created.empty()) {
        pushSpawnAction(created);          // delta — one undo removes all pasted entities
        m_hierarchy.setSelection(created);
        LOG_INFO("Pasted {} entity(ies)", created.size());
    }
}

void Engine::duplicateSelection() {
    const auto& sel = m_hierarchy.selection();
    if (sel.empty()) return;
    std::vector<Entity> ents(sel.begin(), sel.end());
    std::ostringstream os;
    writeEntities(os, ents);
    std::istringstream is(os.str());
    std::vector<Entity> created = readEntities(is, glm::vec3(0.5f, 0.0f, 0.5f));  // separate from the clipboard
    if (!created.empty()) {
        pushSpawnAction(created);          // delta — one undo removes all duplicates
        // Drop the duplicates into the hierarchy order right after their
        // sources so the new rows appear next to what was copied, not at the
        // bottom of the list.
        m_hierarchy.insertAfterSources(ents, created);
        m_hierarchy.setSelection(created);
        LOG_INFO("Duplicated {} entity(ies)", created.size());
    }
}

// ════════════════════════════════════════════════════════════════════════════
// SCENE SAVE / LOAD
// ════════════════════════════════════════════════════════════════════════════

void Engine::clearScene() {
    m_renderer.waitIdle(m_vulkanContext.getDevice());

    // Collect every entity (union of the component pools), then destroy them.
    std::set<Entity> ents;
    auto gather = [&](const auto& pool) { for (size_t i = 0; i < pool.size(); ++i) ents.insert(pool.getEntity(i)); };
    gather(m_registry.pool<TransformComponent>());
    gather(m_registry.pool<MeshComponent>());
    gather(m_registry.pool<MaterialComponent>());
    gather(m_registry.pool<LightComponent>());
    gather(m_registry.pool<EnvironmentComponent>());
    for (Entity e : ents) m_registry.destroyEntity(e);

    // All entities destroyed → reset the id counter so a reload reuses the same low
    // ids instead of climbing (Mesh 3 stays Mesh 3 across reloads/undo).
    m_registry.resetEntityIds();

    // Free all material descriptor sets so reloads don't leak/exhaust the pool.
    m_descriptors.resetMaterials(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());

    m_animClips.clear();   // clips reference now-destroyed entities

    // Stale demo handles → null so fixedUpdate's guards skip them.
    m_selectedEntity    = NULL_ENTITY;
    m_orbitEntity       = NULL_ENTITY;
    m_sunEntity         = NULL_ENTITY;
    m_pointLightEntity  = NULL_ENTITY;
    m_pointLightEntity2 = NULL_ENTITY;
    m_environmentEntity = NULL_ENTITY;
}

Entity Engine::ensureEnvironmentEntity() {
    // Already have it (and the registry still knows about it) — short-circuit.
    if (m_environmentEntity != NULL_ENTITY
        && m_registry.has<EnvironmentComponent>(m_environmentEntity)) {
        return m_environmentEntity;
    }
    // Find an existing one in the registry (scene load path).
    auto& pool = m_registry.pool<EnvironmentComponent>();
    if (pool.size() > 0) {
        m_environmentEntity = pool.getEntity(0);
        return m_environmentEntity;
    }
    // None present → create with defaults. Singleton: never more than one.
    m_environmentEntity = m_registry.createEntity();
    m_registry.assign<EnvironmentComponent>(m_environmentEntity, EnvironmentComponent{});
    return m_environmentEntity;
}

// Write entity blocks (no header) for the given entities — shared by saveScene
// (whole scene → file) and copySelection (selected → clipboard string).
void Engine::writeEntities(std::ostream& os, const std::vector<Entity>& ents) {
    for (Entity e : ents) {
        os << "entity " << e << "\n";
        // Skip Mesh + Material for light entities — they're auto-generated as
        // a viewport gizmo by ensurePointLightGizmo and would otherwise bloat
        // the scene file with the sphere primitive every save/load cycle.
        const bool isLight = m_registry.has<LightComponent>(e);
        if (m_registry.has<TransformComponent>(e)) {
            const auto& t = m_registry.get<TransformComponent>(e);
            long parent = (t.parent == NULL_ENTITY) ? -1 : (long)t.parent;
            // Point lights rewrite scale every frame for the camera-relative
            // gizmo; persist 1 so reloads start from a clean baseline.
            const glm::vec3 wScale = isLight ? glm::vec3(1.0f) : t.scale;
            os << "transform "
               << t.position.x << ' ' << t.position.y << ' ' << t.position.z << ' '
               << t.rotation.x << ' ' << t.rotation.y << ' ' << t.rotation.z << ' ' << t.rotation.w << ' '
               << wScale.x << ' ' << wScale.y << ' ' << wScale.z << ' ' << parent << "\n";
        }
        if (!isLight && m_registry.has<MeshComponent>(e))
            os << "mesh " << m_registry.get<MeshComponent>(e).source << "\n";
        if (!isLight && m_registry.has<MaterialComponent>(e)) {
            const auto& m = m_registry.get<MaterialComponent>(e);
            os << "material "
               << m.baseColorFactor.r << ' ' << m.baseColorFactor.g << ' '
               << m.baseColorFactor.b << ' ' << m.baseColorFactor.a << ' '
               << m.metallic << ' ' << m.roughness << ' '
               << (m.albedoPath.empty() ? "-" : m.albedoPath) << "\n";
            // PBR data maps + alpha cutoff on their own keyed lines (each path read via
            // getline, so paths with spaces survive). Absent maps are simply omitted.
            if (!m.normalPath.empty())     os << "nmap " << m.normalPath << "\n";
            if (!m.metalRoughPath.empty()) os << "rmap " << m.metalRoughPath << "\n";
            if (!m.occlusionPath.empty())  os << "omap " << m.occlusionPath << "\n";
            if (m.alphaCutoff > 0.0f)      os << "acut " << m.alphaCutoff << "\n";
        }
        if (m_registry.has<LightComponent>(e)) {
            const auto& l = m_registry.get<LightComponent>(e);
            os << "light " << (int)l.type << ' '
               << l.color.r << ' ' << l.color.g << ' ' << l.color.b << ' ' << l.intensity << ' '
               << l.direction.x << ' ' << l.direction.y << ' ' << l.direction.z << ' ' << l.radius << ' '
               << (l.castsShadows ? 1 : 0) << ' ' << l.shadowResolution << "\n";
        }
        if (m_registry.has<EnvironmentComponent>(e)) {
            const auto& v = m_registry.get<EnvironmentComponent>(e);
            os << "env "
               << v.skyTop.r     << ' ' << v.skyTop.g     << ' ' << v.skyTop.b     << ' '
               << v.skyHorizon.r << ' ' << v.skyHorizon.g << ' ' << v.skyHorizon.b << ' '
               << v.skyGround.r  << ' ' << v.skyGround.g  << ' ' << v.skyGround.b  << ' '
               << v.skyIntensity << ' '
               << v.ambient.r    << ' ' << v.ambient.g    << ' ' << v.ambient.b    << ' '
               << v.bloomThreshold << ' ' << v.bloomKnee << ' ' << v.bloomStrength << ' '
               << (int)v.tonemapper << ' ' << v.exposure << "\n";
        }
    }
}

// Parse entity blocks, creating entities (does NOT clear). posOffset is added to
// every position (paste nudges duplicates). Parents are remapped within this batch
// (a parent not present in the batch leaves the child unparented). Returns new ids.
std::vector<Entity> Engine::readEntities(std::istream& is, const glm::vec3& posOffset) {
    namespace fs = std::filesystem;
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };

    std::unordered_map<uint32_t, Entity>     idMap;
    std::vector<std::pair<Entity, uint32_t>> parentFix;
    std::vector<Entity>                      created;
    Entity cur = NULL_ENTITY;

    // A material spans several lines (material + optional nmap/rmap/omap/acut), so we
    // buffer it and build the descriptor once the entity's block ends (next "entity"
    // or EOF) — that way every PBR map is present before allocateMaterialSet.
    struct PendingMat {
        bool has = false;
        glm::vec4 bc{1, 1, 1, 1};
        float me = 0.0f, ro = 0.5f, alpha = 0.0f;
        std::string albedo, normal, metalrough, occlusion;
    };
    PendingMat pend;
    Entity     pendEntity = NULL_ENTITY;

    auto flushMat = [&]() {
        if (!pend.has || pendEntity == NULL_ENTITY) { pend = PendingMat{}; return; }
        auto loadTex = [&](const std::string& path, bool srgb) -> Texture* {
            if (path.empty() || path == "-") return nullptr;
            std::string full = fs::exists(path) ? path : (m_projectPath + "/" + path);
            try { return m_resourceCache.getOrCreateTexture(m_vulkanContext, full, srgb); }
            catch (const std::exception& ex) { LOG_ERROR("readEntities: texture '{}': {}", full, ex.what()); return nullptr; }
        };
        Texture* def       = m_resourceCache.getDefaultTexture();
        Texture* albedoTex = loadTex(pend.albedo,     /*srgb=*/true);
        Texture* nrmTex    = loadTex(pend.normal,     /*srgb=*/false);
        Texture* mrTex     = loadTex(pend.metalrough, /*srgb=*/false);
        Texture* aoTex     = loadTex(pend.occlusion,  /*srgb=*/false);

        MaterialParams params{};
        params.baseColorFactor  = pend.bc;
        params.metallic         = pend.me;
        params.roughness        = pend.ro;
        params.hasNormalMap     = nrmTex ? 1.0f : 0.0f;
        params.hasMetalRoughMap = mrTex  ? 1.0f : 0.0f;
        params.alphaCutoff      = pend.alpha;

        Descriptors::MaterialMaps maps{};
        maps.baseColor  = albedoTex ? albedoTex : def;
        maps.normal     = nrmTex    ? nrmTex    : def;
        maps.metalRough = mrTex     ? mrTex     : def;
        maps.occlusion  = aoTex     ? aoTex     : def;

        MaterialComponent mat{};
        mat.texture         = maps.baseColor;
        mat.baseColorFactor = pend.bc;
        mat.metallic        = pend.me;
        mat.roughness       = pend.ro;
        mat.alphaCutoff     = pend.alpha;
        mat.albedoPath      = albedoTex ? pend.albedo : "";
        mat.albedoName      = albedoTex ? fs::path(pend.albedo).filename().string() : "";
        mat.normalPath      = nrmTex ? pend.normal     : "";
        mat.metalRoughPath  = mrTex  ? pend.metalrough : "";
        mat.occlusionPath   = aoTex  ? pend.occlusion  : "";
        mat.descriptorSet   = m_descriptors.allocateMaterialSet(m_vulkanContext, maps, params);
        m_registry.assign<MaterialComponent>(pendEntity, mat);
        pend = PendingMat{};
        pendEntity = NULL_ENTITY;
    };

    std::string line;
    while (std::getline(is, line)) {
        std::string ln = trim(line);
        if (ln.empty() || ln[0] == '#') continue;
        std::istringstream ss(ln);
        std::string key; ss >> key;

        if (key == "entity") {
            flushMat();                       // finish the previous entity's material
            uint32_t oldId = 0; ss >> oldId;
            cur = m_registry.createEntity();
            idMap[oldId] = cur;
            created.push_back(cur);
            // Splash tick — generic label until a "mesh" or "light" line refines it.
            if (m_loadProgressActive) {
                ++m_loadProgressIndex;
                reportLoadProgress("Loading entity " + std::to_string(m_loadProgressIndex)
                                   + " of " + std::to_string(m_loadProgressTotal));
            }
        } else if (cur == NULL_ENTITY) {
            continue;
        } else if (key == "transform") {
            TransformComponent t{};
            float qx, qy, qz, qw; long parent = -1;
            ss >> t.position.x >> t.position.y >> t.position.z
               >> qx >> qy >> qz >> qw
               >> t.scale.x >> t.scale.y >> t.scale.z >> parent;
            t.position += posOffset;
            t.rotation = glm::quat(qw, qx, qy, qz);
            m_registry.assign<TransformComponent>(cur, t);
            if (parent >= 0) parentFix.emplace_back(cur, (uint32_t)parent);
        } else if (key == "mesh") {
            std::string src; std::getline(ss, src); src = trim(src);
            // Refine the splash status with the actual mesh name (file part only).
            if (m_loadProgressActive) {
                std::string nm = src;
                size_t slash = nm.find_last_of("/\\");
                if (slash != std::string::npos) nm = nm.substr(slash + 1);
                reportLoadProgress("Loading mesh: " + nm);
            }
            Mesh* mesh = resolveMesh(src);
            if (!mesh) mesh = resolveMesh("prim:cube");   // fallback so it stays visible
            MeshComponent mc{}; mc.mesh = mesh; mc.source = src;
            m_registry.assign<MeshComponent>(cur, mc);
        } else if (key == "material") {
            pend = PendingMat{};
            pend.has = true; pendEntity = cur;
            ss >> pend.bc.r >> pend.bc.g >> pend.bc.b >> pend.bc.a >> pend.me >> pend.ro;
            std::string albedo; std::getline(ss, albedo); pend.albedo = trim(albedo);
        } else if (key == "nmap") {
            std::string p; std::getline(ss, p); pend.normal     = trim(p);
        } else if (key == "rmap") {
            std::string p; std::getline(ss, p); pend.metalrough = trim(p);
        } else if (key == "omap") {
            std::string p; std::getline(ss, p); pend.occlusion  = trim(p);
        } else if (key == "acut") {
            ss >> pend.alpha;
        } else if (key == "light") {
            int type = 0; LightComponent lc{};
            ss >> type >> lc.color.r >> lc.color.g >> lc.color.b >> lc.intensity
               >> lc.direction.x >> lc.direction.y >> lc.direction.z >> lc.radius;
            int castsShadows = 0;
            if (ss >> castsShadows) lc.castsShadows = (castsShadows != 0);
            int shadowRes = 0;
            if (ss >> shadowRes) lc.shadowResolution = shadowRes;
            lc.type = (type == 1) ? LightComponent::Type::Point : LightComponent::Type::Directional;
            m_registry.assign<LightComponent>(cur, lc);
            if (m_loadProgressActive)
                reportLoadProgress(lc.type == LightComponent::Type::Directional
                                   ? "Adding directional light" : "Adding point light");
        } else if (key == "env") {
            EnvironmentComponent v{};
            int tm = 0;
            ss >> v.skyTop.r     >> v.skyTop.g     >> v.skyTop.b
               >> v.skyHorizon.r >> v.skyHorizon.g >> v.skyHorizon.b
               >> v.skyGround.r  >> v.skyGround.g  >> v.skyGround.b
               >> v.skyIntensity
               >> v.ambient.r    >> v.ambient.g    >> v.ambient.b
               >> v.bloomThreshold >> v.bloomKnee >> v.bloomStrength
               >> tm >> v.exposure;
            v.tonemapper = static_cast<EnvironmentComponent::Tonemapper>(tm);
            m_registry.assign<EnvironmentComponent>(cur, v);
            m_environmentEntity = cur;
        } else if (key == "clip") {
            float dur = 0.0f; int loop = 1; ss >> dur >> loop;
            AnimClipRT clip; clip.duration = dur; clip.loop = (loop != 0);
            m_animClips.push_back(std::move(clip));
        } else if (key == "channel") {
            if (m_animClips.empty()) continue;
            uint32_t tid = 0; int path = 0, interp = 0; size_t keys = 0;
            ss >> tid >> path >> interp >> keys;
            AnimChannelRT c; c.path = path; c.interp = interp;
            auto it = idMap.find(tid);
            c.target = (it != idMap.end()) ? it->second : NULL_ENTITY;
            c.times.resize(keys);
            c.values.resize(keys);
            for (size_t k = 0; k < keys; ++k)
                ss >> c.times[k] >> c.values[k].x >> c.values[k].y >> c.values[k].z >> c.values[k].w;
            if (c.target != NULL_ENTITY) m_animClips.back().channels.push_back(std::move(c));
        }
    }
    flushMat();   // build the last entity's material (no trailing "entity" to trigger it)

    for (auto& [e, parentOld] : parentFix) {
        auto it = idMap.find(parentOld);
        if (it != idMap.end() && m_registry.has<TransformComponent>(e))
            m_registry.get<TransformComponent>(e).parent = it->second;
    }
    // Light entities serialize without their gizmo (writeEntities skips Mesh +
    // Material when LightComponent is present), so re-attach here for every
    // load / paste / duplicate / undo-spawn path that funnels through us.
    for (Entity e : created) ensurePointLightGizmo(e);
    return created;
}

std::vector<Entity> Engine::sceneEntities() {
    std::set<Entity> set;
    auto gather = [&](const auto& pool) { for (size_t i = 0; i < pool.size(); ++i) set.insert(pool.getEntity(i)); };
    gather(m_registry.pool<TransformComponent>());
    gather(m_registry.pool<MeshComponent>());
    gather(m_registry.pool<LightComponent>());
    gather(m_registry.pool<EnvironmentComponent>());
    return {set.begin(), set.end()};
}

std::string Engine::snapshotScene() {
    std::ostringstream os;
    writeEntities(os, sceneEntities());
    return os.str();
}

void Engine::restoreScene(const std::string& snap) {
    clearScene();
    std::istringstream is(snap);
    readEntities(is, glm::vec3(0.0f));
    m_selectedEntity = NULL_ENTITY;
    m_hierarchy.setSelection({});
}

// ── Action-based undo: delta files for transform / spawn / delete; snapshot fallback ──
std::string Engine::nextActionPath(const std::string& ext) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%08zu%s", m_historyCounter++, ext.c_str());
    return m_projectPath + "/scenes/.history/" + buf;
}

void Engine::writeActionFile(const UndoAction& a) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(a.path).parent_path(), ec);
    std::ofstream f(a.path);
    if (!f) { LOG_WARN("writeActionFile: cannot open {}", a.path); return; }

    if (a.kind == UndoAction::Kind::Snapshot) {
        // Snapshot: file content IS the snapshot text. (Filename suffix .snap.)
        // The action's `serialized` field carries the snapshot text for this kind.
        f << a.serialized;
        return;
    }

    // Delta formats use a header line "transform"/"spawn"/"delete" so loadUndoHistoryFromDisk
    // can dispatch correctly when reading back later.
    if (a.kind == UndoAction::Kind::Transform) {
        f << "transform " << a.oldTransforms.size() << "\n";
        for (size_t i = 0; i < a.oldTransforms.size(); ++i) {
            const auto& o = a.oldTransforms[i];
            const auto& n = a.newTransforms[i];
            f << o.e << ' '
              << o.pos.x << ' ' << o.pos.y << ' ' << o.pos.z << ' '
              << o.rot.x << ' ' << o.rot.y << ' ' << o.rot.z << ' ' << o.rot.w << ' '
              << o.scl.x << ' ' << o.scl.y << ' ' << o.scl.z << ' '
              << n.pos.x << ' ' << n.pos.y << ' ' << n.pos.z << ' '
              << n.rot.x << ' ' << n.rot.y << ' ' << n.rot.z << ' ' << n.rot.w << ' '
              << n.scl.x << ' ' << n.scl.y << ' ' << n.scl.z << "\n";
        }
    } else if (a.kind == UndoAction::Kind::Spawn || a.kind == UndoAction::Kind::Delete) {
        f << (a.kind == UndoAction::Kind::Spawn ? "spawn " : "delete ") << a.entities.size();
        for (Entity e : a.entities) f << ' ' << e;
        f << '\n' << a.serialized;
    } else if (a.kind == UndoAction::Kind::Color) {
        f << "color " << a.colorEntity << ' ' << (int)a.colorTarget << ' '
          << a.oldColor.r << ' ' << a.oldColor.g << ' ' << a.oldColor.b << ' ' << a.oldColor.a << ' '
          << a.newColor.r << ' ' << a.newColor.g << ' ' << a.newColor.b << ' ' << a.newColor.a << "\n";
    } else if (a.kind == UndoAction::Kind::Scalar) {
        f << "scalar " << a.scalarEntity << ' ' << (int)a.scalarTarget << ' '
          << a.oldScalar << ' ' << a.newScalar << "\n";
    }
}

bool Engine::readActionFile(UndoAction& out, const std::string& path) const {
    std::ifstream f(path);
    if (!f) return false;
    out.path = path;

    std::string ext = std::filesystem::path(path).extension().string();
    if (ext == ".snap") {
        out.kind = UndoAction::Kind::Snapshot;
        std::ostringstream ss; ss << f.rdbuf();
        out.serialized = ss.str();
        return true;
    }
    // .delta — parse header
    std::string header; size_t count = 0;
    f >> header >> count;
    if (header == "transform") {
        out.kind = UndoAction::Kind::Transform;
        out.oldTransforms.resize(count);
        out.newTransforms.resize(count);
        for (size_t i = 0; i < count; i++) {
            auto& o = out.oldTransforms[i]; auto& n = out.newTransforms[i];
            uint32_t eid;
            f >> eid
              >> o.pos.x >> o.pos.y >> o.pos.z
              >> o.rot.x >> o.rot.y >> o.rot.z >> o.rot.w
              >> o.scl.x >> o.scl.y >> o.scl.z
              >> n.pos.x >> n.pos.y >> n.pos.z
              >> n.rot.x >> n.rot.y >> n.rot.z >> n.rot.w
              >> n.scl.x >> n.scl.y >> n.scl.z;
            o.e = static_cast<Entity>(eid);
            n.e = static_cast<Entity>(eid);
        }
        return true;
    }
    if (header == "color") {
        // "color" uses `count` as the entity id (no per-entry array — always 1).
        out.kind        = UndoAction::Kind::Color;
        out.colorEntity = static_cast<Entity>(count);
        int targetIdx = 0;
        f >> targetIdx
          >> out.oldColor.r >> out.oldColor.g >> out.oldColor.b >> out.oldColor.a
          >> out.newColor.r >> out.newColor.g >> out.newColor.b >> out.newColor.a;
        out.colorTarget = static_cast<Inspector::PickerField>(targetIdx);
        return true;
    }
    if (header == "scalar") {
        out.kind         = UndoAction::Kind::Scalar;
        out.scalarEntity = static_cast<Entity>(count);
        int targetIdx = 0;
        f >> targetIdx >> out.oldScalar >> out.newScalar;
        out.scalarTarget = static_cast<Inspector::ScalarField>(targetIdx);
        return true;
    }
    if (header == "spawn" || header == "delete") {
        out.kind = (header == "spawn") ? UndoAction::Kind::Spawn : UndoAction::Kind::Delete;
        out.entities.reserve(count);
        for (size_t i = 0; i < count; i++) {
            uint32_t eid = 0; f >> eid;
            out.entities.push_back(static_cast<Entity>(eid));
        }
        std::string rest; std::getline(f, rest);   // consume newline
        std::ostringstream ss; ss << f.rdbuf();
        out.serialized = ss.str();
        return true;
    }
    return false;
}

void Engine::loadUndoHistoryFromDisk() {
    namespace fs = std::filesystem;
    std::string dir = m_projectPath + "/scenes/.history";
    if (!fs::exists(dir)) return;

    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().string();
        if (ext == ".snap" || ext == ".delta") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());

    m_undoStack.clear();
    for (const auto& p : files) {
        UndoAction a;
        if (readActionFile(a, p.string())) m_undoStack.push_back(std::move(a));
    }

    if (!files.empty()) {
        // Resume counter past the highest existing file so future writes never collide.
        try { m_historyCounter = std::stoull(files.back().stem().string()) + 1; }
        catch (...) { m_historyCounter = files.size(); }
    }
    LOG_INFO("Undo history loaded: {} action(s) from {}", m_undoStack.size(), dir);
}

// ── Apply / revert ──
void Engine::applyAction(UndoAction& a, bool forward) {
    switch (a.kind) {
        case UndoAction::Kind::Transform: {
            const auto& vec = forward ? a.newTransforms : a.oldTransforms;
            for (const auto& t : vec) {
                if (!m_registry.has<TransformComponent>(t.e)) continue;
                auto& tc = m_registry.get<TransformComponent>(t.e);
                tc.position = t.pos;
                tc.rotation = t.rot;
                tc.scale    = t.scl;
            }
            break;
        }
        case UndoAction::Kind::Spawn: {
            if (forward) {
                // Redo a spawn: re-instantiate the entities from serialized form.
                std::istringstream is(a.serialized);
                a.entities = readEntities(is, glm::vec3(0.0f));   // track new ids
            } else {
                // Undo a spawn: remove all spawned entities.
                m_renderer.waitIdle(m_vulkanContext.getDevice());
                for (Entity e : a.entities) {
                    if (e == NULL_ENTITY) continue;
                    m_registry.destroyEntity(e);
                    if (e == m_selectedEntity) m_selectedEntity = NULL_ENTITY;
                }
            }
            break;
        }
        case UndoAction::Kind::Delete: {
            if (forward) {
                // Redo a delete: remove the entities.
                m_renderer.waitIdle(m_vulkanContext.getDevice());
                for (Entity e : a.entities) {
                    if (e == NULL_ENTITY) continue;
                    m_registry.destroyEntity(e);
                    if (e == m_selectedEntity) m_selectedEntity = NULL_ENTITY;
                }
            } else {
                // Undo a delete: re-instantiate all of them.
                std::istringstream is(a.serialized);
                a.entities = readEntities(is, glm::vec3(0.0f));
            }
            break;
        }
        case UndoAction::Kind::Snapshot: {
            restoreScene(a.serialized);
            break;
        }
        case UndoAction::Kind::Color: {
            const glm::vec4& c = forward ? a.newColor : a.oldColor;
            writeColorTarget(a.colorEntity, a.colorTarget, c);
            break;
        }
        case UndoAction::Kind::Scalar: {
            float v = forward ? a.newScalar : a.oldScalar;
            writeScalarTarget(a.scalarEntity, a.scalarTarget, v);
            break;
        }
    }
    m_sceneDirty = true;
}

void Engine::pushAction(UndoAction&& a) {
    // Persist the action; assign the path so disk and in-memory always match.
    std::string ext = (a.kind == UndoAction::Kind::Snapshot) ? ".snap" : ".delta";
    a.path = nextActionPath(ext);
    writeActionFile(a);
    m_undoStack.push_back(std::move(a));

    // Evict oldest if we've crossed the cap.
    while (m_undoStack.size() > MAX_HISTORY) {
        std::error_code ec; std::filesystem::remove(m_undoStack.front().path, ec);
        m_undoStack.erase(m_undoStack.begin());
    }

    // Fresh action invalidates redo — delete redo files too.
    for (const auto& r : m_redoStack) { std::error_code ec; std::filesystem::remove(r.path, ec); }
    m_redoStack.clear();

    m_sceneDirty = true;
}

// ── Snapshot fallback (used by paste/duplicate/material assign and as a safety net) ──
void Engine::pushUndo() {
    UndoAction a;
    a.kind       = UndoAction::Kind::Snapshot;
    a.serialized = snapshotScene();
    pushAction(std::move(a));
}

// ── Transform deltas: capture old at begin, new at end ──
void Engine::beginTransformUndo(const std::vector<Entity>& ents) {
    m_pendingTransformOld.clear();
    for (Entity e : ents) {
        if (!m_registry.has<TransformComponent>(e)) continue;
        const auto& tc = m_registry.get<TransformComponent>(e);
        m_pendingTransformOld.push_back({e, tc.position, tc.rotation, tc.scale});
    }
    m_pendingTransformActive = !m_pendingTransformOld.empty();
}

void Engine::endTransformUndo() {
    if (!m_pendingTransformActive) return;
    m_pendingTransformActive = false;

    // Compose new transforms by re-reading current values.
    UndoAction a;
    a.kind = UndoAction::Kind::Transform;
    a.oldTransforms = std::move(m_pendingTransformOld);
    a.newTransforms.reserve(a.oldTransforms.size());

    bool anyChanged = false;
    for (const auto& o : a.oldTransforms) {
        if (!m_registry.has<TransformComponent>(o.e)) { a.newTransforms.push_back(o); continue; }
        const auto& tc = m_registry.get<TransformComponent>(o.e);
        UndoAction::TRS n{o.e, tc.position, tc.rotation, tc.scale};
        if (n.pos != o.pos || n.rot != o.rot || n.scl != o.scl) anyChanged = true;
        a.newTransforms.push_back(n);
    }
    // If nothing actually moved, don't pollute the history with a no-op.
    if (!anyChanged) return;

    pushAction(std::move(a));
}

// ── Color delta: per-field old/new RGBA for one component ──
glm::vec4 Engine::readColorTarget(Entity e, Inspector::PickerField t) const {
    using PF = Inspector::PickerField;
    if (e == NULL_ENTITY) return glm::vec4(1.0f);
    switch (t) {
        case PF::MaterialBase:
            if (m_registry.has<MaterialComponent>(e))
                return m_registry.get<MaterialComponent>(e).baseColorFactor;
            break;
        case PF::LightColor:
            if (m_registry.has<LightComponent>(e))
                return glm::vec4(m_registry.get<LightComponent>(e).color, 1.0f);
            break;
        case PF::EnvSkyTop:
            if (m_registry.has<EnvironmentComponent>(e))
                return glm::vec4(m_registry.get<EnvironmentComponent>(e).skyTop, 1.0f);
            break;
        case PF::EnvSkyHorizon:
            if (m_registry.has<EnvironmentComponent>(e))
                return glm::vec4(m_registry.get<EnvironmentComponent>(e).skyHorizon, 1.0f);
            break;
        case PF::EnvSkyGround:
            if (m_registry.has<EnvironmentComponent>(e))
                return glm::vec4(m_registry.get<EnvironmentComponent>(e).skyGround, 1.0f);
            break;
        case PF::EnvAmbient:
            if (m_registry.has<EnvironmentComponent>(e))
                return glm::vec4(m_registry.get<EnvironmentComponent>(e).ambient, 1.0f);
            break;
    }
    return glm::vec4(1.0f);
}

void Engine::writeColorTarget(Entity e, Inspector::PickerField t, const glm::vec4& c) {
    using PF = Inspector::PickerField;
    if (e == NULL_ENTITY) return;
    switch (t) {
        case PF::MaterialBase:
            if (m_registry.has<MaterialComponent>(e))
                m_registry.get<MaterialComponent>(e).baseColorFactor = c;
            break;
        case PF::LightColor:
            if (m_registry.has<LightComponent>(e))
                m_registry.get<LightComponent>(e).color = glm::vec3(c);
            break;
        case PF::EnvSkyTop:
            if (m_registry.has<EnvironmentComponent>(e))
                m_registry.get<EnvironmentComponent>(e).skyTop = glm::vec3(c);
            break;
        case PF::EnvSkyHorizon:
            if (m_registry.has<EnvironmentComponent>(e))
                m_registry.get<EnvironmentComponent>(e).skyHorizon = glm::vec3(c);
            break;
        case PF::EnvSkyGround:
            if (m_registry.has<EnvironmentComponent>(e))
                m_registry.get<EnvironmentComponent>(e).skyGround = glm::vec3(c);
            break;
        case PF::EnvAmbient:
            if (m_registry.has<EnvironmentComponent>(e))
                m_registry.get<EnvironmentComponent>(e).ambient = glm::vec3(c);
            break;
    }
}

void Engine::beginColorUndo(Entity e, Inspector::PickerField t) {
    m_pendingColorEntity = e;
    m_pendingColorTarget = t;
    m_pendingColorOld    = readColorTarget(e, t);
    m_pendingColorActive = true;
}

void Engine::endColorUndo(Entity e, Inspector::PickerField t) {
    if (!m_pendingColorActive) return;
    m_pendingColorActive = false;
    if (e != m_pendingColorEntity || t != m_pendingColorTarget) return;

    glm::vec4 now = readColorTarget(e, t);
    if (now == m_pendingColorOld) return;   // no-op (dragged back to start)

    UndoAction a;
    a.kind        = UndoAction::Kind::Color;
    a.colorEntity = e;
    a.colorTarget = t;
    a.oldColor    = m_pendingColorOld;
    a.newColor    = now;
    pushAction(std::move(a));
}

// ── Scalar delta: per-field old/new float for one component field ──
float Engine::readScalarTarget(Entity e, Inspector::ScalarField t) const {
    using SF = Inspector::ScalarField;
    if (e == NULL_ENTITY) return 0.0f;
    switch (t) {
        case SF::LightIntensity:
            return m_registry.has<LightComponent>(e) ? m_registry.get<LightComponent>(e).intensity : 0.0f;
        case SF::LightRadius:
            return m_registry.has<LightComponent>(e) ? m_registry.get<LightComponent>(e).radius    : 0.0f;
        case SF::LightCastsShadows:
            return (m_registry.has<LightComponent>(e) && m_registry.get<LightComponent>(e).castsShadows) ? 1.0f : 0.0f;
        case SF::LightShadowResolution:
            return m_registry.has<LightComponent>(e) ? (float)m_registry.get<LightComponent>(e).shadowResolution : 512.0f;
        default: break;
    }
    if (!m_registry.has<EnvironmentComponent>(e)) return 0.0f;
    const auto& ec = m_registry.get<EnvironmentComponent>(e);
    switch (t) {
        case SF::EnvSkyIntensity:   return ec.skyIntensity;
        case SF::EnvBloomThreshold: return ec.bloomThreshold;
        case SF::EnvBloomKnee:      return ec.bloomKnee;
        case SF::EnvBloomStrength:  return ec.bloomStrength;
        case SF::EnvExposure:       return ec.exposure;
        case SF::EnvTonemapper:     return static_cast<float>(static_cast<uint32_t>(ec.tonemapper));
        default: break;
    }
    return 0.0f;
}

void Engine::writeScalarTarget(Entity e, Inspector::ScalarField t, float v) {
    using SF = Inspector::ScalarField;
    if (e == NULL_ENTITY) return;
    switch (t) {
        case SF::LightIntensity:
            if (m_registry.has<LightComponent>(e)) m_registry.get<LightComponent>(e).intensity = v;
            return;
        case SF::LightRadius:
            if (m_registry.has<LightComponent>(e)) m_registry.get<LightComponent>(e).radius    = v;
            return;
        case SF::LightCastsShadows:
            if (m_registry.has<LightComponent>(e))
                m_registry.get<LightComponent>(e).castsShadows = (std::round(v) != 0.0f);
            return;
        case SF::LightShadowResolution:
            if (m_registry.has<LightComponent>(e))
                m_registry.get<LightComponent>(e).shadowResolution = (int)std::round(v);
            return;
        default: break;
    }
    if (!m_registry.has<EnvironmentComponent>(e)) return;
    auto& ec = m_registry.get<EnvironmentComponent>(e);
    switch (t) {
        case SF::EnvSkyIntensity:   ec.skyIntensity   = v; break;
        case SF::EnvBloomThreshold: ec.bloomThreshold = v; break;
        case SF::EnvBloomKnee:      ec.bloomKnee      = v; break;
        case SF::EnvBloomStrength:  ec.bloomStrength  = v; break;
        case SF::EnvExposure:       ec.exposure       = v; break;
        case SF::EnvTonemapper: {
            int idx = std::min(2, std::max(0, (int)std::round(v)));
            ec.tonemapper = static_cast<EnvironmentComponent::Tonemapper>(idx);
            break;
        }
        default: break;
    }
}

void Engine::beginScalarUndo(Entity e, Inspector::ScalarField t) {
    m_pendingScalarEntity = e;
    m_pendingScalarTarget = t;
    m_pendingScalarOld    = readScalarTarget(e, t);
    m_pendingScalarActive = true;
}

void Engine::endScalarUndo(Entity e, Inspector::ScalarField t) {
    if (!m_pendingScalarActive) return;
    m_pendingScalarActive = false;
    if (e != m_pendingScalarEntity || t != m_pendingScalarTarget) return;

    float now = readScalarTarget(e, t);
    if (now == m_pendingScalarOld) return;

    UndoAction a;
    a.kind         = UndoAction::Kind::Scalar;
    a.scalarEntity = e;
    a.scalarTarget = t;
    a.oldScalar    = m_pendingScalarOld;
    a.newScalar    = now;
    pushAction(std::move(a));
}

// ── Spawn / Delete deltas: per-entity instead of full-scene snapshot ──
void Engine::pushSpawnAction(const std::vector<Entity>& created) {
    if (created.empty()) return;
    UndoAction a;
    a.kind     = UndoAction::Kind::Spawn;
    a.entities = created;
    std::ostringstream os;
    writeEntities(os, created);
    a.serialized = os.str();
    pushAction(std::move(a));
}

void Engine::pushDeleteAction(const std::vector<Entity>& toDelete) {
    if (toDelete.empty()) return;
    UndoAction a;
    a.kind     = UndoAction::Kind::Delete;
    a.entities = toDelete;
    std::ostringstream os;
    writeEntities(os, toDelete);
    a.serialized = os.str();
    pushAction(std::move(a));
}

void Engine::undo() {
    if (m_undoStack.empty()) { LOG_INFO("Nothing to undo"); return; }

    UndoAction a = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    applyAction(a, /*forward=*/false);

    // Move to redo stack (keep the file — redo will re-apply it).
    m_redoStack.push_back(std::move(a));
    LOG_INFO("Undo ({} more available)", m_undoStack.size());
}

void Engine::redo() {
    if (m_redoStack.empty()) { LOG_INFO("Nothing to redo"); return; }

    UndoAction a = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    applyAction(a, /*forward=*/true);
    m_undoStack.push_back(std::move(a));
    LOG_INFO("Redo ({} more available)", m_redoStack.size());
}

void Engine::loadEditorPrefs() {
    std::ifstream f(m_projectPath + "/editor.prefs");
    if (!f) return;
    std::string key;
    while (f >> key) {
        if      (key == "rightDockCollapsed") { int v = 0; f >> v; m_rightDockCollapsed = (v != 0); }
        else if (key == "rightDockWidth")     f >> m_rightDockWidth;
        else if (key == "hierarchyHeight")    f >> m_hierarchyHeight;
        else if (key == "browserExpanded")    { int v = 0; f >> v; m_contentBrowser.setExpanded(v != 0); }
        else if (key == "browserWidth")       { float v = 0; f >> v; m_contentBrowser.setPanelWidth(v); }
        else if (key == "consoleExpanded")    { int v = 0; f >> v; m_console.setExpanded(v != 0); }
        else if (key == "consoleHeight")      { float v = 0; f >> v; m_console.setPanelHeight(v); }
        else { std::string skip; std::getline(f, skip); }
    }
}

void Engine::saveEditorPrefs() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(m_projectPath, ec);
    std::ofstream f(m_projectPath + "/editor.prefs");
    if (!f) return;
    f << "rightDockCollapsed " << (m_rightDockCollapsed ? 1 : 0) << "\n"
      << "rightDockWidth "     << m_rightDockWidth                << "\n"
      << "hierarchyHeight "    << m_hierarchyHeight               << "\n"
      << "browserExpanded "    << (m_contentBrowser.isExpanded() ? 1 : 0) << "\n"
      << "browserWidth "       << m_contentBrowser.panelWidth()   << "\n"
      << "consoleExpanded "    << (m_console.isExpanded() ? 1 : 0) << "\n"
      << "consoleHeight "      << m_console.panelHeight()         << "\n";
}

void Engine::saveCurrentScene() {
    // Save to the loaded/last-saved scene, or a default the first time. Rescan so a
    // newly-created .scene appears in the content browser. Bound to FILE > Save Scene
    // and Ctrl+S (when the text editor isn't focused).
    std::string scenePath = m_currentScenePath.empty()
        ? (m_projectPath + "/scenes/scene.scene") : m_currentScenePath;
    saveScene(scenePath);
    m_contentBrowser.setProject(m_projectPath);
}

void Engine::saveScene(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    std::ofstream f(path);
    if (!f) { LOG_ERROR("saveScene: cannot write {}", path); return; }
    f << "# Nyx scene v1\n";

    std::vector<Entity> ents = sceneEntities();
    writeEntities(f, ents);

    // Animation clips (so playback survives reload). Channels reference entity ids.
    for (const AnimClipRT& clip : m_animClips) {
        f << "clip " << clip.duration << ' ' << (clip.loop ? 1 : 0) << "\n";
        for (const AnimChannelRT& ch : clip.channels) {
            f << "channel " << ch.target << ' ' << ch.path << ' ' << ch.interp << ' ' << ch.times.size();
            for (size_t k = 0; k < ch.times.size(); ++k)
                f << ' ' << ch.times[k] << ' ' << ch.values[k].x << ' ' << ch.values[k].y
                  << ' ' << ch.values[k].z << ' ' << ch.values[k].w;
            f << "\n";
        }
    }

    m_currentScenePath = path;
    LOG_INFO("Saved scene to {} ({} entities, {} clips)", path, ents.size(), m_animClips.size());
}

void Engine::loadScene(const std::string& path) {
    std::ifstream f(path);
    if (!f) { LOG_ERROR("loadScene: cannot open {}", path); return; }

    clearScene();

    // Pre-count entities so the splash bar can advance proportionally as each
    // one is instantiated below. Only the startup-load path passes a status fn;
    // runtime reloads keep the existing progress range alone.
    if (m_statusFn) {
        std::ifstream cf(path);
        std::string line;
        int total = 0;
        while (std::getline(cf, line)) {
            // First non-space token == "entity" → new entity block.
            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (line.compare(i, 7, "entity ") == 0 || line.compare(i, 7, "entity\t") == 0
                || (line.size() - i == 6 && line.compare(i, 6, "entity") == 0))
                ++total;
        }
        m_loadProgressActive = true;
        m_loadProgressIndex  = 0;
        m_loadProgressTotal  = total;
        m_loadProgressBase   = 0.65f;
        m_loadProgressSpan   = 0.30f;
    }

    std::vector<Entity> created = readEntities(f, glm::vec3(0.0f));

    m_loadProgressActive = false;

    // Scene file no longer stores the auto-generated light gizmo (see writeEntities);
    // re-attach the sphere mesh + material so loaded point lights show up in view.
    attachLightGizmos();

    m_selectedEntity = NULL_ENTITY;
    m_hierarchy.setSelection({});
    m_currentScenePath = path;
    LOG_INFO("Loaded scene {} ({} entities)", path, created.size());
}

void Engine::reportLoadProgress(const std::string& label) {
    if (!m_loadProgressActive || !m_statusFn) return;
    float frac = m_loadProgressTotal > 0
               ? (float)m_loadProgressIndex / (float)m_loadProgressTotal
               : 1.0f;
    if (frac > 1.0f) frac = 1.0f;
    m_statusFn(label, m_loadProgressBase + m_loadProgressSpan * frac);
}

} // namespace Nyx
