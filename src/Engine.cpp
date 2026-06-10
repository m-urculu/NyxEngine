#include "Engine.h"
#include "Logger.h"
#include "Input.h"
#include "ui/Splash.h"
#include "renderer/Vertex.h"
#include "renderer/UniformTypes.h"
#include "renderer/Buffer.h"
#include "renderer/ObjLoader.h"
#include "renderer/GltfLoader.h"
#include "renderer/AssimpLoader.h"
#include "renderer/Texture.h"
#include "renderer/Mesh.h"
#include "ecs/components/TransformComponent.h"
#include "ecs/components/MeshComponent.h"
#include "ecs/components/NameComponent.h"
#include "ecs/components/MaterialComponent.h"
#include "ecs/components/OcclusionOverlay.h"
#include "ecs/components/LightComponent.h"
#include "ecs/components/SkinComponent.h"
#include "ecs/components/EnvironmentComponent.h"
#include "ecs/systems/TransformSystem.h"
#ifdef NYX_HAS_PLANET
#include "procgen/Planet.h"   // project-side planet generator (prim:planet mesh source)
#endif
#include <cstdlib>   // std::getenv (dev planet self-test hook)

#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>
#include <stb_image_write.h>
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
#include <random>

#ifdef _WIN32
#  include <windows.h>
#endif

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
    static const std::set<std::string> m = {".obj", ".gltf", ".glb", ".fbx", ".dae"};
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
        // CCW-outward winding so the geometric normal matches the declared face
        // normal and back-face culling keeps the outside visible. The naive
        // {0,1,2, 2,3,0} order inverts it (faces cull away, you see the inside of
        // the cube — looks unlit) — same fix makePlane needed. See makePlane above.
        idx.insert(idx.end(), {base, base+2, base+1, base+2, base, base+3});
    }
}
// A simple third-person avatar: a capsule (sphere split + stretched) with a head
// region and a dark "face" on the +Z front so its facing reads. Feet sit at y=0.
void makeCharacter(std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    const int   LATS = 12, LONS = 16;
    const float R = 0.45f;            // body radius
    const float H = 1.10f;            // straight cylinder section between the caps
    constexpr float kPi = 3.14159265358979323846f;
    const glm::vec3 body{0.30f, 0.55f, 0.95f};   // blue
    const glm::vec3 head{0.95f, 0.85f, 0.70f};   // tan
    const glm::vec3 face{0.12f, 0.12f, 0.18f};   // dark visor (front of the head)
    const float yShift = R + H * 0.5f;           // lift so the lowest point sits at y=0
    for (int i = 0; i <= LATS; ++i) {
        float t = (float)i / LATS;
        float phi = kPi * t;
        float sp = std::sin(phi), cp = std::cos(phi);
        float yOff = (cp >= 0.0f ? H * 0.5f : -H * 0.5f);     // split the sphere → capsule
        for (int j = 0; j <= LONS; ++j) {
            float u = (float)j / LONS;
            float th = 2.0f * kPi * u;
            glm::vec3 n{sp * std::cos(th), cp, sp * std::sin(th)};
            glm::vec3 col = body;
            if (cp > 0.45f) col = head;                       // head cap
            if (cp > 0.25f && n.z > 0.45f) col = face;         // front-of-head visor
            glm::vec3 pos = R * n;
            pos.y += yOff + yShift;
            v.push_back({pos, glm::normalize(n), col, {u, t}});
        }
    }
    for (int i = 0; i < LATS; ++i)
        for (int j = 0; j < LONS; ++j) {
            uint32_t a = (uint32_t)(i * (LONS + 1) + j);
            uint32_t b = (uint32_t)((i + 1) * (LONS + 1) + j);
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
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

// Last-project persistence. Path: %APPDATA%\Nyx\last_project.txt on Windows.
// Read at startup so the editor reopens the last-used project; written on
// init success and on every Engine::switchProject.
std::filesystem::path nyxLastProjectFilePath() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !*appdata) return {};
    return std::filesystem::path(appdata) / "Nyx" / "last_project.txt";
#else
    return {};
#endif
}
std::string readLastProjectPath() {
    auto p = nyxLastProjectFilePath();
    if (p.empty() || !std::filesystem::exists(p)) return {};
    std::ifstream f(p);
    std::string line;
    if (!std::getline(f, line)) return {};
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                              line.back() == ' '  || line.back() == '\t'))
        line.pop_back();
    return line;
}
void writeLastProjectPath(const std::string& path) {
    auto p = nyxLastProjectFilePath();
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    if (f) f << path << "\n";
}
} // namespace

Engine::Engine() = default;

