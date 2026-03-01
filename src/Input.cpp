#include "Input.h"
#include "Logger.h"

namespace Talos {

GLFWwindow* Input::s_window       = nullptr;
double      Input::s_lastMouseX   = 0.0;
double      Input::s_lastMouseY   = 0.0;
float       Input::s_deltaX       = 0.0f;
float       Input::s_deltaY       = 0.0f;
bool        Input::s_firstMouse   = true;
bool        Input::s_cursorCaptured = true;

void Input::init(GLFWwindow* window) {
    s_window = window;
    s_firstMouse = true;
    s_cursorCaptured = true;

    // Capture cursor for FPS controls
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // Raw mouse motion if available
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }

    glfwSetKeyCallback(window, keyCallback);

    LOG_INFO("Input system initialized");
}

bool Input::isKeyDown(int key) {
    return glfwGetKey(s_window, key) == GLFW_PRESS;
}

float Input::getMouseDeltaX() { return s_deltaX; }
float Input::getMouseDeltaY() { return s_deltaY; }

void Input::update() {
    if (!s_cursorCaptured) {
        s_deltaX = 0.0f;
        s_deltaY = 0.0f;
        return;
    }

    double mouseX, mouseY;
    glfwGetCursorPos(s_window, &mouseX, &mouseY);

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

void Input::keyCallback(GLFWwindow* window, int key, [[maybe_unused]] int scancode,
                         int action, [[maybe_unused]] int mods) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        s_cursorCaptured = !s_cursorCaptured;

        if (s_cursorCaptured) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            s_firstMouse = true;
            LOG_INFO("Cursor captured");
        } else {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            LOG_INFO("Cursor released");
        }
    }
}

} // namespace Talos
