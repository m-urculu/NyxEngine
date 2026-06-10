#pragma once

// Input.h — GLFW input handling with key state and mouse deltas

#include <GLFW/glfw3.h>
#include <functional>

namespace Nyx {

class Window;
class TitleBar;
class ContentBrowser;
class Console;
class CodeEditor;
class SceneHierarchy;
class Inspector;

class Input {
public:
    static void init(GLFWwindow* window, Window* appWindow = nullptr);

    static bool isKeyDown(int key);

    static float getMouseDeltaX();
    static float getMouseDeltaY();

    // Zero the accumulated delta after it's been consumed. The fixed-timestep loop
    // can tick 0..N times per frame; without this, a per-frame mouse delta would be
    // applied multiple times (doubled look) or, on a 0-tick frame, not at all.
    static void consumeMouseDelta() { s_deltaX = 0.0f; s_deltaY = 0.0f; }

    // Call once per frame to update deltas
    static void update();

    // The viewport "captures" the cursor while orbiting (middle-mouse drag), so
    // panels stop reacting to hover during a camera move.
    static bool isCursorCaptured() {
        return s_window && glfwGetMouseButton(s_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    }

    // True while the camera should orbit: middle-mouse held and not typing.
    static bool isOrbiting();

    static void setTitleBar(TitleBar* titleBar) { s_titleBar = titleBar; }
    static void setContentBrowser(ContentBrowser* cb) { s_contentBrowser = cb; }
    static void setConsole(Console* c) { s_console = c; }
    static void setEditor(CodeEditor* e) { s_editor = e; }
    static void setSceneHierarchy(SceneHierarchy* h) { s_hierarchy = h; }
    static void setInspector(Inspector* i) { s_inspector = i; }

    // Ctrl+S saves the scene when the text editor isn't focused (its own Ctrl+S
    // saves the open file). Bound by the Engine to saveCurrentScene().
    static void setSaveSceneCallback(std::function<void()> cb) { s_onSaveScene = std::move(cb); }

    // Scene undo / redo (Ctrl+Z / Ctrl+Shift+Z), routed when neither the text editor
    // nor the content browser is focused (those own their own Ctrl+Z).
    static void setUndoCallback(std::function<void()> cb) { s_onUndo = std::move(cb); }
    static void setRedoCallback(std::function<void()> cb) { s_onRedo = std::move(cb); }

    // Ctrl+G — group the currently-selected entities under a new empty parent.
    // Routed when neither the editor nor the browser has focus.
    static void setGroupSelectedCallback(std::function<void()> cb) { s_onGroupSelected = std::move(cb); }

    // Left-click that fell through every panel to the 3D viewport (x,y in pixels):
    // the Engine uses it to grab a gizmo axis or ray-pick an object.
    static void setViewportPressCallback(std::function<void(double, double)> cb) { s_onViewportPress = std::move(cb); }

    // Scroll wheel that fell through every panel to the 3D viewport → camera zoom.
    static void setViewportZoomCallback(std::function<void(double)> cb) { s_onViewportZoom = std::move(cb); }

    // Right-CLICK (press + release without a drag) that fell through to the 3D
    // viewport → Engine opens the scene context menu. A right-DRAG is still
    // camera look and never fires this.
    static void setViewportRightClickCallback(std::function<void(double, double)> cb) { s_onViewportRightClick = std::move(cb); }

    // Left-press that lands on the right-dock's resize edge → Engine starts a
    // drag. Routed BEFORE the hierarchy/inspector dispatch so the click doesn't
    // also pick an entity or scrub a field. Returns true if consumed.
    static void setRightDockResizeCallback(std::function<bool()> cb) { s_onRightDockResize = std::move(cb); }
    // Same idea for the horizontal split between Hierarchy and Inspector.
    static void setHierSplitResizeCallback(std::function<bool()> cb) { s_onHierSplitResize = std::move(cb); }
    // Ctrl+B toggles the right sidebar (collapse / expand).
    static void setToggleRightDockCallback(std::function<void()> cb) { s_onToggleRightDock = std::move(cb); }

    // True when the code editor has keyboard focus (suppresses camera WASD).
    static bool isTextInputActive();

    // True when camera WASD movement should be ignored: text input OR a sub-window
    // (hierarchy / content browser) holds focus. Right-drag look is unaffected.
    static bool cameraMovementSuppressed();

private:
    static GLFWwindow* s_window;
    static double s_lastMouseX;
    static double s_lastMouseY;
    static float s_deltaX;
    static float s_deltaY;
    static bool s_firstMouse;
    static bool s_lookSuppressed;   // right-press landed on the sidebar (context menu) → don't look
    static Window* s_appWindow;
    static TitleBar* s_titleBar;
    static ContentBrowser* s_contentBrowser;
    static Console* s_console;
    static CodeEditor* s_editor;
    static SceneHierarchy* s_hierarchy;
    static Inspector* s_inspector;
    static std::function<void()> s_onSaveScene;
    static std::function<void()> s_onUndo;
    static std::function<void()> s_onRedo;
    static std::function<void()> s_onGroupSelected;
    static std::function<void(double, double)> s_onViewportPress;
    static std::function<void(double)> s_onViewportZoom;
    static std::function<void(double, double)> s_onViewportRightClick;
    // Right-press bookkeeping so a release without movement = a click (menu),
    // while a release after movement = a camera-look drag (no menu).
    static double s_rightPressX, s_rightPressY;
    static bool   s_rightPressPending;
    static std::function<bool()> s_onRightDockResize;
    static std::function<bool()> s_onHierSplitResize;
    static std::function<void()> s_onToggleRightDock;

    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    static void charCallback(GLFWwindow* window, unsigned int codepoint);
};

} // namespace Nyx
