#pragma once

// Engine.h — Main engine class with ECS, fixed timestep, and resource management

#include "Window.h"
#include "renderer/VulkanContext.h"
#include "renderer/Swapchain.h"
#include "renderer/Pipeline.h"
#include "renderer/Renderer.h"
#include "renderer/Descriptors.h"
#include "renderer/ResourceCache.h"
#include "ui/UIPipeline.h"
#include "ui/TitleBar.h"
#include "scene/Camera.h"
#include "core/Time.h"
#include "ecs/Registry.h"

#include <memory>

namespace Talos {

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void init();
    void run();

private:
    std::unique_ptr<Window> m_window;
    VulkanContext            m_vulkanContext;
    Swapchain                m_swapchain;
    Descriptors              m_descriptors;
    Pipeline                 m_pipeline;
    Renderer                 m_renderer;
    ResourceCache            m_resourceCache;
    UIPipeline               m_uiPipeline;
    TitleBar                 m_titleBar;
    Camera                   m_camera;
    Time                     m_time;
    Registry                 m_registry;

    // Orbiting child entity for demo
    Entity m_orbitEntity = NULL_ENTITY;
    float  m_orbitAngle  = 0.0f;

    // FPS tracking
    float m_fpsTimer  = 0.0f;
    int   m_fpsCount  = 0;

    // Light entities
    Entity m_sunEntity        = NULL_ENTITY;
    Entity m_pointLightEntity = NULL_ENTITY;
    Entity m_pointLightEntity2 = NULL_ENTITY;

    void handleResize();
    void fixedUpdate(float dt);
    void updateUniformBuffer(uint32_t currentFrame);
    void buildDemoScene();
    void loadGltfScene(const std::string& filepath);

    // Helper to create an entity with Transform + Mesh + Material
    Entity createMeshEntity(Mesh* mesh, Texture* texture,
                            const glm::vec3& position = {0,0,0},
                            const glm::vec3& scale = {1,1,1},
                            const glm::vec4& baseColorFactor = {1,1,1,1},
                            float metallic = 0.0f,
                            float roughness = 0.5f);
};

} // namespace Talos
