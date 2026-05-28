#include "Window.h"
#include "Logger.h"
#include <stdexcept>

#ifdef _WIN32
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3native.h>
#  include <windows.h>
#  include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#  include <shobjidl.h>   // IFileOpenDialog (native folder picker)
#  include <filesystem>
#endif

namespace Nyx {

#ifdef _WIN32
// ── Native window chrome ─────────────────────────────────────────────────────
// The window is visually borderless (we draw our own title bar), but we keep the
// resizable frame styles and tell Windows, via WM_NCHITTEST, that the title-bar
// strip is the caption and the edges are resize borders. That hands move, resize,
// double-click-maximize, Win+arrow, and Aero Snap (drag to top/side, per-monitor)
// back to the OS — exactly like a normal Windows app.
namespace {
constexpr int kBarHeight       = 32;       // matches TitleBar::BAR_HEIGHT
constexpr int kCaptionButtonsW = 46 * 3;   // min / max / close (TitleBar::BUTTON_WIDTH * 3)
WNDPROC g_prevProc = nullptr;

LRESULT nyxHitTest(HWND hwnd, LPARAM lParam) {
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    RECT rc; GetWindowRect(hwnd, &rc);
    int x = pt.x - rc.left, y = pt.y - rc.top;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const int B = 6;  // resize-border thickness

    if (!IsZoomed(hwnd)) {                       // edges only when not maximized
        bool l = x < B, r = x >= w - B, t = y < B, b = y >= h - B;
        if (t && l) return HTTOPLEFT;    if (t && r) return HTTOPRIGHT;
        if (b && l) return HTBOTTOMLEFT; if (b && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;  if (r) return HTRIGHT;
        if (t) return HTTOP;   if (b) return HTBOTTOM;
    }
    // Title-bar strip is the caption (draggable → enables Aero Snap), except the
    // min/max/close buttons on the right, which must stay clickable (HTCLIENT).
    if (y < kBarHeight && x < w - kCaptionButtonsW) return HTCAPTION;
    return HTCLIENT;
}

LRESULT CALLBACK nyxWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp == TRUE) {
                // Client fills the whole window (no native frame). When maximized
                // the window rect overhangs the monitor by the frame thickness, so
                // inset the client back to the work area (keeps the taskbar visible
                // and content un-clipped).
                if (IsZoomed(hwnd)) {
                    auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                    int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    p->rgrc[0].left   += fx;
                    p->rgrc[0].right  -= fx;
                    p->rgrc[0].top    += fy;
                    p->rgrc[0].bottom -= fy;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST:
            return nyxHitTest(hwnd, lp);
    }
    return CallWindowProc(g_prevProc, hwnd, msg, wp, lp);
}

void installNativeChrome(GLFWwindow* win) {
    HWND hwnd = glfwGetWin32Window(win);
    if (!hwnd) return;
    // Swap GLFW's borderless WS_POPUP for an overlapped (snappable) window.
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style = (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    // Subclass the window proc (chain to GLFW's for everything we don't handle).
    g_prevProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(hwnd, GWLP_WNDPROC));
    SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(nyxWndProc));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
} // namespace
#endif // _WIN32

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

    // Borderless window — we draw our own title bar (custom chrome). On native
    // Win32 the OS still handles focus/activation correctly.
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    // Create hidden — the splash screen is the user-visible artifact during
    // engine startup; the main window is revealed via Window::show() only once
    // init+scene-load is done. Saves a few seconds of staring at a blank editor.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

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

#ifdef _WIN32
    // Give the borderless window real OS chrome behavior (Aero Snap, etc.).
    installNativeChrome(m_window);
#endif

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

void Window::show() {
    if (m_window) glfwShowWindow(m_window);
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

std::string Window::openFolderDialog(const std::string& title) {
#ifdef _WIN32
    std::string result;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        std::wstring wtitle(title.begin(), title.end());
        dlg->SetTitle(wtitle.c_str());

        HWND owner = glfwGetWin32Window(m_window);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR wpath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath)) && wpath) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 1) {
                        std::string s(static_cast<size_t>(len - 1), '\0');
                        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, s.data(), len, nullptr, nullptr);
                        result = std::filesystem::path(s).generic_string();   // forward slashes
                    }
                    CoTaskMemFree(wpath);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return result;
#else
    (void)title;
    return {};
#endif
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

} // namespace Nyx
