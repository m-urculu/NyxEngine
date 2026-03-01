#pragma once

// Window.h — GLFW window wrapper
//
// Creates and manages the application window. Handles:
// - Window creation with specified dimensions
// - Resize callbacks (needed for swapchain recreation)
// - Close events (the X button or Alt+F4)
// - Providing the raw GLFW window handle for Vulkan surface creation

#include <GLFW/glfw3.h>
#include <string>
#include <functional>

namespace Talos {

class Window {
public:
    // Create a window with the given title and size
    Window(const std::string& title, int width, int height);
    ~Window();

    // No copying — each window owns its GLFW handle
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Returns true when the user closes the window
    bool shouldClose() const;

    // Process pending window events (keyboard, mouse, resize, etc.)
    void pollEvents() const;

    // Raw GLFW handle — needed by Vulkan to create a surface
    GLFWwindow* getHandle() const { return m_window; }

    // Current window size in pixels
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    // True if the window was resized since last check (resets after reading)
    bool wasResized() const { return m_resized; }
    void resetResizedFlag() { m_resized = false; }

private:
    GLFWwindow* m_window = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_resized = false;

    // GLFW calls this when the window is resized
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
};

} // namespace Talos
