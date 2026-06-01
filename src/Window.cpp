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

// Custom maximize state — see the longer comment near enterCustomMaximize for
// why we bypass the OS WS_MAXIMIZE flag. Declared up here so nyxHitTest (just
// below) can read it.
static bool s_customMax = false;
static RECT s_savedWindowedRect = {};

LRESULT nyxHitTest(HWND hwnd, LPARAM lParam) {
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    RECT rc; GetWindowRect(hwnd, &rc);
    int x = pt.x - rc.left, y = pt.y - rc.top;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const int B = 6;  // resize-border thickness

    if (!IsZoomed(hwnd) && !s_customMax) {       // edges only when not maximized
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

// Rubber-band resize state. WM_SIZING locks the window at its pre-drag size
// and tracks the user's proposed rect via a layered overlay window that
// floats above all others. WM_EXITSIZEMOVE snaps the window to the overlay's
// last position.
static bool s_inResize    = false;
static RECT s_lockedRect  = {};
static RECT s_lastBand    = {};
// True when WM_ENTERSIZEMOVE fired with the window already maximized — the
// modal loop that follows is the OS restoring + dragging the window after a
// title-bar grab, and we want it to proceed natively (no rubber-band lock).
static bool s_modalFromMax = false;

// Custom maximize implementation. We do NOT use the OS's WS_MAXIMIZE flag
// because for borderless WS_OVERLAPPEDWINDOW windows the OS maximize inflates
// the rect by the frame thickness on every side, expecting a native frame to
// absorb it — which we don't draw, so the overhang clips UI off-screen on
// every edge. Instead, when "maximize" is requested we save the windowed rect,
// SetWindowPos to the current monitor's work area, and track the state
// ourselves (s_customMax above). Restore just SetWindowPos's back to the
// saved rect.

static void enterCustomMaximize(HWND hwnd) {
    if (s_customMax) return;
    GetWindowRect(hwnd, &s_savedWindowedRect);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return;
    s_customMax = true;
    SetWindowPos(hwnd, nullptr,
                 mi.rcWork.left, mi.rcWork.top,
                 mi.rcWork.right  - mi.rcWork.left,
                 mi.rcWork.bottom - mi.rcWork.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void exitCustomMaximize(HWND hwnd) {
    if (!s_customMax) return;
    s_customMax = false;
    SetWindowPos(hwnd, nullptr,
                 s_savedWindowedRect.left, s_savedWindowedRect.top,
                 s_savedWindowedRect.right  - s_savedWindowedRect.left,
                 s_savedWindowedRect.bottom - s_savedWindowedRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

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
            // s_customMax means the window is "maximized" via SetWindowPos to
            // the work area (no WS_MAXIMIZE flag). On a title-bar drag we exit
            // custom max in TitleBar before SC_MOVE, so by the time this fires
            // the modal loop is already over a small window — no rubber-band
            // lockout needed. The flag still kicks in for the legacy IsZoomed
            // case in case anything else sets it.
            s_modalFromMax = IsZoomed(hwnd) != FALSE;
            break;
        case WM_SIZING: {
            // Drag-from-maximized fires WM_SIZING as Windows restores the
            // window. Our rubber-band lock would freeze that restore and just
            // show a mint outline — clearly the wrong UX. Let the OS finish
            // its restore natively whenever the modal loop began in a
            // maximized state.
            if (s_modalFromMax) break;
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
        case WM_GETMINMAXINFO: {
            // Tell the OS the maximize rect IS the monitor's work area. Helps
            // the glfwMaximizeWindow path land the window itself at the work
            // area. Aero Snap and SC_MAXIMIZE-via-SendMessage paths bypass
            // this entirely (the WM_NCCALCSIZE clamp below is the safety net
            // for those).
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{}; mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                mmi->ptMaxPosition.x = mi.rcWork.left  - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = mi.rcWork.top   - mi.rcMonitor.top;
                mmi->ptMaxSize.x     = mi.rcWork.right  - mi.rcWork.left;
                mmi->ptMaxSize.y     = mi.rcWork.bottom - mi.rcWork.top;
                return 0;
            }
            break;
        }
        case WM_SYSCOMMAND: {
            // Intercept any OS maximize request (max box, double-click caption,
            // Win+Up, Aero Snap top) and route through our custom maximize
            // which SetWindowPos's the window directly to the work area —
            // bypassing the OS maximize state machine entirely.
            UINT cmd = static_cast<UINT>(wp) & 0xFFF0;
            if (cmd == SC_MAXIMIZE) {
                enterCustomMaximize(hwnd);
                return 0;
            }
            if (cmd == SC_RESTORE && s_customMax) {
                exitCustomMaximize(hwnd);
                return 0;
            }
            break;
        }
        case WM_WINDOWPOSCHANGING: {
            // Catch any direct-SetWindowPos maximize (Aero Snap, etc.) that
            // bypasses WM_SYSCOMMAND. Detection: proposed rect overhangs the
            // work area on ALL FOUR sides — only a maximize produces that.
            // Rewrite to the work area before commit. Combined with the
            // WM_SYSCOMMAND intercept above, every maximize path lands at
            // the work area.
            auto* wpos = reinterpret_cast<WINDOWPOS*>(lp);
            if ((wpos->flags & (SWP_NOSIZE | SWP_NOMOVE)) != (SWP_NOSIZE | SWP_NOMOVE)) {
                RECT cur; GetWindowRect(hwnd, &cur);
                int x  = (wpos->flags & SWP_NOMOVE) ? cur.left : wpos->x;
                int y  = (wpos->flags & SWP_NOMOVE) ? cur.top  : wpos->y;
                int cx = (wpos->flags & SWP_NOSIZE) ? (cur.right  - cur.left) : wpos->cx;
                int cy = (wpos->flags & SWP_NOSIZE) ? (cur.bottom - cur.top)  : wpos->cy;
                HMONITOR mon = MonitorFromPoint(POINT{ x + cx/2, y + cy/2 },
                                                MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                if (mon && GetMonitorInfoW(mon, &mi)) {
                    bool xOver = x < mi.rcWork.left && (x + cx) > mi.rcWork.right;
                    bool yOver = y < mi.rcWork.top  && (y + cy) > mi.rcWork.bottom;
                    if (xOver && yOver) {
                        wpos->x  = mi.rcWork.left;
                        wpos->y  = mi.rcWork.top;
                        wpos->cx = mi.rcWork.right  - mi.rcWork.left;
                        wpos->cy = mi.rcWork.bottom - mi.rcWork.top;
                        wpos->flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
                    }
                }
            }
            break;
        }
        case WM_SIZE: {
            LRESULT r = CallWindowProc(g_prevProc, hwnd, msg, wp, lp);
            applyRectRegion(hwnd);
            return r;
        }
        case WM_WINDOWPOSCHANGED: {
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
            s_inResize     = false;
            s_modalFromMax = false;
            // (Live-resize hook moved to enter/exit modal callbacks; this
            // WndProc is dormant — installNativeChrome is no longer called.)
            break;
        case WM_NCCALCSIZE:
            if (wp == TRUE) {
                // Canonical Chrome/VSCode borderless pattern: when the window
                // is maximized, clamp the CLIENT rect to the monitor's work
                // area regardless of the window rect. The overhang NC strip
                // is invisible (off-screen or behind taskbar) since we draw
                // no frame. Vulkan's surface sizes off the client area, so
                // the framebuffer lands on the visible work area exactly.
                // The WM_SIZE handler above forces NCCALCSIZE to re-run after
                // WS_MAXIMIZE is set, since on the OS's first call here
                // IsZoomed is still false during the maximize transition.
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                if (IsZoomed(hwnd)) {
                    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                    if (mon && GetMonitorInfoW(mon, &mi)) {
                        p->rgrc[0] = mi.rcWork;
                    }
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

    // Standard OS-decorated window. The OS handles the title bar, maximize
    // (which goes exactly to the work area, no overhang), Aero Snap, and
    // drag-from-maximized natively. Our previous custom borderless chrome
    // had compounding maximize-rect bugs that aren't worth re-fighting.
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);

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

    // No installNativeChrome — we use GLFW's default decorated window now,
    // so the OS WndProc handles everything itself. See the GLFW_DECORATED hint
    // above for context.

#ifdef _WIN32
    // Match the OS title bar to the Windows system theme. By default, the
    // per-app dark-mode flag is off, so the title bar paints white even when
    // Windows is in dark mode. Read HKCU\...\Personalize\AppsUseLightTheme
    // (0 = dark, 1 = light) and toggle the DWM immersive-dark-mode attribute
    // to match.
    {
        HWND hwnd = glfwGetWin32Window(m_window);
        DWORD appsUseLightTheme = 1;
        DWORD sz = sizeof(appsUseLightTheme);
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&appsUseLightTheme), &sz);
            RegCloseKey(hKey);
        }
        BOOL useDark = (appsUseLightTheme == 0) ? TRUE : FALSE;
        // DWMWA_USE_IMMERSIVE_DARK_MODE was 19 on Win10 build 18362 then renumbered
        // to 20 on Win10 build 19041+. Try the newer first, fall back to the older.
        constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_NEW = 20;
        constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_OLD = 19;
        if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_NEW,
                                         &useDark, sizeof(useDark)))) {
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD,
                                  &useDark, sizeof(useDark));
        }
    }
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

void Window::getPosition(int& x, int& y) const {
    x = y = 0;
    if (m_window) glfwGetWindowPos(m_window, &x, &y);
}

void Window::setPosition(int x, int y) {
    if (m_window) glfwSetWindowPos(m_window, x, y);
}

void Window::setSize(int w, int h) {
    if (!m_window || w <= 0 || h <= 0) return;
    glfwSetWindowSize(m_window, w, h);
    m_width  = w;
    m_height = h;   // the framebuffer-resize callback also fires; this keeps the cached size in sync
}

bool Window::isMaximized() const {
#ifdef _WIN32
    return s_customMax;
#else
    return m_window && glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) != 0;
#endif
}

void Window::maximize() {
#ifdef _WIN32
    if (m_window) enterCustomMaximize(glfwGetWin32Window(m_window));
#else
    if (m_window) glfwMaximizeWindow(m_window);
#endif
}

void toggleCustomMaximize(GLFWwindow* w) {
#ifdef _WIN32
    if (!w) return;
    HWND hwnd = glfwGetWin32Window(w);
    if (s_customMax) exitCustomMaximize(hwnd);
    else             enterCustomMaximize(hwnd);
#else
    (void)w;
#endif
}

bool isCustomMaximized(GLFWwindow* w) {
#ifdef _WIN32
    (void)w;
    return s_customMax;
#else
    (void)w;
    return false;
#endif
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

std::string Window::openFolderDialog(const std::string& title,
                                     const std::string& initialDir) {
#ifdef _WIN32
    std::string result;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        std::wstring wtitle(title.begin(), title.end());
        dlg->SetTitle(wtitle.c_str());

        // Open the dialog in initialDir if it's a real directory. SetFolder
        // wins over the OS's last-used-folder memory, which is what we want
        // when steering the user to the engine's projects/ root rather than
        // wherever they last opened a dialog.
        if (!initialDir.empty()) {
            std::error_code ec;
            std::filesystem::path abs = std::filesystem::absolute(initialDir, ec);
            if (!ec && std::filesystem::exists(abs)) {
                std::wstring wdir = abs.make_preferred().wstring();
                IShellItem* folderItem = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(
                        wdir.c_str(), nullptr, IID_PPV_ARGS(&folderItem)))) {
                    dlg->SetFolder(folderItem);
                    folderItem->Release();
                }
            }
        }

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
