#include "Input.h"
#include "Window.h"
#include "ui/TitleBar.h"
#include "ui/ContentBrowser.h"
#include "ui/Console.h"
#include "ui/CodeEditor.h"
#include "ui/SceneHierarchy.h"
#include "ui/Inspector.h"

namespace Nyx {

GLFWwindow*     Input::s_window         = nullptr;
double          Input::s_lastMouseX     = 0.0;
double          Input::s_lastMouseY     = 0.0;
float           Input::s_deltaX         = 0.0f;
float           Input::s_deltaY         = 0.0f;
bool            Input::s_firstMouse     = true;
bool            Input::s_lookSuppressed = false;
Window*         Input::s_appWindow      = nullptr;
TitleBar*       Input::s_titleBar       = nullptr;
ContentBrowser* Input::s_contentBrowser = nullptr;
Console*        Input::s_console        = nullptr;
CodeEditor*     Input::s_editor         = nullptr;
SceneHierarchy* Input::s_hierarchy      = nullptr;
Inspector*      Input::s_inspector      = nullptr;
std::function<void()> Input::s_onSaveScene;
std::function<void()> Input::s_onUndo;
std::function<void()> Input::s_onRedo;
std::function<void(double, double)> Input::s_onViewportPress;
std::function<void(double)> Input::s_onViewportZoom;
std::function<bool()> Input::s_onRightDockResize;
std::function<bool()> Input::s_onHierSplitResize;
std::function<void()> Input::s_onToggleRightDock;

bool Input::isOrbiting() {
    return s_window
        && glfwGetMouseButton(s_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS
        && !isTextInputActive();
}

bool Input::isTextInputActive() {
    return (s_editor && s_editor->isFocused())
        || (s_contentBrowser && s_contentBrowser->capturesKeyboard())
        || (s_console && s_console->capturesKeyboard());
}

bool Input::cameraMovementSuppressed() {
    return isTextInputActive()
        || (s_hierarchy && s_hierarchy->isFocused())
        || (s_contentBrowser && s_contentBrowser->isFocused());
}

void Input::init(GLFWwindow* window, Window* appWindow) {
    s_window = window;
    s_appWindow = appWindow;
    s_firstMouse = true;

    glfwSetKeyCallback(window, keyCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetCharCallback(window, charCallback);
}

bool Input::isKeyDown(int key) {
    return glfwGetKey(s_window, key) == GLFW_PRESS;
}

float Input::getMouseDeltaX() { return s_deltaX; }
float Input::getMouseDeltaY() { return s_deltaY; }

void Input::update() {
    double mouseX, mouseY;
    glfwGetCursorPos(s_window, &mouseX, &mouseY);

    // Camera orbit: middle-mouse hold drives the rotation deltas (consumed by the
    // Engine, which orbits around the current selection).
    bool orbiting = isOrbiting();

    if (!orbiting) {
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
                         int action, int mods) {
    // Inspector text-edit captures Enter / Esc / Backspace before Ctrl+S /
    // Ctrl+Z / scene shortcuts can hijack them.
    if (s_inspector && s_inspector->capturesKeyboard()) {
        if (s_inspector->handleKey(key, action, mods)) return;
    }
    // Content browser gets keys first when it is renaming (captures everything) or
    // focused (Ctrl+C/X/V/Z on the selected file). It returns true if it consumed.
    if (s_contentBrowser && (s_contentBrowser->capturesKeyboard() || s_contentBrowser->isFocused())) {
        if (s_contentBrowser->handleKey(key, action, mods)) return;
    }
    // Scene hierarchy command keys when focused (Del / Ctrl+C/X/V/A on the selection).
    if (s_hierarchy && s_hierarchy->isFocused()) {
        if (s_hierarchy->handleKey(key, action, mods)) return;
    }
    // Dev console command input.
    if (s_console && s_console->capturesKeyboard()) { s_console->handleKey(key, action, mods); return; }
    if (key == GLFW_KEY_F11 && action == GLFW_PRESS && s_appWindow) {
        s_appWindow->toggleFullscreen();
        return;
    }
    // Ctrl+S saves the scene — except while editing text (the editor's own Ctrl+S,
    // handled above when focused, saves the open file instead).
    if (key == GLFW_KEY_S && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL)
        && !(s_editor && s_editor->isFocused())) {
        if (s_onSaveScene) s_onSaveScene();
        return;
    }
    // Scene undo / redo — Ctrl+Z / Ctrl+Shift+Z (the editor and content browser own
    // their own Ctrl+Z while focused, handled above/below this).
    if (key == GLFW_KEY_Z && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL)
        && !(s_editor && s_editor->isFocused())
        && !(s_contentBrowser && s_contentBrowser->isFocused())) {
        if (mods & GLFW_MOD_SHIFT) { if (s_onRedo) s_onRedo(); }
        else                       { if (s_onUndo) s_onUndo(); }
        return;
    }
    // Ctrl+B toggles the right sidebar (Hierarchy + Inspector).
    if (key == GLFW_KEY_B && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL)
        && !(s_editor && s_editor->isFocused())
        && !(s_contentBrowser && s_contentBrowser->isFocused())) {
        if (s_onToggleRightDock) s_onToggleRightDock();
        return;
    }
    // Esc releases editor focus (back to camera control).
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && s_editor && s_editor->isFocused()) {
        s_editor->setFocused(false);
        return;
    }
    if (s_editor) s_editor->handleKey(key, action, mods);   // editor checks focus internally
}

