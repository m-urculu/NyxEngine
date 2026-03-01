#pragma once

// Engine.h — Main engine class

#include "Window.h"
#include "renderer/VulkanContext.h"
#include "renderer/Swapchain.h"
#include "renderer/Pipeline.h"
#include "renderer/Renderer.h"
#include "renderer/Mesh.h"
#include "renderer/Descriptors.h"
#include "scene/Camera.h"

#include <memory>

namespace VulkanEngine {

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
    Mesh                     m_mesh;
    Camera                   m_camera;

    float m_lastFrameTime = 0.0f;

    void handleResize();
    void createCubeMesh();
    void updateUniformBuffer(uint32_t currentFrame);
};

} // namespace VulkanEngine
