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

namespace Nyx {

// Custom borderless maximize. Bypasses the OS WS_MAXIMIZE state (which
// inflates the maximize rect by the frame thickness on every side, clipping
// our UI off-screen) by SetWindowPos-ing the window to the monitor's work area
// and tracking maximize state internally. Drives the title-bar max button and
// the Window::isMaximized / maximize methods.
void toggleCustomMaximize(GLFWwindow* w);
bool isCustomMaximized(GLFWwindow* w);

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

    // Current window position in screen coords (top-left).
    void getPosition(int& x, int& y) const;
    // Move the window to a specific screen position.
    void setPosition(int x, int y);
    // Maximize state — checked at save time so we can restore via the OS
    // rather than re-applying the overhang-offset coordinates Windows uses for
    // maximized windows (which look right while maximized but place the window
    // off-screen by ~8 px on top-left when restored without re-maximizing).
    bool isMaximized() const;
    void maximize();

    // True if the window was resized since last check (resets after reading)
    bool wasResized() const { return m_resized; }
    void resetResizedFlag() { m_resized = false; }

    // Toggle borderless fullscreen
    void toggleFullscreen();
    bool isFullscreen() const { return m_fullscreen; }

    // Reveal the window. The window is created hidden so the splash screen can
    // own the screen until the engine finishes initialising; call show() once
    // the first frame is ready to render.
    void show();

    // Native "pick a folder" dialog (Windows). Returns the selected path (forward
    // slashes) or "" if cancelled / unsupported.
    std::string openFolderDialog(const std::string& title = "Open Folder",
                                 const std::string& initialDir = "");


private:
    GLFWwindow* m_window = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_resized = false;
    bool m_fullscreen = false;

    // Saved windowed position/size for restoring from fullscreen
    int m_windowedX = 0, m_windowedY = 0;
    int m_windowedWidth = 0, m_windowedHeight = 0;

    // GLFW calls this when the window is resized
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
};

} // namespace Nyx
