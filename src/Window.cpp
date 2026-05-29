#include "Window.h"
#include "Logger.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3native.h>
#  include <windows.h>
#  include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#  include <shobjidl.h>   // IFileOpenDialog (native folder picker)
#  include <dwmapi.h>     // DwmSetWindowAttribute for square corners (Win 11+)
#  include <filesystem>
// DWMWA_WINDOW_CORNER_PREFERENCE landed in the Win 10 22000 SDK — older SDKs
// don't define it. Both the attribute index (33) and the DWMWCP_DONOTROUND
// value (1) are stable, so falling back to the literals lets the same source
// build against older SDKs while still working on Win 11 at runtime.
#  ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#    define DWMWA_WINDOW_CORNER_PREFERENCE 33
#  endif
#  ifndef DWMWCP_DONOTROUND
#    define DWMWCP_DONOTROUND 1
#  endif
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

// Force a rectangular window shape — used as a stronger fallback when
// DwmSetWindowAttribute(DWMWCP_DONOTROUND) is silently ignored (some Win11
// builds round corners regardless of the corner preference attribute). The
// region IS the window shape, so DWM has no rounded region to clip against.
void applyRectRegion(HWND hwnd) {
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HRGN rgn = CreateRectRgn(0, 0, w, h);
    // SetWindowRgn takes ownership of rgn; don't DeleteObject it.
    SetWindowRgn(hwnd, rgn, TRUE);
}

// Set by Window::setLiveResizeCallback. Called from the WndProc on resize so
// the engine paints mid-drag. Plain function pointer/closure is fine — the
// WndProc only runs on the main thread (same thread that polls events).
static std::function<void()> s_liveResizeCb;

// Rubber-band resize state. WM_SIZING locks the window at its pre-drag size
// and tracks the user's proposed rect via a layered overlay window that
// floats above all others. WM_EXITSIZEMOVE snaps the window to the overlay's
// last position.
static bool s_inResize    = false;
static RECT s_lockedRect  = {};
static RECT s_lastBand    = {};

// Layered overlay updated atomically per frame via UpdateLayeredWindow with a
// 32-bit DIB. Position, size, AND pixel contents change in a single call so
// DWM can't show a half-resized state — which is what caused the moving-edge
// blink with the previous SetWindowPos + InvalidateRect / WM_PAINT path.
static HWND          s_bandHwnd  = nullptr;
static const wchar_t kBandClass[] = L"NyxResizeBand";

LRESULT CALLBACK bandProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensureBandWindow() {
    if (s_bandHwnd) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc   = bandProc;
    wc.hInstance     = hi;
    wc.lpszClassName = kBandClass;
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    s_bandHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kBandClass, L"", WS_POPUP,
        0, 0, 100, 100, nullptr, nullptr, hi, nullptr);
}

static void showBand(const RECT& r) {
    ensureBandWindow();
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;

    // Build a 32-bit BGRA DIB sized to the overlay. Premultiplied alpha is
    // the layered-window contract: each colour channel is multiplied by the
    // alpha already. For an opaque (alpha = 255) mint pixel, that's just the
    // mint colour itself.
    HDC screen = GetDC(nullptr);
    HDC mem    = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;        // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(mem, dib);

    // Zero-init = fully transparent everywhere.
    std::memset(bits, 0, static_cast<size_t>(w) * h * 4);

    // Mint border strips. Matches the splash screen accent (RGB 107,255,170)
    // at its 2 px thickness so the resize hint visually belongs with the
    // rest of the editor chrome. BGRA premultiplied byte order.
    const uint32_t mintBGRA = (0xFFu << 24) | (107u << 16) | (255u << 8) | 170u;
    const int t = 2;
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < t && y < h; ++y)
        for (int x = 0; x < w; ++x) px[y * w + x] = mintBGRA;
    for (int y = std::max(0, h - t); y < h; ++y)
        for (int x = 0; x < w; ++x) px[y * w + x] = mintBGRA;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < t && x < w; ++x) px[y * w + x] = mintBGRA;
        for (int x = std::max(0, w - t); x < w; ++x) px[y * w + x] = mintBGRA;
    }

    POINT dstPos { r.left, r.top };
    SIZE  sz     { w, h };
    POINT srcPos { 0, 0 };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(s_bandHwnd, screen,
                        &dstPos, &sz, mem, &srcPos, 0, &blend, ULW_ALPHA);

    if (!IsWindowVisible(s_bandHwnd)) ShowWindow(s_bandHwnd, SW_SHOWNOACTIVATE);

    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static void hideBand() {
    if (s_bandHwnd) ShowWindow(s_bandHwnd, SW_HIDE);
}

