#include "Engine.h"
#include "Logger.h"
#include "Input.h"
#include "renderer/Vertex.h"
#include "renderer/UniformTypes.h"
#include "renderer/ObjLoader.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace VulkanEngine {

Engine::Engine() = default;

Engine::~Engine() {
    m_renderer.waitIdle(m_vulkanContext.getDevice());

    m_mesh.cleanup(m_vulkanContext.getAllocator());
    m_renderer.cleanup(m_vulkanContext.getDevice());
    m_pipeline.cleanup(m_vulkanContext.getDevice());
    m_descriptors.cleanup(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());
    m_swapchain.cleanup(m_vulkanContext.getDevice(), m_vulkanContext.getAllocator());
    m_vulkanContext.cleanup();

    LOG_INFO("Engine shut down");
}

void Engine::init() {
    Logger::init();
    LOG_INFO("=== VulkanEngine v0.1.0 ===");

    m_window = std::make_unique<Window>("VulkanEngine", 1280, 720);

    m_vulkanContext.init(m_window->getHandle());

    m_swapchain.init(m_vulkanContext, m_window->getWidth(), m_window->getHeight());

    m_descriptors.init(m_vulkanContext);

    m_pipeline.init(m_vulkanContext, m_swapchain.getExtent(), m_swapchain.getImageFormat(),
                     m_swapchain.getDepthBuffer().getFormat(), m_descriptors.getLayout());

    m_renderer.init(m_vulkanContext, m_swapchain, m_pipeline);

    createCubeMesh();

    Input::init(m_window->getHandle());

    float aspect = static_cast<float>(m_window->getWidth()) / static_cast<float>(m_window->getHeight());
    m_camera.init({0.0f, 0.0f, 3.0f}, aspect);

    m_lastFrameTime = static_cast<float>(glfwGetTime());

    LOG_INFO("Engine initialized successfully!");
}

void Engine::run() {
    LOG_INFO("Entering main loop");

    while (!m_window->shouldClose()) {
        m_window->pollEvents();

        // Delta time
        float currentTime = static_cast<float>(glfwGetTime());
        float deltaTime = currentTime - m_lastFrameTime;
        m_lastFrameTime = currentTime;

        // Update input and camera
        Input::update();
        m_camera.update(deltaTime);

        if (m_window->wasResized()) {
            m_window->resetResizedFlag();
            handleResize();
            continue;
        }

        // Update UBO with current camera matrices
        updateUniformBuffer(m_renderer.getCurrentFrame());

        bool ok = m_renderer.drawFrame(m_vulkanContext, m_swapchain, m_pipeline,
                                        m_mesh, m_descriptors);
        if (!ok) {
            handleResize();
        }
    }

    LOG_INFO("Main loop ended");
}

void Engine::handleResize() {
    int width = m_window->getWidth();
    int height = m_window->getHeight();
    if (width == 0 || height == 0) return;

    LOG_INFO("Handling resize to {}x{}", width, height);

    m_renderer.waitIdle(m_vulkanContext.getDevice());

    m_swapchain.recreate(m_vulkanContext, width, height);
    m_pipeline.recreate(m_vulkanContext, m_swapchain.getExtent(), m_swapchain.getImageFormat(),
                         m_swapchain.getDepthBuffer().getFormat(), m_descriptors.getLayout());
    m_renderer.recreateFramebuffers(m_vulkanContext.getDevice(), m_swapchain, m_pipeline);

    m_camera.setAspectRatio(static_cast<float>(width) / static_cast<float>(height));
}

void Engine::updateUniformBuffer(uint32_t currentFrame) {
    UniformBufferObject ubo{};
    ubo.model      = glm::mat4(1.0f);
    ubo.view       = m_camera.getViewMatrix();
    ubo.projection = m_camera.getProjectionMatrix();
    ubo.normalMatrix = glm::transpose(glm::inverse(ubo.model));

    // Lighting
    ubo.lightDirection = glm::vec4(glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f)), 0.0f);
    ubo.lightColor     = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    ubo.ambientColor   = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);
    ubo.cameraPosition = glm::vec4(m_camera.getPosition(), 1.0f);

    m_descriptors.getUniformBuffer(currentFrame).uploadData(
        m_vulkanContext.getAllocator(), &ubo, sizeof(ubo));
}

void Engine::createCubeMesh() {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    if (!ObjLoader::load("assets/models/cube.obj", vertices, indices, {0.7f, 0.7f, 0.7f})) {
        throw std::runtime_error("Failed to load cube.obj");
    }

    m_mesh.init(m_vulkanContext, vertices, indices);
}

} // namespace VulkanEngine
