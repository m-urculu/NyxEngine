#include "Window.h"
#include "Logger.h"
#include <stdexcept>

namespace Talos {

Window::Window(const std::string& title, int width, int height)
    : m_width(width), m_height(height)
{
    // Initialize the GLFW library (safe to call multiple times)
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    // Tell GLFW we're using Vulkan, not OpenGL
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Allow the window to be resized (we'll handle swapchain recreation)
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

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