Engine::~Engine() {
    // Don't leave an orphaned game window when the editor closes.
    stopPlay();

    m_renderer.waitIdle(m_vulkanContext.getDevice());

    // Persist editor layout (collapse + sizes) before tearing things down.
    // Skipped in game mode: the transient play process must not overwrite the
    // editor's saved camera pose / panel sizes.
    if (!m_gameMode) saveEditorPrefs();

    m_editor.cleanup(m_vulkanContext.getAllocator());
    m_gizmo.cleanup(m_vulkanContext.getAllocator());
    m_inspector.cleanup(m_vulkanContext.getAllocator());
    m_hierarchy.cleanup(m_vulkanContext.getAllocator());
    m_console.cleanup(m_vulkanContext.getAllocator());
    m_contentBrowser.cleanup(m_vulkanContext.getAllocator());
    m_titleBar.cleanup(m_vulkanContext.getAllocator());
    m_planet.cleanup(m_vulkanContext.getAllocator());
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

void Engine::setGameMode(const std::string& scenePath, const std::string& projectPath) {
    m_gameMode        = true;
    m_gameScenePath   = scenePath;
    m_gameProjectPath = projectPath;
}

void Engine::init(StatusFn onStatus) {
    m_statusFn = onStatus;   // lower layers (loadScene/loadGltfScene) tick into this
    auto status = [&](const char* s, float p) { if (onStatus) onStatus(s, p); };

    Logger::init();
    LOG_INFO("=== Nyx v0.3.0 (Phase 3B) ===");

    // Game mode: the content root is fixed by the launching editor, not by the
    // last-project file. Set it up front so window title / scene load below use it.
    if (m_gameMode && !m_gameProjectPath.empty())
        m_projectPath = std::filesystem::path(m_gameProjectPath).generic_string();

    // Re-open the last project the user worked in. Falls back to the default
    // m_projectPath ("projects/Sandbox") on first run or if the saved folder
    // no longer exists. Skipped in game mode (the editor dictates the project).
    if (!m_gameMode) {
        std::string last = readLastProjectPath();
        if (!last.empty() && std::filesystem::exists(last)) {
            m_projectPath = std::filesystem::path(last).generic_string();
            LOG_INFO("Reopening last project: {}", m_projectPath);
        }
    }

    status("Opening window...", 0.02f);
    // Always open at the default 1280×720 — the previous "persist window pos +
    // size + maximize flag" path was too fragile across monitor reconfigs and
    // borderless-vs-decorated maximize handling, and it left the UI clipped
    // when restored to a maximize-overhang position without re-maximizing.
    // Users resize / maximize each session; that's what a regular Windows app
    // does.
    m_window = std::make_unique<Window>(m_gameMode ? "Nyx \xE2\x80\x94 Play" : "Nyx Engine", 1280, 720);

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
    // Procgen: generate a planet (icosphere + fractal-noise terrain + biome colors).
    // `procgen.planet` uses a random seed; `procgen.planet <seed>` is reproducible.
    m_console.registerCommand("procgen.planet",
                              "procgen.planet [seed] - generate a procgen planet (random seed if omitted)",
                              [this, errCol](const std::vector<std::string>& a) {
        uint32_t seed;
        if (a.empty()) {
            seed = std::random_device{}();
        } else {
            try { seed = (uint32_t)std::stoul(a[0]); }
            catch (...) { m_console.print("usage: procgen.planet [seed]", errCol); return; }
        }
        createPlanetEntity(seed);
        m_console.print("Spawned planet (seed " + std::to_string(seed) + ")");
    });

    // planet.enter — make THIS scene a walkable planet world. In the editor this
    // configures + previews it (free-fly, scene controls); the character + walking
    // happen in PLAY mode and the exported game (gated to game mode). Persisted in
    // the scene's EnvironmentComponent so the play process / exported exe spawn it.
    m_console.registerCommand("planet.enter",
                              "planet.enter [seed] - make this scene a walkable planet (preview; walk in Play)",
                              [this, errCol](const std::vector<std::string>& a) {
        uint32_t seed;
        if (a.empty()) {
            seed = std::random_device{}();
        } else {
            try { seed = (uint32_t)std::stoul(a[0]); }
            catch (...) { m_console.print("usage: planet.enter [seed]", errCol); return; }
        }
        const float radius = 1500.0f;                         // "huge but walkable in ~10 min"
        const glm::vec3 center{0.0f, 0.0f, 0.0f};
        m_renderer.waitIdle(m_vulkanContext.getDevice());     // safe to (re)build GPU meshes
        m_planet.cleanup(m_vulkanContext.getAllocator());     // rebuild if re-entered
        m_planet.init(m_vulkanContext, m_descriptors, m_resourceCache, seed, center, radius);
        // Persist on the scene so Play / the exported game spawn the player here.
        EnvironmentComponent& env = m_registry.get<EnvironmentComponent>(ensureEnvironmentEntity());
        env.planetActive = true; env.planetSeed = seed; env.planetRadius = radius;
        m_sceneDirty = true;
        if (m_gameMode) {                                     // exported/play exe: drop in walking
            setPlanetWalk(true);
        } else {                                              // editor: free-fly preview only
            m_camera.frame(center, radius * 1.3f);
            m_console.print("Planet set for this scene (seed " + std::to_string(seed)
                            + "). Free-fly to preview; press Play to walk it.");
        }
    });

    m_console.registerCommand("planet.exit", "planet.exit - leave the streaming planet",
                              [this](const std::vector<std::string>&) {
        if (!m_planet.active()) { m_console.print("no active planet"); return; }
        if (m_planetWalk) setPlanetWalk(false);             // release cursor, restore free-fly
        m_renderer.waitIdle(m_vulkanContext.getDevice());
        m_planet.cleanup(m_vulkanContext.getAllocator());
        // Editor: also clear the scene's planet flag so it stops being a planet world.
        if (!m_gameMode) {
            EnvironmentComponent& env = m_registry.get<EnvironmentComponent>(ensureEnvironmentEntity());
            env.planetActive = false;
            m_sceneDirty = true;
        }
        m_console.print("Left planet");
    });

    m_console.registerCommand("planet.walk", "planet.walk - walk the surface (Play mode only)",
                              [this](const std::vector<std::string>&) {
        if (!m_gameMode) { m_console.print("walking is a Play-mode feature — press Play (the editor previews in free-fly)"); return; }
        if (!m_planet.active()) { m_console.print("no active planet"); return; }
        setPlanetWalk(true);
        m_console.print("Walk mode: WASD move, mouse look, TAB frees the cursor");
    });
    m_console.registerCommand("planet.fly", "planet.fly - free-fly overview of the planet",
                              [this](const std::vector<std::string>&) {
        if (!m_planet.active()) { m_console.print("no active planet"); return; }
        setPlanetWalk(false);
        m_console.print("Fly mode: WASD fly, middle-drag to look");
    });

    // Objective readout of LOD streaming: cached vs visible chunk counts + the
    // camera's distance/altitude. Fly toward the planet and re-run it — the counts
    // should climb as finer chunks stream in.
    m_console.registerCommand("planet.stats", "planet.stats - print planet LOD/streaming stats",
                              [this](const std::vector<std::string>&) {
        if (!m_planet.active()) { m_console.print("no active planet"); return; }
        glm::vec3 c = m_planet.center();
        float dist = glm::length(m_camera.getPosition() - c);
        float alt  = dist - m_planet.radius();
        m_console.print("planet: chunks=" + std::to_string(m_planet.chunkCount())
                      + " visible=" + std::to_string(m_planet.draws().size())
                      + " dist=" + std::to_string((int)dist)
                      + " altitude=" + std::to_string((int)alt));
    });

    m_console.registerCommand("planet.land", "planet.land - drop the camera onto the planet surface",
                              [this](const std::vector<std::string>&) {
        if (!m_planet.active()) { m_console.print("no active planet"); return; }
        glm::vec3 c = m_planet.center();
        glm::vec3 d = m_camera.getPosition() - c;
        if (glm::length(d) < 1e-3f) d = glm::vec3(0.0f, 1.0f, 0.0f);
        d = glm::normalize(d);
        float sd = m_planet.surfaceDistance(c + d * m_planet.radius());
        m_camera.setPose(c + d * (sd + 2.0f), m_camera.getYaw(), 0.0f);
        m_console.print("Landed on the surface");
    });

    // Dev diagnostics: NYX_SHOT=<path> auto-writes a screenshot after ~200 frames
    // (lets the framebuffer be inspected headlessly when "black screen" reports come in).
    if (const char* shot = std::getenv("NYX_SHOT")) m_shotPath = shot;

    // Dev self-test hook: NYX_PLANET=<seed> auto-enters a planet on startup and
    // logs chunk streaming, so the system can be verified headlessly (stdout).
    if (const char* envSeed = std::getenv("NYX_PLANET")) {
        uint32_t seed = 12345;
        try { seed = (uint32_t)std::stoul(envSeed); } catch (...) {}
        const float radius = 150000.0f;
        const glm::vec3 center{0.0f, 0.0f, 0.0f};
        m_renderer.waitIdle(m_vulkanContext.getDevice());
        m_planet.init(m_vulkanContext, m_descriptors, m_resourceCache, seed, center, radius);
        m_planet.setLogStreaming(true);
        if (std::getenv("NYX_OVERVIEW")) {
            m_camera.frame(center, radius * 3.0f);   // diagnostic: far overview (preview-equivalent)
            LOG_INFO("NYX_PLANET auto-enter: seed {} (overview)", seed);
        } else {
            setPlanetWalk(true);             // project script (game::onSpawn) places the player on land
            LOG_INFO("NYX_PLANET auto-enter: seed {} (walking)", seed);
        }
    }

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
        // Unknown / binary types are intentionally not opened in the code
        // editor — the hex preview was rarely useful and noisy on accidental
        // double-clicks of .spv / .ttf / random assets.
        else LOG_INFO("Skipping unknown file type in code editor: {}", path);
    });
    m_contentBrowser.setPathRemovedCallback([this](const std::string& path) {
        m_editor.closePath(path);
    });
    // File-menu (project) actions the engine owns.
    m_contentBrowser.setFileMenuCallback([this](const std::string& action) {
        if      (action == "save") {
            // One "Save" now saves everything: all open text tabs + the scene.
            m_editor.saveAll();
            saveCurrentScene();
        }
        else if (action == "saveas")  saveProjectAs();
        else if (action == "export")  exportGame();
        else if (action == "exit")    glfwSetWindowShouldClose(m_window->getHandle(), GLFW_TRUE);
        else if (action == "openproject") {
            // Steer the picker to the engine's projects/ root (parent of the
            // currently-open project) so the user is browsing sibling projects
            // rather than whatever folder Windows last remembered.
            namespace fs = std::filesystem;
            fs::path parent = fs::path(m_projectPath).parent_path();
            std::string startDir = parent.empty() ? std::string("projects")
                                                  : parent.generic_string();
            std::string dir = m_window->openFolderDialog("Open Project Folder", startDir);
            if (!dir.empty()) switchProject(dir);
        }
    });
    // New Project / Switch Project from the browser menus route here. Engine
    // owns the full save-current / clear-state / load-new flow + persists
    // the choice across sessions.
    m_contentBrowser.setSwitchProjectCallback([this](const std::string& path) {
        switchProject(path);
    });

    // Scene Hierarchy → selection drives the Inspector; commands act on the selection.
    m_hierarchy.setSelectCallback([this](Entity e) { m_selectedEntity = e; });
    m_hierarchy.setRenameCommitCallback([this](Entity e, std::string name) {
        if (e == NULL_ENTITY) return;
        // Empty name → remove the NameComponent so the hierarchy falls back to
        // the kind-derived default label.
        if (name.empty()) {
            if (m_registry.has<NameComponent>(e)) m_registry.pool<NameComponent>().remove(e);
        } else {
            if (m_registry.has<NameComponent>(e)) m_registry.get<NameComponent>(e).name = name;
            else                                  m_registry.assign<NameComponent>(e, NameComponent{name});
        }
        m_sceneDirty = true;
    });
    m_hierarchy.setCommandCallback([this](SceneHierarchy::Command c) {
        switch (c) {
            case SceneHierarchy::Command::Delete:    deleteSelection();    break;
            case SceneHierarchy::Command::Copy:      copySelection();      break;
            case SceneHierarchy::Command::Cut:       cutSelection();       break;
            case SceneHierarchy::Command::Paste:     pasteClipboard();     break;
            case SceneHierarchy::Command::Duplicate: duplicateSelection(); break;
            case SceneHierarchy::Command::Group:     groupSelected();      break;
            case SceneHierarchy::Command::Ungroup:   ungroupSelected();    break;
            case SceneHierarchy::Command::CreateCube:       createCubeEntity();        break;
            case SceneHierarchy::Command::CreatePointLight: createLightEntity(false);  break;
            case SceneHierarchy::Command::CreateDirLight:   createLightEntity(true);   break;
            case SceneHierarchy::Command::Rename: {
                if (m_hierarchy.selection().empty()) break;
                Entity e = m_hierarchy.selection().back();
                std::string initial;
                if (m_registry.has<NameComponent>(e)) initial = m_registry.get<NameComponent>(e).name;
                m_hierarchy.startRename(e, initial);
                break;
            }
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
        // Snapshot every entity in the hierarchy selection so a group scrub
        // (Inspector applies the same delta to all selected) can be undone in
        // one Ctrl+Z. Falls back to the primary if the hierarchy is empty.
        std::vector<Entity> sel = m_hierarchy.selection();
        if (sel.empty() && m_selectedEntity != NULL_ENTITY) sel.push_back(m_selectedEntity);
        beginTransformUndo(sel);
    });
    m_inspector.setOnEditCallback([this]() {
        m_sceneDirty = true;
        // A live material-scalar edit (subsurface / metallic / roughness) mutated
        // only the CPU component; push it to the GPU material UBO now so the change
        // shows this frame (the per-material UBO is GPU-only, not per-frame).
        if (m_pendingScalarActive
            && (m_pendingScalarTarget == Inspector::ScalarField::MaterialSubsurface
             || m_pendingScalarTarget == Inspector::ScalarField::MaterialMetallic
             || m_pendingScalarTarget == Inspector::ScalarField::MaterialRoughness))
            reuploadMaterialParams(m_pendingScalarEntity);
    });
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

    // Play/Stop toolbar (editor only). Toggling launches the standalone game
    // process or terminates the running one; the game process never shows the
    // button (no callback set → not drawn / not hit-tested).
    if (!m_gameMode) {
        m_titleBar.setOnPlayToggle([this]() {
            if (isPlayRunning()) stopPlay();
            else                 launchPlay();
        });
    }

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
    Input::setGroupSelectedCallback([this]() { groupSelected(); });
    Input::setViewportPressCallback([this](double mx, double my) { onViewportPress(mx, my); });
    Input::setViewportRightClickCallback([this](double mx, double my) { onViewportRightClick(mx, my); });
    Input::setViewportZoomCallback([this](double sy) {
        if (m_planetWalk) {                                   // scroll zooms the third-person boom
            m_player.camDist = glm::clamp(m_player.camDist - static_cast<float>(sy) * 2.0f, 3.0f, 60.0f);
        } else if (!m_gameMode) {
            m_camera.dolly(static_cast<float>(sy), selectionPivot());   // editor free-fly only
        }
        // Play-mode planet overview: ignore — the project script owns that camera.
    });
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

    // loadEditorPrefs() ran before the camera existed, so the saved camera pose
    // was buffered into m_pendingCameraPose. Apply it here so the editor opens
    // looking at the same view you left it in.
    if (m_pendingCameraPose.has) {
        m_camera.setPose(m_pendingCameraPose.position,
                         m_pendingCameraPose.yaw,
                         m_pendingCameraPose.pitch);
        m_camera.setFov(m_pendingCameraPose.fov);
        m_pendingCameraPose.has = false;
    }

    // Seed the previous-frame tracker with the pose we just restored so the
    // in-session debounced save (in run()) only fires once the user actually moves.
    m_prevCamPos   = m_camera.getPosition();
    m_prevCamYaw   = m_camera.getYaw();
    m_prevCamPitch = m_camera.getPitch();
    m_prevCamFov   = m_camera.getFov();

    m_time.init();

    // Load the project's saved scene if one exists; otherwise seed a clean
    // default scene (Environment only). The saved scene is authoritative — once
    // you Save Scene, your saved state is what loads, including any models with
    // full PBR maps and transforms (.scene round-trips materials losslessly).
    {
        // Game mode loads the exact scene the editor handed us; otherwise the
        // project's saved scene.
        std::string projectScene = (m_gameMode && !m_gameScenePath.empty())
                                  ? m_gameScenePath
                                  : (m_projectPath + "/scenes/scene.scene");
        if (std::filesystem::exists(projectScene)) {
            status("Loading scene...", 0.65f);
            loadScene(projectScene);
        } else {
            status("Building default scene...", 0.65f);
            buildDefaultScene();
        }
    }

    // Resume the persistent undo history from disk so the user can keep undoing
    // changes from a previous session. (Must run AFTER loadScene — clearScene wipes
    // the in-memory stacks.)
    status("Restoring undo history...", 0.96f);
    loadUndoHistoryFromDisk();

    status("Ready.", 1.0f);
    m_statusFn = nullptr;   // splash is about to close
    // Persist whichever project is now loaded so future launches reopen it
    // even after a clean first-run (where last_project.txt didn't exist yet).
    // Not in game mode — the transient play process must not change which project
    // the editor reopens next.
    if (!m_gameMode) writeLastProjectPath(m_projectPath);
    LOG_INFO("Engine initialized successfully!");
}

