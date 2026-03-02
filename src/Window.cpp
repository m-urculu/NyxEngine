#include "Window.h"
#include "Logger.h"
#include <stdexcept>

namespace Talos {

Window::Window(const std::string& title, int width, int height)
    : m_width(width), m_height(height)
{
    // Force X11 backend — Wayland doesn't support glfwSetWindowPos (needed for drag-to-move)
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);

    // Initialize the GLFW library (safe to call multiple times)
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    // Tell GLFW we're using Vulkan, not OpenGL
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Allow the window to be resized (we'll handle swapchain recreation)
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // Borderless window — no OS decorations/shadows
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    // Create the actual window
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    // Store a pointer to this Window object inside the GLFW window
    // so our static callback can access it
    glfwSetWindowUserPointer(m_window, this);

    // Register the resize callback
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);

    LOG_INFO("Window created: {}x{}", width, height);
}

Window::~Window() {
    if (m_window) {
        glfwDestroyWindow(m_window);
    }
    glfwTerminate();
    LOG_INFO("Window destroyed");
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_window);
}

void Window::pollEvents() const {
    glfwPollEvents();
}

void Window::toggleFullscreen() {
    if (!m_fullscreen) {
        // Save current windowed position and size
        glfwGetWindowPos(m_window, &m_windowedX, &m_windowedY);
        m_windowedWidth = m_width;
        m_windowedHeight = m_height;

        // Get primary monitor position and video mode
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        int monX, monY;
        glfwGetMonitorPos(monitor, &monX, &monY);

        // Borderless windowed fullscreen (more reliable than exclusive on XWayland)
        glfwSetWindowPos(m_window, monX, monY);
        glfwSetWindowSize(m_window, mode->width, mode->height);
        m_fullscreen = true;
        LOG_INFO("Entered fullscreen: {}x{} at ({},{})", mode->width, mode->height, monX, monY);
    } else {
        // Restore windowed mode
        glfwSetWindowPos(m_window, m_windowedX, m_windowedY);
        glfwSetWindowSize(m_window, m_windowedWidth, m_windowedHeight);
        m_fullscreen = false;
        LOG_INFO("Exited fullscreen: {}x{} at ({},{})",
                 m_windowedWidth, m_windowedHeight, m_windowedX, m_windowedY);
    }
}

// This is called by GLFW whenever the framebuffer (drawable area) is resized
void Window::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
    // Retrieve our Window object from the GLFW user pointer
    auto* app = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    app->m_resized = true;
    app->m_width = width;
    app->m_height = height;
    LOG_INFO("Window resized: {}x{}", width, height);
}

} // namespace Talos
