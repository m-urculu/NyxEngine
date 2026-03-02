#include "Input.h"
#include "Window.h"
#include "ui/TitleBar.h"

namespace Talos {

GLFWwindow* Input::s_window      = nullptr;
double      Input::s_lastMouseX  = 0.0;
double      Input::s_lastMouseY  = 0.0;
float       Input::s_deltaX      = 0.0f;
float       Input::s_deltaY      = 0.0f;
bool        Input::s_firstMouse  = true;
Window*     Input::s_appWindow   = nullptr;
TitleBar*   Input::s_titleBar    = nullptr;

void Input::init(GLFWwindow* window, Window* appWindow) {
    s_window = window;
    s_appWindow = appWindow;
    s_firstMouse = true;

    glfwSetKeyCallback(window, keyCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
}

bool Input::isKeyDown(int key) {
    return glfwGetKey(s_window, key) == GLFW_PRESS;
}

float Input::getMouseDeltaX() { return s_deltaX; }
float Input::getMouseDeltaY() { return s_deltaY; }

void Input::update() {
    double mouseX, mouseY;
    glfwGetCursorPos(s_window, &mouseX, &mouseY);

    // Camera look: right-click hold
    bool rightHeld = glfwGetMouseButton(s_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    if (!rightHeld) {
        s_deltaX = 0.0f;
        s_deltaY = 0.0f;
        s_firstMouse = true;
        s_lastMouseX = mouseX;
        s_lastMouseY = mouseY;
        return;
    }

    if (s_firstMouse) {
        s_lastMouseX = mouseX;
        s_lastMouseY = mouseY;
        s_firstMouse = false;
    }

    s_deltaX = static_cast<float>(mouseX - s_lastMouseX);
    s_deltaY = static_cast<float>(mouseY - s_lastMouseY);

    s_lastMouseX = mouseX;
    s_lastMouseY = mouseY;
}

void Input::keyCallback([[maybe_unused]] GLFWwindow* window, int key,
                         [[maybe_unused]] int scancode,
                         int action, [[maybe_unused]] int mods) {
    if (key == GLFW_KEY_F11 && action == GLFW_PRESS && s_appWindow) {
        s_appWindow->toggleFullscreen();
    }
}

void Input::mouseButtonCallback([[maybe_unused]] GLFWwindow* window, int button,
                                 int action, [[maybe_unused]] int mods) {
    // Forward all releases to title bar (for drag/resize end)
    if (action == GLFW_RELEASE) {
        if (s_titleBar) s_titleBar->handleMouseButton(button, action);
        return;
    }

    // Only handle left press
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;

    // Let title bar handle the click
    if (s_titleBar && s_titleBar->handleMouseButton(button, action)) {
        return;
    }
}

} // namespace Talos
