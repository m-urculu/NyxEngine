#pragma once

// Input.h — GLFW input handling with key state and mouse deltas

#include <GLFW/glfw3.h>

namespace Talos {

class Window;
class TitleBar;

class Input {
public:
    static void init(GLFWwindow* window, Window* appWindow = nullptr);

    static bool isKeyDown(int key);

    static float getMouseDeltaX();
    static float getMouseDeltaY();

    // Call once per frame to update deltas
    static void update();

    static bool isCursorCaptured() {
        return s_window && glfwGetMouseButton(s_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    }

    static void setTitleBar(TitleBar* titleBar) { s_titleBar = titleBar; }

private:
    static GLFWwindow* s_window;
    static double s_lastMouseX;
    static double s_lastMouseY;
    static float s_deltaX;
    static float s_deltaY;
    static bool s_firstMouse;
    static Window* s_appWindow;
    static TitleBar* s_titleBar;

    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
};

} // namespace Talos
