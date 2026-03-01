#pragma once

// Engine.h — Main engine class
//
// This is the top-level class that owns all subsystems and runs the game loop.
// The lifecycle is:
//   1. init()  — Create window, Vulkan context, swapchain, pipeline, renderer
//   2. run()   — Enter the main loop (poll events → draw frame → repeat)
//   3. ~Engine — Cleanup everything in reverse order

#include "Window.h"
#include "renderer/VulkanContext.h"
#include "renderer/Swapchain.h"
#include "renderer/Pipeline.h"
#include "renderer/Renderer.h"

#include <memory>

namespace VulkanEngine {

class Engine {
public:
    Engine();
    ~Engine();

    // No copying
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Initialize all subsystems
    void init();

    // Run the main game loop (blocks until window is closed)
    void run();

private:
    std::unique_ptr<Window> m_window;
    VulkanContext            m_vulkanContext;
    Swapchain                m_swapchain;
    Pipeline                 m_pipeline;
    Renderer                 m_renderer;

    // Handle window resize by recreating swapchain, pipeline, and framebuffers
    void handleResize();
};

} // namespace VulkanEngine