void Engine::run() {
    if (m_gameMode) { runGame(); return; }

    LOG_INFO("Entering main loop");

    // Note: live content updates during a Win32 modal resize/move drag are
    // a known limitation of Vulkan/DXGI windows — DWM caches the last frame
    // and only resumes picking up new presents after WM_EXITSIZEMOVE. The
    // window itself still resizes live; just the rendered contents update
    // on release. Matches typical Vulkan game/engine behavior.

    // Reveal the main window now that the editor is fully initialised. Created
    // hidden in Window — see GLFW_VISIBLE hint — so the splash owns the screen
    // through startup. Restore the saved window state: maximized (default), else
    // the saved windowed size. The first-frame resize check recreates the
    // swapchain at the new size.
    // Establish the windowed size first, so exiting fullscreen/maximize later
    // returns to it; then apply fullscreen (highest precedence) or maximized.
    if (m_pendingWindow.w > 0 && m_pendingWindow.h > 0)
        m_window->setSize(m_pendingWindow.w, m_pendingWindow.h);
    if (m_pendingWindow.fullscreen)
        m_window->toggleFullscreen();
    else if (m_pendingWindow.maximized)
        m_window->maximize();
    m_window->show();

    // Seed the previous-frame trackers with the state we just applied so the
    // in-loop debounced save only fires once the user actually changes something.
    m_windowedW = (m_pendingWindow.w > 0) ? m_pendingWindow.w : m_window->getWidth();
    m_windowedH = (m_pendingWindow.h > 0) ? m_pendingWindow.h : m_window->getHeight();
    m_prevWinW       = m_window->getWidth();
    m_prevWinH       = m_window->getHeight();
    m_prevWinMax     = m_window->isEffectivelyMaximized();
    m_prevFullscreen = m_window->isFullscreen();

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
        // The OS draws the title bar now, so hide the in-engine CAPTION
        // (logo, "Nyx Engine" text, min/max/close buttons). Keep the widget
        // itself "visible" so the FPS overlay (top-right) still renders.
        // Detect the game process exiting (window closed / crashed) so the
        // toolbar flips Stop → Play on its own.
        updatePlayProcess();

        m_titleBar.setVisible(!m_window->isFullscreen());
        m_titleBar.setCaptionVisible(false);
        m_titleBar.setPlayRunning(isPlayRunning());
        bool cursorFree = !Input::isCursorCaptured();
        m_titleBar.update(static_cast<float>(m_window->getWidth()),
                          static_cast<float>(m_window->getHeight()), cursorFree,
                          m_time.getDeltaTime(), m_renderer.getCurrentFrame(),
                          m_window->isFullscreen() ? 0.0f : m_rightDockWidth,
                          m_window->isFullscreen() ? 0.0f : m_contentBrowser.currentWidth(),
                          // Push the play button + FPS overlay below the editor tab bar when one is shown.
                          (!m_window->isFullscreen() && m_editor.hasDocs()) ? CodeEditor::TABBAR_H : 0.0f);
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
            m_inspector.setSelectionGroup(m_hierarchy.selection());
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
                    || m_hierarchy.wantsPointerCursor()
                    || m_editor.wantsPointerCursor())
                    shape = GLFW_HAND_CURSOR;
            }
            m_titleBar.applyCursor(shape);
        }

        // Update UBO with current camera matrices
        updateUniformBuffer(m_renderer.getCurrentFrame());

        // Refresh planet LOD against the camera (no-op unless a planet is active).
        m_planet.update(m_camera.getPosition());
        glm::vec3 originOffset = m_planet.active() ? m_camera.getPosition() : glm::vec3(0.0f);

        bool ok = m_renderer.drawFrame(m_vulkanContext, m_swapchain, m_pipeline,
                                        m_registry, m_descriptors, &m_shadowMap,
                                        m_pointShadows.data(),
                                        m_pointShadowJobs.data(), m_pointShadowJobs.size(),
                                        &m_uiPipeline, &m_titleBar, &m_contentBrowser, &m_console, &m_editor,
                                        &m_imagePipeline, &m_matPreviewPipeline,
                                        &m_hierarchy, &m_inspector, &m_gizmo, &m_planet, originOffset);
        if (!ok) {
            handleResize();
        }

        if (!m_shotPath.empty() && ++m_frameNo == 200) captureScreenshot(m_shotPath);

        // Persist any mutation made this frame. pushUndo flips m_sceneDirty true; the
        // save flushes the POST-mutation scene to scene.scene so the next launch sees
        // exactly the state you left.
        if (m_sceneDirty) {
            saveCurrentScene();
            m_sceneDirty = false;
        }

        // Persist the camera pose AND window size/maximized state during the
        // session (debounced ~0.8s after they settle), not only in the destructor
        // — so they survive a crash or a non-clean exit, the same way the scene
        // auto-saves continuously.
        {
            glm::vec3 p    = m_camera.getPosition();
            float yaw      = m_camera.getYaw();
            float pitch    = m_camera.getPitch();
            float fov      = m_camera.getFov();
            int  ww        = m_window->getWidth();
            int  wh        = m_window->getHeight();
            bool wmax      = m_window->isEffectivelyMaximized();
            bool wfs       = m_window->isFullscreen();
            // Remember the size only while plain-windowed (this is what we persist
            // as the windowed size).
            if (!wfs && !wmax) { m_windowedW = ww; m_windowedH = wh; }

            // Debounce against the PREVIOUS FRAME (the m_prev* trackers hold last
            // frame's values), not the last save: arm the countdown while a value
            // is actively changing, then fire ONCE it's been stable ~0.8s. Comparing
            // to the last-SAVED value re-armed every frame until the save, so the
            // countdown never reached 0 and nothing was ever auto-saved.
            bool changedThisFrame =
                   p     != m_prevCamPos   || yaw  != m_prevCamYaw
                || pitch != m_prevCamPitch || fov  != m_prevCamFov
                || ww    != m_prevWinW     || wh   != m_prevWinH
                || wmax  != m_prevWinMax   || wfs  != m_prevFullscreen;
            m_prevCamPos = p; m_prevCamYaw = yaw; m_prevCamPitch = pitch; m_prevCamFov = fov;
            m_prevWinW = ww;  m_prevWinH = wh;  m_prevWinMax = wmax; m_prevFullscreen = wfs;

            if (changedThisFrame) {
                m_prefsSaveCountdown = 0.8f;                 // (re)arm while changing
            } else if (m_prefsSaveCountdown > 0.0f) {
                m_prefsSaveCountdown -= m_time.getDeltaTime();
                if (m_prefsSaveCountdown <= 0.0f) {
                    saveEditorPrefs();
                    m_prefsSaveCountdown = -1.0f;            // done until the next change
                }
            }
        }
    }

    LOG_INFO("Main loop ended");
}

// ── Standalone game/play loop ───────────────────────────────────────────────
// Runs in the child process launched with `--play`. No editor chrome, no
// gizmo/picking, free-fly camera, ESC to quit, and — critically — no scene
// autosave, so a play session never mutates the project on disk.
void Engine::runGame() {
    LOG_INFO("Entering game loop (play mode)");

    // Hide every editor panel. Their draw() calls early-out on !isVisible(), so
    // only the 3D scene reaches the screen (same path the editor's fullscreen
    // mode already exercises). We never flip these back on in this process.
    m_titleBar.setVisible(false);
    m_contentBrowser.setVisible(false);
    m_console.setVisible(false);
    m_editor.setVisible(false);
    m_hierarchy.setVisible(false);
    m_inspector.setVisible(false);

    // Play window opens maximized (the chosen "maximize viewport" play layout).
    m_window->maximize();
    m_window->show();

    // If this scene is a walkable planet world, create the planet and drop the
    // player onto its surface in walk mode (this is the play/exported-game path).
    {
        EnvironmentComponent& env = m_registry.get<EnvironmentComponent>(ensureEnvironmentEntity());
        // Play mode always opens in the planet OVERVIEW (this is the planet game). Use the
        // scene's configured planet if it has one, otherwise a sensible default — so Play
        // reliably shows a planet you can reroll (Space) and drop into (Enter), even if the
        // scene was never set up with planet.enter in the editor.
        uint32_t seed   = env.planetActive ? env.planetSeed : 1337u;
        // World radius 150000 (100× the old 1500). The preview still fits: the overview
        // camera sits at a fixed multiple of the radius, so a bigger planet just sits
        // proportionally farther and looks the same on screen. (kMaxLevel/kUniformLevel
        // were raised so local terrain stays walkable-fine despite the larger radius.)
        float radius = 150000.0f;
        m_planet.init(m_vulkanContext, m_descriptors, m_resourceCache, seed, glm::vec3(0.0f), radius);
        env.planetActive = true;          // ensure the overview branch + walk gate are live
        env.planetSeed   = seed;
        env.planetRadius = radius;
        m_camera.frame(glm::vec3(0.0f), radius * 3.0f);   // pulled back; project orbit refines it
        LOG_INFO("Game: planet overview (seed {}, radius {}) — Space = new world, Enter = explore",
                 seed, (int)radius);
    }

    while (!m_window->shouldClose()) {
        m_window->pollEvents();

        Input::update();
        m_time.update();

        if (Input::isKeyDown(GLFW_KEY_ESCAPE))
            glfwSetWindowShouldClose(m_window->getHandle(), GLFW_TRUE);

        while (m_time.shouldTick()) {
            fixedUpdate(Time::FIXED_DT);
            m_time.consumeTick();
        }

        if (m_window->wasResized()) {
            m_window->resetResizedFlag();
            handleResize();
            continue;
        }

        // Refresh planet LOD/draw list against the camera (the editor loop does this
        // too — without it the play process never streams chunks → black screen).
        m_planet.update(m_camera.getPosition());

        updateUniformBuffer(m_renderer.getCurrentFrame());

        bool ok = m_renderer.drawFrame(m_vulkanContext, m_swapchain, m_pipeline,
                                        m_registry, m_descriptors, &m_shadowMap,
                                        m_pointShadows.data(),
                                        m_pointShadowJobs.data(), m_pointShadowJobs.size(),
                                        &m_uiPipeline, &m_titleBar, &m_contentBrowser, &m_console, &m_editor,
                                        &m_imagePipeline, &m_matPreviewPipeline,
                                        &m_hierarchy, &m_inspector, &m_gizmo, &m_planet,
                                        m_planet.active() ? m_camera.getPosition() : glm::vec3(0.0f));
        if (!ok) handleResize();
        if (!m_shotPath.empty() && ++m_frameNo == 200) captureScreenshot(m_shotPath);
        // No saveCurrentScene() — play mode is non-destructive.
    }

    LOG_INFO("Game loop ended");
}

// ── Editor side: launch / stop / poll the standalone game process ───────────
void Engine::launchPlay() {
    if (m_playProcess) return;   // already running (toggle guards this too)

    // Persist the current edits first so the child loads exactly what's on screen.
    m_editor.saveAll();
    saveCurrentScene();

#ifdef _WIN32
    namespace fs = std::filesystem;
    std::error_code ec;

    std::string scenePath = m_currentScenePath.empty()
                          ? (m_projectPath + "/scenes/scene.scene")
                          : m_currentScenePath;
    std::wstring sceneW = fs::absolute(scenePath, ec).wstring();
    std::wstring projW  = fs::absolute(m_projectPath, ec).wstring();

    wchar_t exeBuf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { LOG_WARN("launchPlay: cannot resolve exe path"); return; }
    std::wstring exeW(exeBuf, n);

    std::wstring cmd = L"\"" + exeW + L"\" --play \"" + sceneW + L"\" --project \"" + projW + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};        si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL okp = CreateProcessW(exeW.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
                              0, nullptr, nullptr, &si, &pi);
    if (!okp) { LOG_WARN("launchPlay: CreateProcess failed (err {})", (unsigned)GetLastError()); return; }

    CloseHandle(pi.hThread);
    m_playProcess = pi.hProcess;
    LOG_INFO("Play: launched game process (pid {})", (unsigned)pi.dwProcessId);
#else
    LOG_WARN("launchPlay: standalone play is only implemented on Windows");
#endif
}

void Engine::stopPlay() {
#ifdef _WIN32
    if (m_playProcess) {
        TerminateProcess(static_cast<HANDLE>(m_playProcess), 0);
        CloseHandle(static_cast<HANDLE>(m_playProcess));
        m_playProcess = nullptr;
        LOG_INFO("Play: stopped game process");
    }
#endif
}

void Engine::updatePlayProcess() {
#ifdef _WIN32
    if (m_playProcess
        && WaitForSingleObject(static_cast<HANDLE>(m_playProcess), 0) == WAIT_OBJECT_0) {
        CloseHandle(static_cast<HANDLE>(m_playProcess));
        m_playProcess = nullptr;
        LOG_INFO("Play: game process exited");
    }
#endif
}

GameContext Engine::makeGameContext(float dt) {
    GameContext ctx;
    ctx.dt             = dt;
    ctx.camera         = &m_camera;
    ctx.planet         = &m_planet;
    ctx.player         = &m_player;
    ctx.cursorCaptured = m_walkCursorCaptured;
    ctx.walking        = m_planetWalk;
    return ctx;
}

void Engine::updatePlanetWalk(float dt) {
    // Controls are a PROJECT gameplay script: it reads input, moves the player, and
    // drives the camera through the context. The engine just renders the avatar at
    // the resulting player state (it owns the avatar entity, not the gameplay).
    GameContext ctx = makeGameContext(dt);
    game::update(ctx);
    Input::consumeMouseDelta();   // applied once per frame even if this ticks 2× (or 0×)

    if (m_charEntity != NULL_ENTITY && m_registry.has<TransformComponent>(m_charEntity)) {
        // Right-handed basis (cross(up,forward) so col0×col1=col2): a reflection
        // (negative determinant) makes quat_cast garbage → the avatar lies on its side.
        glm::vec3 right = glm::normalize(glm::cross(m_player.up, m_player.forward));
        glm::mat3 basis(right, m_player.up, m_player.forward);   // local X/Y/Z → world
        TransformComponent& tc = m_registry.get<TransformComponent>(m_charEntity);
        tc.position = m_player.pos;
        tc.rotation = glm::quat_cast(glm::mat4(basis));
        tc.scale    = glm::vec3(1.0f);
    }
}

