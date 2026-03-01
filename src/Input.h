#pragma once

// Input.h — GLFW input handling with key state and mouse deltas

#include <GLFW/glfw3.h>

namespace Talos {

class Input {
public:
    static void init(GLFWwindow* window);

    static bool isKeyDown(int key);

    static float getMouseDeltaX();
    static float getMouseDeltaY();

    // Call once per frame to update deltas
    static void update();

private:
    static GLFWwindow* s_window;
    static double s_lastMouseX;
    static double s_lastMouseY;
    static float s_deltaX;
    static float s_deltaY;
    static bool s_firstMouse;
    static bool s_cursorCaptured;

    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
};

} // namespace Talos