LRESULT CALLBACK nyxWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ENTERSIZEMOVE:
            // Modal loop begins (resize OR move). Capture the starting rect;
            // s_inResize flips on the first WM_SIZING and stays off for moves
            // so dragging the title bar keeps live behaviour.
            GetWindowRect(hwnd, &s_lockedRect);
            s_lastBand = s_lockedRect;
            s_inResize = false;
            break;
        case WM_SIZING: {
            // Lock the window at its pre-drag rect by overwriting Windows'
            // proposed rect; track the cursor's real proposed rect via the
            // overlay window so we can snap to it on release.
            RECT* prc = reinterpret_cast<RECT*>(lp);
            RECT proposed = *prc;
            *prc = s_lockedRect;
            s_inResize = true;
            showBand(proposed);
            s_lastBand = proposed;
            return TRUE;
        }
        case WM_SIZE:
        case WM_WINDOWPOSCHANGED: {
            // Outside the modal resize loop the window can size normally.
            LRESULT r = CallWindowProc(g_prevProc, hwnd, msg, wp, lp);
            applyRectRegion(hwnd);
            return r;
        }
        case WM_EXITSIZEMOVE:
            // Modal loop ends. Hide the overlay, then snap the window to the
            // rect the user landed on; the chained WM_SIZE handles
            // applyRectRegion + GLFW resize callback.
            hideBand();
            if (s_inResize) {
                SetWindowPos(hwnd, nullptr,
                             s_lastBand.left, s_lastBand.top,
                             s_lastBand.right  - s_lastBand.left,
                             s_lastBand.bottom - s_lastBand.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            s_inResize = false;
            if (s_liveResizeCb) s_liveResizeCb();
            break;
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
    // Apply Aero Snap/move behavior etc. first; we'll re-set the corner
    // attribute AFTER the frame change so DWM picks it up against the new
    // window style.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Disable Windows 11 auto-rounded corners — the OS rounds the top two
    // corners only (the bottom are squared off when WM_NCCALCSIZE removes the
    // frame), which looks asymmetric. DWMWCP_DONOTROUND forces hard 90° on all
    // four. No-op on Windows 10 (the call returns DWM_E_NOT_SUPPORTED). Apply
    // after SetWindowPos — applying before, then triggering SWP_FRAMECHANGED,
    // can leave DWM's corner preference unread on some Win11 builds.
    DWORD pref = DWMWCP_DONOTROUND;
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    if (FAILED(hr)) {
        // Older DWM (pre-22000) doesn't know the attribute. That's fine — those
        // builds don't round corners anyway. Anything else is unexpected.
        LOG_INFO("DwmSetWindowAttribute(corner=DONOTROUND) hr=0x{:x} (skipped on Win10)",
                 static_cast<unsigned>(hr));
    }
    // Force the window shape to be rectangular. Belt-and-suspenders on Win11
    // builds where DWMWCP_DONOTROUND is silently ignored — DWM can't round a
    // shape that's already explicitly rectangular. WM_SIZE / WM_WINDOWPOSCHANGED
    // re-apply this on every resize.
    applyRectRegion(hwnd);
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

void Window::setLiveResizeCallback(std::function<void()> cb) {
#ifdef _WIN32
    s_liveResizeCb = std::move(cb);
#else
    (void)cb;
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