// Roll a new world: rebuild the planet's streaming chunks from a new seed (same centre
// and radius). Used by the in-game overview (Space). Safe mid-loop — same teardown the
// planet.enter console command uses (idle the GPU, free chunks, re-init).
void Engine::regeneratePlanet(uint32_t newSeed) {
    if (!m_planet.active()) return;
    glm::vec3 center = m_planet.center();
    float     radius = m_planet.radius();
    m_renderer.waitIdle(m_vulkanContext.getDevice());
    m_planet.cleanup(m_vulkanContext.getAllocator());
    m_planet.init(m_vulkanContext, m_descriptors, m_resourceCache, newSeed, center, radius);
    // Track the seed on the scene's environment so the overview keeps rolling fresh ones.
    EnvironmentComponent& env = m_registry.get<EnvironmentComponent>(ensureEnvironmentEntity());
    env.planetSeed = newSeed;
    m_camera.frame(center, radius * 3.0f);   // re-frame the overview on the new planet (project orbit refines)
    LOG_INFO("Regenerated planet world (seed {})", newSeed);
}

void Engine::captureScreenshot(const std::string& path) {
    VkDevice     device = m_vulkanContext.getDevice();
    VmaAllocator alloc  = m_vulkanContext.getAllocator();
    vkDeviceWaitIdle(device);

    VkExtent2D ext = m_swapchain.getExtent();
    uint32_t W = ext.width, H = ext.height;
    if (W == 0 || H == 0 || m_swapchain.getImages().empty()) { LOG_WARN("screenshot: no swapchain image"); return; }
    VkImage      src = m_swapchain.getImages()[0];
    VkFormat     fmt = m_swapchain.getImageFormat();
    VkDeviceSize sz  = (VkDeviceSize)W * H * 4;

    VkBufferCreateInfo bci{}; bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sz; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
    VkBuffer buf; VmaAllocation bufAlloc;
    if (vmaCreateBuffer(alloc, &bci, &aci, &buf, &bufAlloc, nullptr) != VK_SUCCESS) { LOG_WARN("screenshot: buffer alloc"); return; }

    VkCommandBuffer cmd = m_vulkanContext.beginSingleTimeCommands();
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = src; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = sa; b.dstAccessMask = da;
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    m_vulkanContext.endSingleTimeCommands(cmd);

    void* data = nullptr; vmaMapMemory(alloc, bufAlloc, &data);
    const uint8_t* s = (const uint8_t*)data;
    bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
    std::vector<uint8_t> rgba(sz);
    double lum = 0.0;
    for (VkDeviceSize i = 0; i < (VkDeviceSize)W * H; ++i) {
        uint8_t r = bgra ? s[i*4+2] : s[i*4+0];
        uint8_t g = s[i*4+1];
        uint8_t b = bgra ? s[i*4+0] : s[i*4+2];
        rgba[i*4+0] = r; rgba[i*4+1] = g; rgba[i*4+2] = b; rgba[i*4+3] = 255;
        lum += r + g + b;
    }
    vmaUnmapMemory(alloc, bufAlloc);
    stbi_write_png(path.c_str(), (int)W, (int)H, 4, rgba.data(), (int)W * 4);
    vmaDestroyBuffer(alloc, buf, bufAlloc);
    LOG_INFO("screenshot: wrote {} ({}x{}) avgLum={}", path, W, H, (int)(lum / ((double)W * H * 3.0)));
}

void Engine::setPlanetWalk(bool walk) {
    if (walk) {
        if (!m_planet.active()) { m_console.print("no active planet"); return; }
        // Engine owns the avatar entity (rendering); the project script owns where it
        // goes. Spawn the avatar mesh if needed, then let the script place the player.
        if (m_charEntity == NULL_ENTITY) {
            std::vector<Vertex> cv; std::vector<uint32_t> ci; makeCharacter(cv, ci);
            Mesh* cm = m_resourceCache.getOrCreateMesh(m_vulkanContext, "prim:character", cv, ci);
            m_charEntity = createMeshEntity(cm, m_resourceCache.getDefaultTexture(), glm::vec3(0.0f),
                                            glm::vec3(1.0f), glm::vec4(1.0f), 0.0f, 0.6f, "", "",
                                            nullptr, nullptr, nullptr, 0.01f);   // cutout → two-sided
            m_registry.assign<OcclusionOverlay>(m_charEntity, {});   // draw-through silhouette when behind terrain
        }
        GameContext ctx = makeGameContext(0.0f);
        game::onSpawn(ctx);                                   // project script places the player
        m_planetWalk = true;
        m_walkCursorCaptured = true;
        m_player.firstLook = true;                            // swallow the capture-frame mouse jump
        m_console.setFocused(false);    // so WASD/mouse-look work immediately after the command
        glfwSetInputMode(m_window->getHandle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    } else {
        m_planetWalk = false;
        if (m_charEntity != NULL_ENTITY) {            // remove the avatar
            m_registry.destroyEntity(m_charEntity);
            m_charEntity = NULL_ENTITY;
        }
        if (m_walkCursorCaptured) {
            m_walkCursorCaptured = false;
            glfwSetInputMode(m_window->getHandle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        // Hand orientation back to the free-fly camera without a snap.
        glm::vec3 f = m_camera.getFront();
        m_camera.setPose(m_camera.getPosition(),
                         glm::degrees(std::atan2(f.z, f.x)),
                         glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f))));
    }
}

