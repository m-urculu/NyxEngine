#include "Engine.h"
#include "Logger.h"

namespace VulkanEngine {

Engine::Engine() = default;

Engine::~Engine() {
    // Wait for the GPU to finish all work before destroying anything
    m_renderer.waitIdle(m_vulkanContext.getDevice());

    // Cleanup in reverse order of creation
    m_renderer.cleanup(m_vulkanContext.getDevice());
    m_pipeline.cleanup(m_vulkanContext.getDevice());
    m_swapchain.cleanup(m_vulkanContext.getDevice());
    m_vulkanContext.cleanup();
    // Window is destroyed automatically by unique_ptr

    LOG_INFO("Engine shut down");
}

void Engine::init() {
    // 1. Initialize logging first (so everything else can log)
    Logger::init();
    LOG_INFO("=== VulkanEngine v0.1.0 ===");

    // 2. Create the window
    m_window = std::make_unique<Window>("VulkanEngine", 1280, 720);

    // 3. Initialize Vulkan (instance, device, queues)
    m_vulkanContext.init(m_window->getHandle());

    // 4. Create the swapchain (frame buffering)
    m_swapchain.init(m_vulkanContext, m_window->getWidth(), m_window->getHeight());

    // 5. Create the graphics pipeline (shaders, render pass)
    m_pipeline.init(m_vulkanContext.getDevice(), m_swapchain.getExtent(), m_swapchain.getImageFormat());

    // 6. Create the renderer (command buffers, sync objects)
    m_renderer.init(m_vulkanContext, m_swapchain, m_pipeline);

    LOG_INFO("Engine initialized successfully!");
}

void Engine::run() {
    LOG_INFO("Entering main loop");

    while (!m_window->shouldClose()) {
        // Process window events (keyboard, mouse, close button)
        m_window->pollEvents();

        // Check if window was resized
        if (m_window->wasResized()) {
            m_window->resetResizedFlag();
            handleResize();
            continue;  // Skip this frame
        }

        // Draw a frame
        bool ok = m_renderer.drawFrame(m_vulkanContext, m_swapchain, m_pipeline);
        if (!ok) {
            // Swapchain is out of date — recreate it
            handleResize();
        }
    }

    LOG_INFO("Main loop ended");
}

void Engine::handleResize() {
    // Don't resize if the window is minimized (size = 0)
    int width = m_window->getWidth();
    int height = m_window->getHeight();
    if (width == 0 || height == 0) return;

    LOG_INFO("Handling resize to {}x{}", width, height);

    // Wait for GPU to finish current work
    m_renderer.waitIdle(m_vulkanContext.getDevice());

    // Recreate everything that depends on the window size
    m_swapchain.recreate(m_vulkanContext, width, height);
    m_pipeline.recreate(m_vulkanContext.getDevice(), m_swapchain.getExtent(), m_swapchain.getImageFormat());
    m_renderer.recreateFramebuffers(m_vulkanContext.getDevice(), m_swapchain, m_pipeline);
}

} // namespace VulkanEngine
