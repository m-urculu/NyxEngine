#include "Engine.h"
#include "Logger.h"
#include "Input.h"
#include "renderer/Vertex.h"
#include "renderer/UniformTypes.h"
#include "renderer/ObjLoader.h"
#include "renderer/GltfLoader.h"
#include "renderer/Texture.h"
#include "renderer/Mesh.h"
#include "ecs/components/TransformComponent.h"
#include "ecs/components/MeshComponent.h"
#include "ecs/components/MaterialComponent.h"
#include "ecs/components/LightComponent.h"
#include "ecs/systems/TransformSystem.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stdexcept>
#include <filesystem>

namespace Talos {

Engine::Engine() = default;

Engine::~Engine() {
    m_renderer.waitIdle(m_vulkanContext.getDevice());

    m_titleBar.cleanup(m_vulkanContext.getAllocator());
    m_uiPipeline.cleanup(m_vulkanContext.getDevice());
    m_resourceCache.cleanup(m_vulkanContext);
    m_renderer.cleanup(m_vulkanContext.getDevice());
    m_pipeline.cleanup(m_vulkanContext.getDevice());
    m_descriptors.cleanup(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());
    m_swapchain.cleanup(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());
    m_vulkanContext.cleanup();

    LOG_INFO("Engine shut down");
}

void Engine::init() {
    Logger::init();
    LOG_INFO("=== Talos v0.3.0 (Phase 3B) ===");

    m_window = std::make_unique<Window>("Talos", 1280, 720);

    m_vulkanContext.init(m_window->getHandle());

    m_swapchain.init(m_vulkanContext, m_window->getWidth(), m_window->getHeight());

    m_descriptors.init(m_vulkanContext);

    m_pipeline.init(m_vulkanContext, m_swapchain.getExtent(), m_swapchain.getImageFormat(),
                     m_swapchain.getDepthBuffer().getFormat(),
                     m_descriptors.getGlobalLayout(), m_descriptors.getMaterialLayout());

    m_renderer.init(m_vulkanContext, m_swapchain, m_pipeline);

    m_resourceCache.init(m_vulkanContext);

    m_uiPipeline.init(m_vulkanContext, m_pipeline.getRenderPass());
    m_titleBar.init(m_vulkanContext.getAllocator(), m_window->getHandle());

    Input::init(m_window->getHandle(), m_window.get());
    Input::setTitleBar(&m_titleBar);

    float aspect = static_cast<float>(m_window->getWidth()) / static_cast<float>(m_window->getHeight());
    m_camera.init({0.0f, 2.0f, 6.0f}, aspect);

    m_time.init();

    buildDemoScene();

    LOG_INFO("Engine initialized successfully!");
}

void Engine::run() {
    LOG_INFO("Entering main loop");

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
                          static_cast<float>(m_window->getHeight()), cursorFree);
        if (cursorFree) {
            m_titleBar.handleDragResize();
            m_titleBar.updateCursorShape();
        }

        // Update UBO with current camera matrices
        updateUniformBuffer(m_renderer.getCurrentFrame());

        bool ok = m_renderer.drawFrame(m_vulkanContext, m_swapchain, m_pipeline,
                                        m_registry, m_descriptors,
                                        &m_uiPipeline, &m_titleBar);
        if (!ok) {
            handleResize();
        }

        // FPS counter
        m_fpsCount++;
        m_fpsTimer += m_time.getDeltaTime();
        if (m_fpsTimer >= 2.0f) {
            float fps = static_cast<float>(m_fpsCount) / m_fpsTimer;
            LOG_INFO("FPS: {:.1f}", fps);
            m_fpsCount = 0;
            m_fpsTimer = 0.0f;
        }
    }

    LOG_INFO("Main loop ended");
}