void Engine::fixedUpdate(float dt) {
    // Focus pivot drives both the dynamic WASD fly speed and middle-mouse orbit:
    // the selected object's centre, or a point ahead of the camera when nothing
    // is selected. Computed once per tick and shared.
    glm::vec3 pivot = selectionPivot();

    // TAB toggles the cursor free while walking, so the console / UI is reachable
    // (mouse-look recaptures it). Edge-detected.
    bool tab = Input::isKeyDown(GLFW_KEY_TAB);
    if (m_planetWalk && tab && !m_prevTab) {
        m_walkCursorCaptured = !m_walkCursorCaptured;
        glfwSetInputMode(m_window->getHandle(), GLFW_CURSOR,
                         m_walkCursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        m_player.firstLook = m_walkCursorCaptured;   // swallow the recapture-frame jump
    }
    m_prevTab = tab;

    // Play mode only: ENTER toggles planet surface-exploration (the console isn't
    // reachable in the game window). Bootstraps the planet if the scene configured
    // one (or a default) and none is active yet; otherwise flips walk ↔ fly.
    if (m_gameMode && !Input::cameraMovementSuppressed()) {
        bool ent = Input::isKeyDown(GLFW_KEY_ENTER) || Input::isKeyDown(GLFW_KEY_KP_ENTER);
        if (ent && !m_prevEnter) {
            if (!m_planet.active()) {
                EnvironmentComponent& env = m_registry.get<EnvironmentComponent>(ensureEnvironmentEntity());
                uint32_t seed   = env.planetActive ? env.planetSeed   : 1337u;
                float    radius = env.planetActive ? env.planetRadius : 1500.0f;
                m_renderer.waitIdle(m_vulkanContext.getDevice());
                m_planet.init(m_vulkanContext, m_descriptors, m_resourceCache, seed, glm::vec3(0.0f), radius);
                setPlanetWalk(true);
            } else {
                setPlanetWalk(!m_planetWalk);              // walk ↔ fly overview
            }
        }
        m_prevEnter = ent;
    }

    if (m_planet.active() && m_planetWalk) {
        updatePlanetWalk(dt);                              // spherical-gravity walk
    } else if (m_gameMode && m_planet.active()) {
        // Play-mode planet OVERVIEW. The PROJECT script owns the camera and input here
        // (Space rerolls the world) — the engine's free-fly camera is an EDITOR control
        // and is deliberately NOT run in-game (that's what made Space also move the
        // camera up). Avatar isn't spawned in the overview, so nothing else to place.
        GameContext ctx = makeGameContext(dt);             // ctx.walking == false
        game::update(ctx);
        if (ctx.requestRegen) {
            EnvironmentComponent& env = m_registry.get<EnvironmentComponent>(ensureEnvironmentEntity());
            regeneratePlanet(env.planetSeed * 1664525u + 1013904223u);   // LCG → next seed
        }
    } else if (!m_gameMode) {
        // EDITOR free-fly (never in play mode — in-game cameras are project-owned).
        m_camera.update(dt, pivot);

        // Planet surface collision in fly mode: keep the camera from sinking through
        // the terrain so you can fly down and skim/land (free-fly + a hard floor).
        if (m_planet.active()) {
            glm::vec3 p  = m_camera.getPosition();
            glm::vec3 cp = m_planet.collide(p, 2.0f);      // 2-unit eye clearance
            if (cp != p) m_camera.setPose(cp, m_camera.getYaw(), m_camera.getPitch());
        }

        // Middle-mouse drag orbits the camera around the current selection.
        if (Input::isOrbiting())
            m_camera.orbit(pivot, Input::getMouseDeltaX(), Input::getMouseDeltaY());
    }

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

glm::vec3 Engine::entitiesPivot(const std::vector<Entity>& ents) {
    constexpr float INF = std::numeric_limits<float>::infinity();
    glm::vec3 mn{ INF,  INF,  INF};
    glm::vec3 mx{-INF, -INF, -INF};
    bool any = false;

    // Pre-compute parent → children adjacency once so we can walk descendants
    // of each input entity. Without this, grouping a Group (or selecting a
    // group whose children own the visible meshes) gives a degenerate AABB at
    // the group's pivot — the gizmo lands at the centroid but doesn't track
    // the actual visual centre of the meshes living below.
    auto& tfPool = m_registry.pool<TransformComponent>();
    std::unordered_map<Entity, std::vector<Entity>> kids;
    for (size_t i = 0; i < tfPool.size(); ++i) {
        Entity e = tfPool.getEntity(i);
        Entity p = tfPool[i].parent;
        if (p != NULL_ENTITY) kids[p].push_back(e);
    }

    auto growBy = [&](Entity e) {
        if (!m_registry.has<TransformComponent>(e)) return false;
        const auto& tc = m_registry.get<TransformComponent>(e);
        if (m_registry.has<MeshComponent>(e) && m_registry.get<MeshComponent>(e).mesh) {
            const Mesh* mesh = m_registry.get<MeshComponent>(e).mesh;
            const glm::vec3& lmn = mesh->boundsMin();
            const glm::vec3& lmx = mesh->boundsMax();
            for (int i = 0; i < 8; ++i) {
                glm::vec3 c{ (i & 1) ? lmx.x : lmn.x,
                             (i & 2) ? lmx.y : lmn.y,
                             (i & 4) ? lmx.z : lmn.z };
                glm::vec3 w = glm::vec3(tc.worldMatrix * glm::vec4(c, 1.0f));
                mn = glm::min(mn, w);
                mx = glm::max(mx, w);
            }
            return true;
        }
        return false;   // empty/group entity → contributes nothing on its own
    };

    std::function<void(Entity)> walk = [&](Entity e) {
        if (growBy(e)) any = true;
        auto it = kids.find(e);
        if (it == kids.end()) return;
        for (Entity c : it->second) walk(c);
    };

    for (Entity e : ents) walk(e);

    // If nothing in the walk carried a mesh, fall back to pivot points so we
    // still return something sensible (a group with no descendants, a lone
    // empty entity, ...).
    if (!any) {
        for (Entity e : ents) {
            if (!m_registry.has<TransformComponent>(e)) continue;
            glm::vec3 w = glm::vec3(m_registry.get<TransformComponent>(e).worldMatrix[3]);
            mn = glm::min(mn, w);
            mx = glm::max(mx, w);
            any = true;
        }
    }
    if (any) return (mn + mx) * 0.5f;

    // Nothing in the set → orbit/zoom around a point in front of the camera.
    return m_camera.getPosition() + m_camera.getFront() * 6.0f;
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
    return entitiesPivot(ents);
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
    // Re-query the window's current framebuffer size. During a modal Win32
    // resize loop this runs on the resize thread (see startResizeThread);
    // otherwise it runs on the main thread. Either way getWidth/getHeight
    // reflects the latest WM_SIZE delivered by GLFW. Skip on minimised
    // (0 × 0) windows.
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

    m_renderer.waitIdle(m_vulkanContext.getDevice());

    // Only the swapchain (images sized to the new client area) and the
    // framebuffers (which reference those images) depend on window
    // dimensions. All pipelines use VK_DYNAMIC_STATE_VIEWPORT and
    // VK_DYNAMIC_STATE_SCISSOR, so they don't need rebuilding on a size
    // change — that was costing ~14 shader reloads per WM_SIZE during a
    // modal resize drag and freezing the visible frame.
    m_swapchain.recreate(m_vulkanContext, width, height);
    m_renderer.recreateFramebuffers(m_vulkanContext, m_swapchain, m_pipeline);

    m_camera.setAspectRatio(static_cast<float>(width) / static_cast<float>(height));
}

void Engine::updateUniformBuffer(uint32_t currentFrame) {
    UniformBufferObject ubo{};

    // Floating origin: when a streaming planet is active, render camera-relative so
    // 32-bit float precision holds at planetary distances. The view becomes
    // rotation-only (camera at the origin), cameraPosition is zeroed, point lights
    // and the shadow matrix are shifted by the camera, and every object's model is
    // offset by -camPos (planet chunks do this in double; ECS entities below via the
    // renderer). When no planet is active this is a no-op (absolute rendering).
    const bool      camRel = m_planet.active();
    const glm::vec3 camPos = m_camera.getPosition();

    // Dynamic clip planes for planetary scale: near scales with altitude (tight on
    // the surface, loose in orbit), far always reaches across the whole planet.
    if (camRel) {
        float distToCenter = glm::length(camPos - m_planet.center());
        float maxR = m_planet.radius() * 1.25f;            // radius + max relief + margin
        float nearP, farP;
        if (m_planetWalk) {
            // On the surface: tiny near for terrain right in front of the camera; far
            // reaches a bit past the horizon so distant peaks still draw.
            float horizon = std::sqrt(std::max(1.0f, distToCenter * distToCenter
                                               - m_planet.radius() * m_planet.radius()));
            nearP = 0.1f;
            farP  = horizon + m_planet.radius() * 0.3f;
        } else {
            // Far overview: bracket near/far TIGHTLY around the planet. A tiny near plane
            // at ~400k units out collapses depth precision so front/back terrain faces
            // z-fight into holes — near must sit just in front of the planet, not at 5000.
            nearP = std::max(0.1f, distToCenter - maxR);
            farP  = distToCenter + maxR;
        }
        m_camera.setClip(nearP, farP);
    } else {
        m_camera.setClip(0.01f, 10000.0f);                 // editor defaults
    }

    ubo.projection = m_camera.getProjectionMatrix();
    if (camRel) {
        // Build the rotation-only view DIRECTLY from the camera basis with the eye at the
        // ORIGIN — do NOT strip translation from lookAt(pos, pos+front, up). At planetary
        // distances (camera ~600k units out) the `pos + front` inside lookAt rounds away
        // the unit `front`, so the recovered view DIRECTION jittered by degrees each frame
        // → the whole image shook. From the origin there's no large-magnitude cancellation,
        // so the orientation is rock-steady; the floating origin already offsets geometry.
        ubo.view           = glm::lookAt(glm::vec3(0.0f), m_camera.getFront(), m_camera.getUp());
        ubo.cameraPosition = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    } else {
        ubo.view           = m_camera.getViewMatrix();
        ubo.cameraPosition = glm::vec4(camPos, 1.0f);
    }

    // Third-person see-through is now handled entirely in the renderer: the character is
    // redrawn where terrain hides it, screen-door dithered (see mesh.frag occludable>2.5
    // + the overlay pass in Renderer). No per-fragment occluder data needed here.
    ubo.occluder = glm::vec4(0.0f);   // unused; kept for UBO layout compatibility

    // Pull sky + ambient from the singleton EnvironmentComponent. Auto-creates
    // the entity on first call so existing scenes (no env entity saved yet)
    // still get sensible defaults from the component's in-class initialisers.
    const EnvironmentComponent& env =
        m_registry.get<EnvironmentComponent>(ensureEnvironmentEntity());
    ubo.ambientColor = glm::vec4(env.ambient, 1.0f);
    ubo.skyTop       = glm::vec4(env.skyTop,     1.0f);
    ubo.skyHorizon   = glm::vec4(env.skyHorizon, env.skyIntensity);
    ubo.skyGround    = glm::vec4(env.skyGround,  1.0f);

    // Planet atmosphere: fade the sky gradient (which drives both the visible skybox
    // and the IBL) toward black space as the camera climbs out of the atmosphere.
    // On the surface it's the scene's blue sky; in orbit it's near-black with the
    // planet lit only by the sun — all via the existing sky pipeline, no shader.
    if (camRel) {
        float distToCenter = glm::length(camPos - m_planet.center());
        // Altitude above the LOCAL surface, so the sky stays blue while you're on the
        // ground (even atop a tall mountain) and only fades to space as you fly up.
        float altitude = std::max(0.0f, distToCenter - m_planet.surfaceDistance(camPos));
        float atmH = m_planet.radius() * 0.25f;                 // fly ~375u up → space
        float t    = glm::clamp(altitude / atmH, 0.0f, 1.0f);   // 0 = surface, 1 = space
        const glm::vec3 space(0.008f, 0.010f, 0.020f);
        ubo.skyTop     = glm::vec4(glm::mix(env.skyTop,     space, t), 1.0f);
        ubo.skyHorizon = glm::vec4(glm::mix(env.skyHorizon, space, t),
                                   glm::mix(env.skyIntensity, 0.05f, t));
        ubo.skyGround  = glm::vec4(glm::mix(env.skyGround,  space, t), 1.0f);
    }

    // Sun-shadow light-space matrix: orthographic projection looking along the
    // directional sun's direction. Volume is sized to cover the typical scene (about
    // the gladiator + small surroundings). With GLM_FORCE_DEPTH_ZERO_TO_ONE the proj
    // returns Vulkan-compatible z [0,1]; we Y-flip so shadow UVs match the renderer's
    // convention.
    // Resolve the sun direction. A .scene file load doesn't tag a specific
    // entity as the sun (and a fresh project has no directional light at all) —
    // fall back to the first directional light in the registry, if any, so
    // loaded scenes still cast. m_sunEntity stays valid only if something set it.
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

    // Planet OVERVIEW: lock the sun to the CAMERA (a fixed 3/4 angle relative to the
    // view) instead of the world. The camera orbits the planet, so a world-fixed sun
    // would make the lit side sweep across — making it look like the camera is flying
    // around. Co-rotating the sun keeps the lighting/terminator static on screen while
    // the terrain turns past, i.e. it reads as the PLANET spinning under a fixed lamp.
    const bool overviewSun = m_gameMode && m_planet.active() && !m_planetWalk;
    if (overviewSun) {
        glm::vec3 f = m_camera.getFront();
        glm::vec3 u = m_camera.getUp();
        glm::vec3 r = glm::normalize(glm::cross(f, u));
        sunDir = glm::normalize(f * 0.25f - r * 0.80f - u * 0.45f);   // light from upper-right of view
    }

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
    // Camera-relative: the frag passes camera-relative positions into lightSpace, so
    // convert them back to world first (translate by +camPos) before the light proj.
    if (camRel) ubo.lightSpace = ubo.lightSpace * glm::translate(glm::mat4(1.0f), camPos);

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
            // In the overview, all directional light tracks the camera-locked sun (above)
            // so the planet reads as spinning under a fixed lamp, not as a flying camera.
            glm::vec3 d = overviewSun ? sunDir : glm::normalize(lc.direction);
            gpu.positionAndType = glm::vec4(d, 0.0f);
        } else {
            // Point light — read position from TransformComponent
            glm::vec3 pos{0.0f};
            if (m_registry.has<TransformComponent>(entity)) {
                pos = m_registry.get<TransformComponent>(entity).position;
            }
            if (camRel) pos -= camPos;        // shift into camera-relative space
            gpu.positionAndType = glm::vec4(pos, 1.0f);
        }

        lightIndex++;
    }
    // Planet mode needs a sun: if the scene has no directional light the planet would
    // be lit only by IBL (near-black in space). Inject a default sun (UBO only, scene
    // untouched) so the lit hemisphere is always visible.
    if (camRel && lightIndex < MAX_LIGHTS) {
        bool hasDir = false;
        for (int k = 0; k < lightIndex; ++k)
            if (ubo.lights[k].positionAndType.w < 0.5f) { hasDir = true; break; }
        if (!hasDir) {
            GpuLightData& gpu = ubo.lights[lightIndex];
            gpu.positionAndType   = glm::vec4(sunDir, 0.0f);
            gpu.colorAndIntensity = glm::vec4(1.0f, 0.98f, 0.95f, 3.0f);
            gpu.params            = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
            lightIndex++;
        }
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
    // Capture the UBO Buffer* so inspector edits (subsurface) can re-upload params
    // to the existing GPU-only buffer without rebuilding the descriptor set.
    mat.descriptorSet = m_descriptors.allocateMaterialSet(m_vulkanContext, maps, params,
                                                          /*hostVisible=*/false, &mat.materialUBO);
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
    // NB: check "prim:planet" BEFORE "prim:plane" — the latter is a prefix of the
    // former, so the order matters or every planet would resolve to a plane.
    if (source.rfind("prim:planet", 0) == 0) {
        // "prim:planet:<seed>" — the seed makes terrain reproducible across scene
        // save/load. Cache by the full source so each seed is its own mesh.
        uint32_t seed = 1337;
        size_t colon = source.find(':', 5);   // the ':' before the seed (after "prim")
        if (colon != std::string::npos) {
            try { seed = (uint32_t)std::stoul(source.substr(colon + 1)); } catch (...) {}
        }
        std::vector<Vertex> v; std::vector<uint32_t> i;
#ifdef NYX_HAS_PLANET
        procgen::makePlanet(v, i, seed);
        return m_resourceCache.getOrCreateMesh(m_vulkanContext, source, v, i);
#else
        // No project terrain generator compiled in — can't build the planet mesh.
        // Fall back to a cube so an old scene referencing prim:planet still loads.
        (void)seed;
        LOG_WARN("resolveMesh: '{}' requested but planet support is not built "
                 "(no project procgen/Planet.h); substituting a cube", source);
        makeCube(v, i);
        return m_resourceCache.getOrCreateMesh(m_vulkanContext, "prim:cube", v, i);
#endif
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
        // Same dispatch as loadGltfScene: glTF/glb → cgltf, everything else → Assimp.
        // Otherwise a scene that saved an FBX entity (mesh source "gltf:foo.fbx#N")
        // would fail to reload because cgltf can't parse FBX.
        std::vector<GltfMeshData> datas;
        try {
            std::string ext = toLowerExt(path);
            if (ext == ".gltf" || ext == ".glb")
                datas = GltfLoader::load(path);
            else
                datas = AssimpLoader::loadImport(path).primitives;
        } catch (const std::exception& ex) {
            LOG_ERROR("resolveMesh: '{}' failed to load: {}", path, ex.what()); return nullptr;
        }
        if (idx < 0 || idx >= (int)datas.size()) { LOG_WARN("resolveMesh: '{}' has no primitive {}", path, idx); return nullptr; }
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
    params.subsurface      = mat.subsurface;

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
            else if (key == "metallic")   ss >> params.metallic;
            else if (key == "roughness")  ss >> params.roughness;
            else if (key == "subsurface") ss >> params.subsurface;
            else if (key == "albedo")     std::getline(ss, albedoRel);    // rest of line = path
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
    mat.subsurface      = params.subsurface;
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

// True when the 3D scene is the thing under (mx,my): inside the central area and
// not covered by the code editor (which only covers it when tabs are open and the
// editor⇄scene toggle is OFF). Gates the viewport right-click context menu.
bool Engine::cursorOverViewport(double mx, double my) const {
    const bool fs = m_window->isFullscreen();
    float winW = static_cast<float>(m_window->getWidth());
    float winH = static_cast<float>(m_window->getHeight());

    float left       = (!fs && m_contentBrowser.isVisible()) ? m_contentBrowser.currentWidth() : 0.0f;
    float rightDockW = fs ? 0.0f : (m_rightDockCollapsed ? RIGHT_DOCK_COLLAPSED_W : m_rightDockWidth);
    float midRight   = winW - rightDockW;
    float top        = TitleBar::BAR_HEIGHT;
    // When the editor panel is up, its tab bar occupies the top strip of the
    // central area — exclude it (in scene-view the scene shows below the bar).
    if (!fs && m_editor.isVisible()) top += CodeEditor::TABBAR_H;
    float bottom     = (!fs && m_console.isVisible()) ? (winH - m_console.currentHeight()) : winH;

    if (mx < left || mx >= midRight || my < top || my >= bottom) return false;
    // Editor with code open (and not toggled to scene-view) covers the scene.
    bool editorCoveringWithCode = m_editor.isVisible() && !m_editor.showingScene();
    return !editorCoveringWithCode;
}

void Engine::onViewportRightClick(double mx, double my) {
    if (!m_hierarchy.isVisible()) return;        // the hierarchy renders the menu
    if (!cursorOverViewport(mx, my))  return;

    // Pick the object under the cursor. If it isn't already in the selection,
    // select just it so Delete / Duplicate / etc. act on what was clicked. Empty
    // space leaves the selection as-is (Create still works; clipboard ops use it).
    float winW = static_cast<float>(m_window->getWidth());
    float winH = static_cast<float>(m_window->getHeight());
    Entity hit = pickEntity(mx, my, winW, winH);
    if (hit != NULL_ENTITY) {
        const auto& sel = m_hierarchy.selection();
        if (std::find(sel.begin(), sel.end(), hit) == sel.end())
            m_hierarchy.setSelection({hit});     // fires onSelect → m_selectedEntity
    }
    m_hierarchy.openContextMenuAt(mx, my);
}

void Engine::onViewportPress(double mx, double my) {
    if (m_gameMode) return;            // selection / gizmo is an editor control — never in-game
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
                if (m_vpAdditive) {
                    // Ctrl/Shift+click toggles the hit entity in the existing
                    // selection (no toggle on background clicks — those just
                    // keep the selection intact, matching Unity / Maya).
                    std::vector<Entity> sel = m_hierarchy.selection();
                    if (hit != NULL_ENTITY) {
                        auto it = std::find(sel.begin(), sel.end(), hit);
                        if (it != sel.end()) sel.erase(it);
                        else                 sel.push_back(hit);
                    }
                    m_hierarchy.setSelection(sel);
                } else {
                    m_hierarchy.setSelection(hit != NULL_ENTITY ? std::vector<Entity>{hit}
                                                                : std::vector<Entity>{});
                }
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

    // ── Point-light icons: a camera-facing sun glyph at each light's position ──
    // Replaces the old lit gizmo sphere (which read as a dark ball). Screen-space,
    // so it always faces the camera and is never shadowed. Editor-only (skipped in
    // fullscreen / the standalone play process, which never calls updateGizmo).
    std::vector<std::pair<glm::vec2, glm::vec4>> lightIcons;
    if (!m_window->isFullscreen()) {
        auto& lp = m_registry.pool<LightComponent>();
        for (size_t i = 0; i < lp.size(); ++i) {
            if (lp[i].type != LightComponent::Type::Point) continue;
            Entity e = lp.getEntity(i);
            if (!m_registry.has<TransformComponent>(e)) continue;
            glm::vec3 wp = glm::vec3(m_registry.get<TransformComponent>(e).worldMatrix[3]);
            glm::vec2 s;
            if (worldToScreen(wp, vp, winW, winH, s))
                lightIcons.push_back({ s, glm::vec4(lp[i].color, 1.0f) });
        }
    }

    m_gizmo.update(m_gizmoVisible, m_gizmoOrigin, m_gizmoTip, hoverAxis, marqueeActive, mq0, mq1, outlines, lightIcons);
}

void Engine::buildDefaultScene() {
    // A brand-new project opens as a clean slate: only the Environment
    // (sky / IBL / ambient) entity, no lights or meshes. The user adds their
    // own content via the hierarchy's create menu and the content browser.
    // Sky IBL still lights anything they add, so the viewport isn't black.
    ensureEnvironmentEntity();

    LOG_INFO("Default scene built (Environment only)");
}

Entity Engine::loadGltfScene(const std::string& filepath, const glm::vec3& position, float rootScale) {
    GltfImport imp;
    try {
        // Pick the loader by extension. glTF/glb → cgltf (skinning + anim).
        // Everything else assimp can read (.fbx/.dae/.obj/...) goes through
        // AssimpLoader, which yields the same GltfImport shape.
        std::string ext = toLowerExt(filepath);
        if (ext == ".gltf" || ext == ".glb")
            imp = GltfLoader::loadImport(filepath);
        else
            imp = AssimpLoader::loadImport(filepath);
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

    // Large FBX (gladiator SK_*.fbx are 20-72 MB) can take many seconds to
    // parse via Assimp on the main thread. Log on entry so the user can see
    // "this is loading" instead of thinking the editor froze.
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (!ec) LOG_INFO("spawnModel: loading '{}' ({} KB)…", path, sz / 1024);

    try {
        if (ext == ".obj") {
            std::string src = "obj:" + path;
            Mesh* mesh = resolveMesh(src);
            if (!mesh) { LOG_ERROR("spawnModel: failed to load {}", path); return; }
            created = createMeshEntity(mesh, m_resourceCache.getDefaultTexture(), position,
                                       {1,1,1}, {1,1,1,1}, 0.0f, 0.5f, src);
        } else if (ext == ".gltf" || ext == ".glb"
                || ext == ".fbx"  || ext == ".dae") {
            // Both glTF and assimp-handled formats end up in the same GltfImport
            // shape, so loadGltfScene takes either one.
            created = loadGltfScene(path, position);
        } else {
            LOG_WARN("spawnModel: '{}' is not a model (.obj/.gltf/.glb/.fbx/.dae)", path);
            return;
        }
    } catch (const std::exception& ex) {
        // Asset paths can throw deep inside the loader / texture cache / Vulkan
        // upload. Don't let a bad import kill the whole editor.
        LOG_ERROR("spawnModel: '{}' failed: {}", path, ex.what());
        return;
    } catch (...) {
        LOG_ERROR("spawnModel: '{}' failed (unknown exception)", path);
        return;
    }

    if (created != NULL_ENTITY) {
        pushSpawnAction({created});            // delta-based: undo just destroys this entity
        m_hierarchy.setSelection({created});   // fires onSelect → m_selectedEntity
        LOG_INFO("Spawned '{}' as entity {}", std::filesystem::path(path).filename().string(), created);
    }
}

void Engine::createCubeEntity() {
    Mesh* mesh = resolveMesh("prim:cube");
    if (!mesh) { LOG_WARN("createCubeEntity: cube mesh unavailable"); return; }
    glm::vec3 pos = entitiesPivot({});         // a point in front of the camera
    Entity e = createMeshEntity(mesh, m_resourceCache.getDefaultTexture(), pos,
                                {1,1,1}, {1,1,1,1}, 0.0f, 0.5f, "prim:cube");
    pushSpawnAction({e});                       // one Ctrl+Z removes it
    m_hierarchy.setSelection({e});
    LOG_INFO("Created cube (entity {})", e);
}

void Engine::createLightEntity(bool directional) {
    Entity e = m_registry.createEntity();
    LightComponent lc{};
    if (directional) {
        lc.type      = LightComponent::Type::Directional;
        lc.color     = {1.0f, 1.0f, 0.95f};
        lc.intensity = 1.0f;
        lc.direction = glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f));
        m_registry.assign<LightComponent>(e, lc);
    } else {
        TransformComponent tc{};
        tc.position = entitiesPivot({});        // in front of the camera
        m_registry.assign<TransformComponent>(e, tc);
        lc.type      = LightComponent::Type::Point;
        lc.color     = {1.0f, 0.9f, 0.7f};
        lc.intensity = 2.0f;
        lc.radius    = 8.0f;
        m_registry.assign<LightComponent>(e, lc);
        ensurePointLightGizmo(e);               // visible sphere marker + tinted material
    }
    pushSpawnAction({e});
    m_hierarchy.setSelection({e});
    LOG_INFO("Created {} light (entity {})", directional ? "directional" : "point", e);
}

void Engine::createPlanetEntity(uint32_t seed) {
    // The seed is baked into the source descriptor so the planet regenerates
    // identically on scene reload (resolveMesh parses it back out).
    std::string source = "prim:planet:" + std::to_string(seed);
    Mesh* mesh = resolveMesh(source);
    if (!mesh) { LOG_WARN("createPlanetEntity: planet mesh unavailable"); return; }

    // Place it well in front of the camera (it's ~radius 8 once scaled, so don't
    // drop it on top of the camera). White matte material → the per-vertex biome
    // colours come through as albedo, lit by the sun + sky IBL.
    glm::vec3 pos = m_camera.getPosition() + m_camera.getFront() * 30.0f;
    Entity e = createMeshEntity(mesh, m_resourceCache.getDefaultTexture(), pos,
                                {8.0f, 8.0f, 8.0f}, {1, 1, 1, 1}, 0.0f, 0.9f, source);
    pushSpawnAction({e});
    m_hierarchy.setSelection({e});
    LOG_INFO("Created planet (entity {}, seed {})", e, seed);
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
        // User-set name on its own line (whole rest-of-line = the name, so
        // spaces work). Read back via getline in readEntities.
        if (m_registry.has<NameComponent>(e)) {
            const auto& nc = m_registry.get<NameComponent>(e);
            if (!nc.name.empty()) os << "name " << nc.name << "\n";
        }
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
            if (m.subsurface > 0.0f)       os << "ssub " << m.subsurface << "\n";
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
               << (int)v.tonemapper << ' ' << v.exposure << ' '
               << (v.planetActive ? 1 : 0) << ' ' << v.planetSeed << ' ' << v.planetRadius << "\n";
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
        float me = 0.0f, ro = 0.5f, alpha = 0.0f, sub = 0.0f;
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
        params.subsurface       = pend.sub;

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
        mat.subsurface      = pend.sub;
        mat.albedoPath      = albedoTex ? pend.albedo : "";
        mat.albedoName      = albedoTex ? fs::path(pend.albedo).filename().string() : "";
        mat.normalPath      = nrmTex ? pend.normal     : "";
        mat.metalRoughPath  = mrTex  ? pend.metalrough : "";
        mat.occlusionPath   = aoTex  ? pend.occlusion  : "";
        mat.descriptorSet   = m_descriptors.allocateMaterialSet(m_vulkanContext, maps, params,
                                                                /*hostVisible=*/false, &mat.materialUBO);
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
        } else if (key == "ssub") {
            ss >> pend.sub;
        } else if (key == "name") {
            std::string n; std::getline(ss, n); n = trim(n);
            if (!n.empty()) m_registry.assign<NameComponent>(cur, NameComponent{n});
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
            // Optional trailing planet fields (absent in older scenes → keep defaults).
            int pa = 0; uint32_t pseed = 0; float prad = 1500.0f;
            if (ss >> pa)    v.planetActive = (pa != 0);
            if (ss >> pseed) v.planetSeed   = pseed;
            if (ss >> prad)  v.planetRadius  = prad;
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
    if (a.kind == UndoAction::Kind::Group) {
        // Group deltas don't persist across sessions yet — the entity IDs they
        // reference are only stable within the live registry. Emit an empty
        // marker so the file exists; loadUndoHistoryFromDisk skips it.
        f << "group-volatile\n";
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
        case UndoAction::Kind::Group: {
            if (forward) {
                // Redo: recreate group, re-apply child reparents. The new ID may
                // differ from the original — update the action so undo can find it.
                Entity g = m_registry.createEntity();
                TransformComponent gtc{};
                gtc.position = a.groupPosition;
                gtc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                gtc.scale    = glm::vec3(1.0f);
                gtc.parent   = NULL_ENTITY;
                m_registry.assign<TransformComponent>(g, gtc);
                a.groupEntity = g;
                for (auto& cr : a.childReparents) {
                    if (!m_registry.has<TransformComponent>(cr.entity)) continue;
                    auto& tc = m_registry.get<TransformComponent>(cr.entity);
                    tc.position = cr.newPosition;
                    tc.parent   = g;
                    cr.newParent = g;
                }
                m_hierarchy.expandGroup(g);
                m_hierarchy.setSelection({g});
                m_inspector.setSelectionGroup({g});
                m_selectedEntity = g;
            } else {
                // Undo: restore each child's old parent + local TRS, then destroy
                // the group entity. Iterate in reverse so deeply-nested reparents
                // peel off cleanly.
                for (auto it = a.childReparents.rbegin(); it != a.childReparents.rend(); ++it) {
                    if (!m_registry.has<TransformComponent>(it->entity)) continue;
                    auto& tc = m_registry.get<TransformComponent>(it->entity);
                    tc.position = it->oldPosition;
                    tc.rotation = it->oldRotation;
                    tc.scale    = it->oldScale;
                    tc.parent   = it->oldParent;
                }
                if (a.groupEntity != NULL_ENTITY) {
                    m_registry.destroyEntity(a.groupEntity);
                    if (m_selectedEntity == a.groupEntity) m_selectedEntity = NULL_ENTITY;
                }
                // Restore selection to the original targets so the user picks up
                // where they were before the group operation.
                std::vector<Entity> orig;
                orig.reserve(a.childReparents.size());
                for (const auto& cr : a.childReparents) orig.push_back(cr.entity);
                m_hierarchy.setSelection(orig);
                m_inspector.setSelectionGroup(orig);
                m_selectedEntity = orig.empty() ? NULL_ENTITY : orig.back();
            }
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
        case SF::MaterialSubsurface:
            return m_registry.has<MaterialComponent>(e) ? m_registry.get<MaterialComponent>(e).subsurface : 0.0f;
        case SF::MaterialMetallic:
            return m_registry.has<MaterialComponent>(e) ? m_registry.get<MaterialComponent>(e).metallic : 0.0f;
        case SF::MaterialRoughness:
            return m_registry.has<MaterialComponent>(e) ? m_registry.get<MaterialComponent>(e).roughness : 0.0f;
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
        case SF::MaterialSubsurface:
            if (m_registry.has<MaterialComponent>(e)) {
                m_registry.get<MaterialComponent>(e).subsurface = v;
                // Undo/redo and any direct write must reach the GPU material UBO.
                reuploadMaterialParams(e);
            }
            return;
        case SF::MaterialMetallic:
            if (m_registry.has<MaterialComponent>(e)) {
                m_registry.get<MaterialComponent>(e).metallic = v;
                reuploadMaterialParams(e);
            }
            return;
        case SF::MaterialRoughness:
            if (m_registry.has<MaterialComponent>(e)) {
                m_registry.get<MaterialComponent>(e).roughness = v;
                reuploadMaterialParams(e);
            }
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

void Engine::reuploadMaterialParams(Entity e) {
    if (e == NULL_ENTITY || !m_registry.has<MaterialComponent>(e)) return;
    auto& mat = m_registry.get<MaterialComponent>(e);
    if (!mat.materialUBO) return;   // no captured UBO (e.g. legacy/gizmo material)
    // Rebuild the full param block from the component. The map-presence flags are
    // derived from the stored paths (the canonical source, same as flushMat).
    MaterialParams params{};
    params.baseColorFactor  = mat.baseColorFactor;
    params.metallic         = mat.metallic;
    params.roughness        = mat.roughness;
    params.hasNormalMap     = mat.normalPath.empty()     ? 0.0f : 1.0f;
    params.hasMetalRoughMap = mat.metalRoughPath.empty() ? 0.0f : 1.0f;
    params.alphaCutoff      = mat.alphaCutoff;
    params.subsurface       = mat.subsurface;
    // The UBO may be referenced by an in-flight command buffer; wait before the
    // staging copy overwrites it (same discipline as assignMaterialToSelected).
    m_renderer.waitIdle(m_vulkanContext.getDevice());
    m_descriptors.reuploadMaterialParams(m_vulkanContext, mat.materialUBO, params);
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

void Engine::groupSelected() {
    auto selection = m_hierarchy.selection();
    if (selection.empty()) {
        LOG_INFO("Group: nothing selected");
        return;
    }

    // Filter to entities with a transform so the centroid math is well-defined.
    // A single item is fine — it becomes the only child of the new group, which
    // is still useful for organising the hierarchy.
    std::vector<Entity> targets;
    targets.reserve(selection.size());
    for (Entity e : selection)
        if (e != NULL_ENTITY && m_registry.has<TransformComponent>(e)) targets.push_back(e);
    if (targets.empty()) {
        LOG_INFO("Group: no transformable entities in selection");
        return;
    }

    // Ensure world matrices are current before reading them. The TransformSystem
    // runs each frame, but groupSelected can fire mid-frame between a transform
    // mutation and the next tick.
    TransformSystem::update(m_registry);

    // Centre the group on the **union world-space AABB** of the targets — same
    // logic the camera-frame and gizmo pivot use. Averaging the entities'
    // pivot positions instead would land the group at the base of meshes whose
    // pivot sits at the foot (e.g. the gladiator's body part), not the visual
    // centre.
    auto worldPos = [this](Entity e) {
        return glm::vec3(m_registry.get<TransformComponent>(e).worldMatrix[3]);
    };
    glm::vec3 centroid = entitiesPivot(targets);

    // Build a delta-style undo entry: remember each target's old (parent, local
    // TRS) so undo can put them back exactly where they were. The group entity
    // is also recreated by the undo system from groupSerialized (mirrors how
    // Kind::Spawn works), so ID continuity is preserved across redo cycles.
    UndoAction action;
    action.kind = UndoAction::Kind::Group;

    // Empty parent entity at the world centroid.
    Entity group = m_registry.createEntity();
    TransformComponent gtc{};
    gtc.position = centroid;
    gtc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    gtc.scale    = glm::vec3(1.0f);
    gtc.parent   = NULL_ENTITY;
    m_registry.assign<TransformComponent>(group, gtc);

    // For each target: capture old TRS+parent, then re-express the world
    // position in the new group's frame (G^-1 · W). G is translation-only here,
    // so the local position is just (world - centroid) — and rotation/scale
    // pass through unchanged.
    action.groupEntity   = group;
    action.groupPosition = centroid;
    action.childReparents.reserve(targets.size());
    for (Entity e : targets) {
        auto& tc = m_registry.get<TransformComponent>(e);
        UndoAction::ChildReparent cr{};
        cr.entity      = e;
        cr.oldParent   = tc.parent;
        cr.oldPosition = tc.position;
        cr.oldRotation = tc.rotation;
        cr.oldScale    = tc.scale;
        cr.newParent   = group;
        cr.newPosition = worldPos(e) - centroid;

        tc.position = cr.newPosition;
        tc.parent   = cr.newParent;
        // Rotation/scale unchanged — group has identity rot/scale.
        action.childReparents.push_back(cr);
    }

    pushAction(std::move(action));

    // Auto-expand the new group so the user can immediately see its children.
    m_hierarchy.expandGroup(group);

    // Select the new group so the user can move it immediately.
    m_hierarchy.setSelection({group});
    m_inspector.setSelectionGroup({group});
    m_selectedEntity = group;

    m_sceneDirty = true;
    LOG_INFO("Grouped {} entities under new entity {}", targets.size(), group);
}

void Engine::ungroupSelected() {
    auto selection = m_hierarchy.selection();
    if (selection.empty()) { LOG_INFO("Ungroup: nothing selected"); return; }

    // A "group" here is an entity with a TransformComponent but no Mesh / Light /
    // Environment — i.e. one of the empty parents that groupSelected() created.
    auto isGroup = [this](Entity e) {
        if (e == NULL_ENTITY || !m_registry.has<TransformComponent>(e)) return false;
        if (m_registry.has<MeshComponent>(e))        return false;
        if (m_registry.has<LightComponent>(e))       return false;
        if (m_registry.has<EnvironmentComponent>(e)) return false;
        return true;
    };

    // Collect target groups + their children before we mutate anything (the pool
    // changes underneath if we destroy entities mid-iteration).
    struct Job { Entity group; std::vector<Entity> children; };
    std::vector<Job> jobs;
    std::unordered_set<Entity> selSet(selection.begin(), selection.end());
    auto& tfPool = m_registry.pool<TransformComponent>();
    for (Entity g : selection) {
        if (!isGroup(g)) continue;
        Job j; j.group = g;
        for (size_t i = 0; i < tfPool.size(); ++i) {
            Entity e = tfPool.getEntity(i);
            if (tfPool[i].parent == g) j.children.push_back(e);
        }
        jobs.push_back(std::move(j));
    }
    if (jobs.empty()) {
        LOG_INFO("Ungroup: no groups in selection (selection items must be empty parents)");
        return;
    }

    pushUndo();

    std::vector<Entity> orphaned;
    for (auto& j : jobs) {
        const auto& gtc = m_registry.get<TransformComponent>(j.group);
        Entity newParent = gtc.parent;   // hoist children up one level (root if none)
        // Promote children: their local position absorbs the group's local
        // position so their world position is preserved. Rotation/scale of the
        // group are identity from groupSelected(), but be defensive in case the
        // user has scrubbed the group's transform.
        for (Entity c : j.children) {
            auto& ctc = m_registry.get<TransformComponent>(c);
            ctc.position = gtc.position + gtc.rotation * (gtc.scale * ctc.position);
            ctc.rotation = gtc.rotation * ctc.rotation;
            ctc.scale    = gtc.scale * ctc.scale;
            ctc.parent   = newParent;
            orphaned.push_back(c);
        }
        m_registry.destroyEntity(j.group);
    }

    // Selecting the orphaned children gives the user something to act on next.
    m_hierarchy.setSelection(orphaned);
    m_inspector.setSelectionGroup(orphaned);
    m_selectedEntity = orphaned.empty() ? NULL_ENTITY : orphaned.back();

    m_sceneDirty = true;
    LOG_INFO("Ungrouped {} group(s) → {} children", jobs.size(), orphaned.size());
}

void Engine::loadEditorPrefs() {
    std::ifstream f(m_projectPath + "/editor.prefs");
    if (!f) return;
    std::set<std::string> openFolders;
    std::string line;
    while (std::getline(f, line)) {
        // Split into key + remainder. Single-token keys use the remainder as a
        // single value; "browserOpenFolder" uses the whole remainder as a path
        // (paths can contain spaces).
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string key   = line.substr(0, sp);
        std::string value = line.substr(sp + 1);
        // strip leading spaces from value
        size_t vs = value.find_first_not_of(' ');
        if (vs != std::string::npos) value = value.substr(vs);

        auto asInt   = [&]{ try { return std::stoi(value); }   catch (...) { return 0; } };
        auto asFloat = [&]{ try { return std::stof(value); }   catch (...) { return 0.0f; } };

        if      (key == "windowFullscreen")   m_pendingWindow.fullscreen = (asInt() != 0);
        else if (key == "windowMaximized")    m_pendingWindow.maximized = (asInt() != 0);
        else if (key == "windowWidth")        m_pendingWindow.w = asInt();
        else if (key == "windowHeight")       m_pendingWindow.h = asInt();
        else if (key == "rightDockCollapsed") m_rightDockCollapsed = (asInt() != 0);
        else if (key == "rightDockWidth")     m_rightDockWidth     = asFloat();
        else if (key == "hierarchyHeight")    m_hierarchyHeight    = asFloat();
        else if (key == "browserExpanded")    m_contentBrowser.setExpanded(asInt() != 0);
        else if (key == "browserWidth")       m_contentBrowser.setPanelWidth(asFloat());
        else if (key == "consoleExpanded")    m_console.setExpanded(asInt() != 0);
        else if (key == "consoleHeight")      m_console.setPanelHeight(asFloat());
        else if (key == "browserOpenFolder")  openFolders.insert(value);
        else if (key == "camera") {
            // "camera px py pz yaw pitch fov" — buffered now, applied after the
            // camera is initialised (loadEditorPrefs runs early in startup).
            std::istringstream cs(value);
            glm::vec3 p; float yaw, pitch, fov;
            if (cs >> p.x >> p.y >> p.z >> yaw >> pitch >> fov) {
                m_pendingCameraPose.has      = true;
                m_pendingCameraPose.position = p;
                m_pendingCameraPose.yaw      = yaw;
                m_pendingCameraPose.pitch    = pitch;
                m_pendingCameraPose.fov      = fov;
            }
        }
    }
    if (!openFolders.empty()) m_contentBrowser.setExpandedFolders(openFolders);
}

void Engine::saveEditorPrefs() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(m_projectPath, ec);
    std::ofstream f(m_projectPath + "/editor.prefs");
    if (!f) return;
    // Persist the windowed size from the tracked plain-windowed dims (so a session
    // spent fullscreen/maximized doesn't overwrite it with the monitor size).
    int savedW = (m_windowedW > 0) ? m_windowedW : m_window->getWidth();
    int savedH = (m_windowedH > 0) ? m_windowedH : m_window->getHeight();
    f << "windowFullscreen "   << (m_window->isFullscreen() ? 1 : 0) << "\n"
      << "windowMaximized "    << (m_window->isEffectivelyMaximized() ? 1 : 0) << "\n"
      << "windowWidth "        << savedW << "\n"
      << "windowHeight "       << savedH << "\n"
      << "rightDockCollapsed " << (m_rightDockCollapsed ? 1 : 0) << "\n"
      << "rightDockWidth "     << m_rightDockWidth                << "\n"
      << "hierarchyHeight "    << m_hierarchyHeight               << "\n"
      << "browserExpanded "    << (m_contentBrowser.isExpanded() ? 1 : 0) << "\n"
      << "browserWidth "       << m_contentBrowser.panelWidth()   << "\n"
      << "consoleExpanded "    << (m_console.isExpanded() ? 1 : 0) << "\n"
      << "consoleHeight "      << m_console.panelHeight()         << "\n";
    {
        glm::vec3 p = m_camera.getPosition();
        f << "camera " << p.x << ' ' << p.y << ' ' << p.z << ' '
          << m_camera.getYaw() << ' ' << m_camera.getPitch() << ' '
          << m_camera.getFov() << "\n";
    }
    // Window pos/size persistence intentionally not written — see the
    // comment in Engine::initialize() at the Window constructor.
    // One line per open folder — getline-friendly so paths with spaces work.
    for (const std::string& p : m_contentBrowser.expandedFolders())
        f << "browserOpenFolder " << p << "\n";
}

void Engine::switchProject(const std::string& newPath) {
    namespace fs = std::filesystem;
    std::string normalized = fs::path(newPath).generic_string();
    if (normalized.empty()) return;
    if (normalized == m_projectPath) return;   // no-op

    LOG_INFO("Switching project: '{}' -> '{}'", m_projectPath, normalized);

    // The save/clear/load below blocks the main thread for several seconds on
    // a real project (texture decodes alone). Throw up the same splash window
    // we use at engine startup so the user sees progress instead of a frozen
    // editor. Reuse m_statusFn so the existing status pings inside loadScene
    // / loadGltfScene / loadUndoHistoryFromDisk pipe through automatically.
    Nyx::Splash splash;
    splash.show();
    auto status = [&](const char* s, float p) { splash.setStatus(s, p); };
    m_statusFn = [&](const std::string& s, float p) { splash.setStatus(s, p); };

    // 1. Persist current project's state before we lose it.
    status("Saving current project...", 0.05f);
    saveCurrentScene();
    saveEditorPrefs();

    // 2. Tear down in-memory state belonging to the current project. waitIdle
    //    inside clearScene ensures the GPU isn't still using anything we drop.
    status("Closing open files...", 0.15f);
    m_editor.closePath(m_projectPath);    // closes every tab whose path lives under the old project

    status("Clearing scene...", 0.25f);
    clearScene();
    for (const auto& a : m_undoStack) { std::error_code ec; fs::remove(a.path, ec); }
    for (const auto& a : m_redoStack) { std::error_code ec; fs::remove(a.path, ec); }
    m_undoStack.clear();
    m_redoStack.clear();
    m_currentScenePath.clear();
    m_sceneDirty = false;

    // 3. Repoint and refresh.
    status("Opening project...", 0.35f);
    m_projectPath = normalized;
    m_contentBrowser.setProject(m_projectPath);

    // 4. Load the new project (scene + prefs + undo). Mirrors Engine::init's
    //    project-loading block so the experience matches a fresh launch.
    loadEditorPrefs();
    std::string projectScene = m_projectPath + "/scenes/scene.scene";
    if (fs::exists(projectScene)) {
        status("Loading scene...", 0.5f);
        loadScene(projectScene);
    } else {
        status("Building default scene...", 0.5f);
        buildDefaultScene();
    }
    status("Restoring undo history...", 0.95f);
    loadUndoHistoryFromDisk();
    status("Ready.", 1.0f);

    // 5. Remember the choice across sessions.
    writeLastProjectPath(m_projectPath);

    m_statusFn = nullptr;
    splash.close();
}

void Engine::saveProjectAs() {
    namespace fs = std::filesystem;

    // Flush the current project to disk first so the copy is current.
    m_editor.saveAll();
    saveCurrentScene();
    saveEditorPrefs();

    // Ask where to put the copy. The user picks a PARENT folder; we create a new
    // subfolder named after the current project inside it. Steer the picker to the
    // engine's projects/ root so siblings are the obvious destination.
    fs::path curr   = fs::path(m_projectPath);
    std::string projName = curr.filename().string();
    if (projName.empty()) projName = "Project";
    fs::path parentOfCurr = curr.parent_path();
    std::string startDir = parentOfCurr.empty() ? std::string("projects")
                                                : parentOfCurr.generic_string();
    std::string destParent = m_window->openFolderDialog(
        "Save Project As — pick a destination folder", startDir);
    if (destParent.empty()) return;   // cancelled

    // Choose a non-colliding destination: <destParent>/<projName>, then append
    // " Copy", " Copy 2", … if something is already there. Never overwrite.
    fs::path dest = fs::path(destParent) / projName;
    if (fs::exists(dest)) {
        dest = fs::path(destParent) / (projName + " Copy");
        for (int n = 2; fs::exists(dest); ++n)
            dest = fs::path(destParent) / (projName + " Copy " + std::to_string(n));
    }
    // Guard against copying a project into itself (dest under source).
    {
        std::error_code ec;
        fs::path s = fs::weakly_canonical(curr, ec);
        fs::path d = fs::weakly_canonical(dest, ec);
        std::string ss = s.generic_string(), ds = d.generic_string();
        if (!ss.empty() && ds.rfind(ss + "/", 0) == 0) {
            LOG_ERROR("Save As: destination '{}' is inside the source project — aborting", ds);
            return;
        }
    }

    std::error_code ec;
    fs::copy(curr, dest, fs::copy_options::recursive, ec);
    if (ec) {
        LOG_ERROR("Save As: copy '{}' -> '{}' failed: {}",
                  curr.generic_string(), dest.generic_string(), ec.message());
        return;
    }
    LOG_INFO("Save As: copied project to {}", dest.generic_string());

    // Switch to the copy so the user is now editing it (matches Open Project UX).
    switchProject(dest.generic_string());
}

void Engine::exportGame(const std::string& destParentArg) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Flush current state so the export reflects exactly what's on screen — incl.
    // editor.prefs, which carries the camera pose. The exported game has no game
    // camera of its own yet, so it reuses that saved pose to frame the scene the
    // way you set it up; without it the game opens at the default camera and your
    // objects can be off-screen.
    m_editor.saveAll();
    saveCurrentScene();
    // Flush editor.prefs (camera pose) only for an interactive menu export — there
    // the window is in a real maximized/windowed state. A headless --export has no
    // shown window, so saving here would write a bogus windowMaximized and clobber
    // the project's saved window state; it just copies the prefs already on disk
    // (which already carry the camera from the editing session).
    if (destParentArg.empty()) saveEditorPrefs();

    std::string projName = fs::path(m_projectPath).filename().string();
    if (projName.empty()) projName = "Game";

    // Pick a destination parent; we create <dest>/<projName>/ inside it. A caller
    // may pass one directly (e.g. the --export CLI path); otherwise ask the user.
    std::string destParent = destParentArg.empty()
        ? m_window->openFolderDialog("Export Game — pick a destination folder", "")
        : destParentArg;
    if (destParent.empty()) return;   // cancelled

    fs::path out = fs::path(destParent) / projName;
    if (fs::exists(out)) {
        out = fs::path(destParent) / (projName + " Export");
        for (int n = 2; fs::exists(out); ++n)
            out = fs::path(destParent) / (projName + " Export " + std::to_string(n));
    }
    // Guard: never export into the project itself (dest under source).
    {
        fs::path ps = fs::weakly_canonical(fs::path(m_projectPath), ec);
        fs::path os = fs::weakly_canonical(out, ec);
        std::string pss = ps.generic_string(), oss = os.generic_string();
        if (!pss.empty() && oss.rfind(pss + "/", 0) == 0) {
            LOG_ERROR("Export: destination '{}' is inside the project — aborting", oss);
            return;
        }
    }

    LOG_INFO("Export: building '{}' … (copying assets can take a moment)", out.generic_string());
    fs::create_directories(out, ec);

    // 0) game.cfg FIRST (tiny) — its presence is what makes the copied exe boot
    //    into the game instead of the editor. Writing it before the (potentially
    //    large/slow) asset copy means an interrupted export still produces a folder
    //    recognised as a game, not one that silently opens the editor. Path is
    //    relative to the export root.
    {
        std::string sceneRel = fs::relative(m_currentScenePath, m_projectPath, ec).generic_string();
        if (ec || sceneRel.empty() || sceneRel.rfind("..", 0) == 0) sceneRel = "scenes/scene.scene";
        ec.clear();
        std::ofstream cfg(out / "game.cfg");
        cfg << "scene projects/"   << projName << "/" << sceneRel << "\n";
        cfg << "project projects/" << projName << "\n";
    }

    // 1) The running executable. Export from the RELEASE build for a shippable
    //    game — a Debug exe requests Vulkan validation layers a player won't have.
    {
        wchar_t exeBuf[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) { LOG_ERROR("Export: cannot resolve exe path"); return; }
        fs::copy_file(fs::path(std::wstring(exeBuf, n)), out / (projName + ".exe"),
                      fs::copy_options::overwrite_existing, ec);
        if (ec) { LOG_ERROR("Export: copy exe failed: {}", ec.message()); return; }
#ifndef NDEBUG
        LOG_WARN("Export: this is a DEBUG build — the exported game needs the Vulkan "
                 "SDK to run. Re-export from the Release build before shipping.");
#endif
    }

    // 2) Compiled shaders (runtime loads shaders/*.spv by relative path).
    fs::copy("shaders", out / "shaders",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    ec.clear();

    // 3) Project content, kept at the SAME relative path the scene's asset
    //    references use (projects/<name>/…) so nothing needs rewriting. Then prune
    //    the editor-only bits (undo history, prefs, scene backups).
    fs::path projDst = out / "projects" / projName;
    // fs::copy does NOT create intermediate parents — make the full destination
    // path first, then copy the project's contents into it. (Without this the copy
    // failed with "cannot find the path specified", which left the export with no
    // game content — and, in the old ordering, no game.cfg → it opened the editor.)
    fs::create_directories(projDst, ec); ec.clear();
    fs::copy(m_projectPath, projDst,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) { LOG_ERROR("Export: copy project failed: {}", ec.message()); return; }
    fs::remove_all(projDst / "scenes" / ".history", ec);   ec.clear();
    fs::remove(projDst / "scenes" / "scene.scene.bak", ec); ec.clear();
    // Keep editor.prefs: the game reads the saved camera pose from it to frame
    // the scene (the only "editor" bit the runtime actually uses).

    LOG_INFO("Export: done → {}  (double-click {}.exe to play)",
             out.generic_string(), projName);
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