void Input::charCallback([[maybe_unused]] GLFWwindow* window, unsigned int codepoint) {
    if (s_inspector && s_inspector->capturesKeyboard()) {
        s_inspector->handleChar(codepoint);                 // numeric field entry
        return;
    }
    if (s_contentBrowser && s_contentBrowser->capturesKeyboard()) {
        s_contentBrowser->handleChar(codepoint);            // rename text entry
        return;
    }
    if (s_console && s_console->capturesKeyboard()) { s_console->handleChar(codepoint); return; }
    if (s_editor) s_editor->handleChar(codepoint);          // editor checks focus internally
}

void Input::mouseButtonCallback([[maybe_unused]] GLFWwindow* window, int button,
                                 int action, [[maybe_unused]] int mods) {
    // Forward releases to all panels (drag/resize/drop end).
    if (action == GLFW_RELEASE) {
        if (button == GLFW_MOUSE_BUTTON_RIGHT) s_lookSuppressed = false;
        if (s_titleBar)       s_titleBar->handleMouseButton(button, action);
        if (s_contentBrowser) s_contentBrowser->handleRelease();
        if (s_console)        s_console->handleRelease();
        if (s_hierarchy)      s_hierarchy->handleRelease();
        if (s_inspector)      s_inspector->handleRelease();
        return;
    }

    if (action != GLFW_PRESS) return;

    // Any press commits a pending inspector text-edit. Without this, clicks on
    // other panels would leave the typed value unsaved when the focus shifts.
    if (s_inspector) s_inspector->commitTextEditOnExternalClick();

    // Right-press → content browser context menu. If it lands on the panel, the
    // menu opens, the tree takes keyboard focus, and camera look is suppressed.
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (s_contentBrowser && s_contentBrowser->handleRightPress()) {
            s_lookSuppressed = true;
            s_contentBrowser->setFocused(true);
            if (s_hierarchy) s_hierarchy->setFocused(false);
            if (s_editor)    s_editor->setFocused(false);
        } else if (s_hierarchy && s_hierarchy->handleRightPress()) {
            s_lookSuppressed = true;
            s_hierarchy->setFocused(true);
            if (s_contentBrowser) s_contentBrowser->setFocused(false);
            if (s_editor)         s_editor->setFocused(false);
            if (s_console)        s_console->setFocused(false);
        }
        return;
    }

    if (button != GLFW_MOUSE_BUTTON_LEFT) return;

    // Exactly one sub-window holds keyboard/command focus. Whichever panel consumes
    // the click takes focus and clears the OTHERS (console/editor self-focus inside
    // their own handlers, so we must not clear them after); a viewport click clears
    // all so camera controls resume (see cameraMovementSuppressed()).
    if (s_titleBar && s_titleBar->handleMouseButton(button, action)) return;
    // Right-dock resize edge — must run BEFORE hierarchy/inspector so a click on
    // the edge starts a drag instead of picking an entity or scrubbing a field.
    if (s_onRightDockResize && s_onRightDockResize()) return;
    if (s_onHierSplitResize && s_onHierSplitResize()) return;
    if (s_contentBrowser && s_contentBrowser->handleMouseButton(button, action)) {
        s_contentBrowser->setFocused(true);
        if (s_hierarchy) s_hierarchy->setFocused(false);
        if (s_editor)    s_editor->setFocused(false);
        if (s_console)   s_console->setFocused(false);
        return;
    }
    if (s_hierarchy && s_hierarchy->handleMouseButton(button, action)) {
        s_hierarchy->setFocused(true);
        if (s_contentBrowser) s_contentBrowser->setFocused(false);
        if (s_editor)         s_editor->setFocused(false);
        if (s_console)        s_console->setFocused(false);
        return;
    }
    if (s_inspector && s_inspector->handleMouseButton(button, action)) {
        if (s_contentBrowser) s_contentBrowser->setFocused(false);
        if (s_hierarchy)      s_hierarchy->setFocused(false);
        if (s_editor)         s_editor->setFocused(false);
        if (s_console)        s_console->setFocused(false);
        return;
    }
    if (s_console && s_console->handleMouseButton(button, action)) {
        if (s_contentBrowser) s_contentBrowser->setFocused(false);
        if (s_hierarchy)      s_hierarchy->setFocused(false);
        if (s_editor)         s_editor->setFocused(false);
        return;
    }
    if (s_editor && s_editor->handleMouseButton(button, action, mods)) {
        if (s_contentBrowser) s_contentBrowser->setFocused(false);
        if (s_hierarchy)      s_hierarchy->setFocused(false);
        if (s_console)        s_console->setFocused(false);
        return;
    }
    // Click fell through to the 3D viewport → release all keyboard/command focus,
    // then let the Engine grab a gizmo axis or ray-pick an object.
    if (s_editor)         s_editor->setFocused(false);
    if (s_contentBrowser) s_contentBrowser->setFocused(false);
    if (s_hierarchy)      s_hierarchy->setFocused(false);
    if (s_console)        s_console->setFocused(false);
    if (s_onViewportPress) {
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        s_onViewportPress(mx, my);
    }
}

void Input::scrollCallback([[maybe_unused]] GLFWwindow* window,
                           [[maybe_unused]] double xoffset, double yoffset) {
    // Route the wheel to whichever panel the cursor is over.
    if (s_contentBrowser && s_contentBrowser->handleScroll(yoffset)) return;
    if (s_hierarchy && s_hierarchy->handleScroll(yoffset)) return;
    if (s_console && s_console->handleScroll(yoffset)) return;
    if (s_editor && s_editor->handleScroll(yoffset)) return;
    // Fell through to the 3D viewport → zoom the camera.
    if (s_onViewportZoom) s_onViewportZoom(yoffset);
}

} // namespace Nyx