void Engine::fixedUpdate(float dt) {
    m_camera.update(dt);

    // Orbit the child entity around its parent
    if (m_orbitEntity != NULL_ENTITY && m_registry.has<TransformComponent>(m_orbitEntity)) {
        m_orbitAngle += dt * 1.5f; // radians per second
        auto& tc = m_registry.get<TransformComponent>(m_orbitEntity);
        float radius = 2.5f;
        tc.position = {radius * cosf(m_orbitAngle), 0.5f, radius * sinf(m_orbitAngle)};
    }

    TransformSystem::update(m_registry);
}

void Engine::handleResize() {
    int width = m_window->getWidth();
    int height = m_window->getHeight();
    if (width == 0 || height == 0) return;

    LOG_INFO("Handling resize to {}x{}", width, height);

    m_renderer.waitIdle(m_vulkanContext.getDevice());

    m_swapchain.recreate(m_vulkanContext, width, height);
    m_pipeline.recreate(m_vulkanContext, m_swapchain.getExtent(), m_swapchain.getImageFormat(),
                         m_swapchain.getDepthBuffer().getFormat(),
                         m_descriptors.getGlobalLayout(), m_descriptors.getMaterialLayout());
    m_uiPipeline.recreate(m_vulkanContext, m_pipeline.getRenderPass());
    m_renderer.recreateFramebuffers(m_vulkanContext.getDevice(), m_swapchain, m_pipeline);

    m_camera.setAspectRatio(static_cast<float>(width) / static_cast<float>(height));
}

void Engine::updateUniformBuffer(uint32_t currentFrame) {
    UniformBufferObject ubo{};
    ubo.view       = m_camera.getViewMatrix();
    ubo.projection = m_camera.getProjectionMatrix();

    ubo.ambientColor   = glm::vec4(0.15f, 0.15f, 0.15f, 1.0f);
    ubo.cameraPosition = glm::vec4(m_camera.getPosition(), 1.0f);

    // Pack lights from ECS into UBO
    int lightIndex = 0;
    auto& lightPool = m_registry.pool<LightComponent>();
    for (size_t i = 0; i < lightPool.size() && lightIndex < MAX_LIGHTS; i++) {
        Entity entity = lightPool.getEntity(i);
        const LightComponent& lc = lightPool[i];

        GpuLightData& gpu = ubo.lights[lightIndex];
        gpu.colorAndIntensity = glm::vec4(lc.color, lc.intensity);
        gpu.params = glm::vec4(lc.radius, 0.0f, 0.0f, 0.0f);

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

    m_descriptors.getUniformBuffer(currentFrame).uploadData(
        m_vulkanContext.getAllocator(), &ubo, sizeof(ubo));
}

Entity Engine::createMeshEntity(Mesh* mesh, Texture* texture,
                                 const glm::vec3& position, const glm::vec3& scale,
                                 const glm::vec4& baseColorFactor,
                                 float metallic, float roughness) {
    Entity e = m_registry.createEntity();

    TransformComponent tc{};
    tc.position = position;
    tc.scale = scale;
    m_registry.assign<TransformComponent>(e, tc);

    MeshComponent mc{};
    mc.mesh = mesh;
    m_registry.assign<MeshComponent>(e, mc);

    MaterialParams params{};
    params.baseColorFactor = baseColorFactor;
    params.metallic  = metallic;
    params.roughness = roughness;

    MaterialComponent mat{};
    mat.texture = texture;
    mat.baseColorFactor = baseColorFactor;
    mat.metallic  = metallic;
    mat.roughness = roughness;
    mat.descriptorSet = m_descriptors.allocateMaterialSet(
        m_vulkanContext.getDevice(), m_vulkanContext.getAllocator(), *texture, params);
    m_registry.assign<MaterialComponent>(e, mat);

    return e;
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

    // 1. OBJ Cube (backward compatibility)
    Mesh* cubeMesh = m_resourceCache.getOrCreateMeshFromOBJ(m_vulkanContext, "assets/models/cube.obj");
    Entity cube = createMeshEntity(cubeMesh, defaultTex, {0.0f, 0.5f, 0.0f});
    (void)cube;

    // 2. Orbiting child cube
    m_orbitEntity = createMeshEntity(cubeMesh, defaultTex, {2.5f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.5f});
    m_registry.get<TransformComponent>(m_orbitEntity).parent = cube;

    // 3. Floor quad
    std::vector<Vertex> floorVerts = {
        {{-5.0f, 0.0f, -5.0f}, {0.0f, 1.0f, 0.0f}, {0.4f, 0.4f, 0.4f}, {0.0f, 0.0f}},
        {{ 5.0f, 0.0f, -5.0f}, {0.0f, 1.0f, 0.0f}, {0.4f, 0.4f, 0.4f}, {1.0f, 0.0f}},
        {{ 5.0f, 0.0f,  5.0f}, {0.0f, 1.0f, 0.0f}, {0.4f, 0.4f, 0.4f}, {1.0f, 1.0f}},
        {{-5.0f, 0.0f,  5.0f}, {0.0f, 1.0f, 0.0f}, {0.4f, 0.4f, 0.4f}, {0.0f, 1.0f}},
    };
    std::vector<uint32_t> floorIndices = {0, 1, 2, 2, 3, 0};
    Mesh* floorMesh = m_resourceCache.getOrCreateMesh(m_vulkanContext, "__floor", floorVerts, floorIndices);
    createMeshEntity(floorMesh, defaultTex, {0.0f, 0.0f, 0.0f});

    // 4. Try to load a glTF model if one exists (prefer .gltf with separate textures)
    auto findGltf = [](const std::string& base) -> std::string {
        // Prefer .gltf (has URI texture refs) over .glb (embedded)
        for (const auto& ext : {".gltf", ".glb"}) {
            std::string path = base + ext;
            if (std::filesystem::exists(path)) return path;
        }
        return "";
    };

    std::string gltfPath;
    if (gltfPath.empty()) gltfPath = findGltf("assets/models/DamagedHelmet/DamagedHelmet");
    if (gltfPath.empty()) gltfPath = findGltf("assets/models/DamagedHelmet");
    if (gltfPath.empty()) gltfPath = findGltf("assets/models/BoxTextured/BoxTextured");
    if (gltfPath.empty()) gltfPath = findGltf("assets/models/BoxTextured");

    if (!gltfPath.empty()) {
        loadGltfScene(gltfPath);
    } else {
        LOG_INFO("No glTF model found in assets/models/ — creating extra cubes for demo");
        // Extra cubes at different positions to show multi-object rendering
        createMeshEntity(cubeMesh, defaultTex, {-3.0f, 0.5f, -2.0f}, {0.7f, 0.7f, 0.7f});
        createMeshEntity(cubeMesh, defaultTex, { 3.0f, 1.0f, -1.0f}, {0.6f, 1.2f, 0.6f});
    }

    LOG_INFO("Demo scene built with {} entities", m_registry.pool<MeshComponent>().size());
}

void Engine::loadGltfScene(const std::string& filepath) {
    auto meshDatas = GltfLoader::load(filepath);

    // Resolve base directory for texture paths
    std::string baseDir = filepath.substr(0, filepath.find_last_of("/\\") + 1);

    for (size_t i = 0; i < meshDatas.size(); i++) {
        auto& md = meshDatas[i];
        std::string meshKey = filepath + "#" + std::to_string(i);

        Mesh* mesh = m_resourceCache.getOrCreateMesh(m_vulkanContext, meshKey, md.vertices, md.indices);

        Texture* tex = m_resourceCache.getDefaultTexture();
        if (!md.baseColorTextureURI.empty()) {
            std::string texPath = baseDir + md.baseColorTextureURI;
            if (std::filesystem::exists(texPath)) {
                tex = m_resourceCache.getOrCreateTexture(m_vulkanContext, texPath);
            } else {
                LOG_WARN("Texture not found: {}", texPath);
            }
        }

        createMeshEntity(mesh, tex, {0.0f, 1.5f, -2.0f}, {1,1,1},
                         md.baseColorFactor, md.metallicFactor, md.roughnessFactor);
    }
}

} // namespace Talos
