#include "ui/Splash.h"
#include "ui/PixelFont.h"

#ifdef _WIN32

namespace Nyx {

namespace {
constexpr wchar_t kClass[] = L"NyxSplashWnd";

// Engine palette — kept in sync with the in-editor UI panels so the splash
// visually belongs to the same product.
constexpr COLORREF kBg        = RGB(17, 18, 22);     // panel canvas
constexpr COLORREF kBgDeep    = RGB(23, 25, 32);     // bar background
constexpr COLORREF kAccent    = RGB(107, 255, 170);  // mint (titlebar + section headers)
constexpr COLORREF kBorder    = RGB(56, 60, 76);
constexpr COLORREF kTitle     = RGB(222, 228, 238);
constexpr COLORREF kSubtitle  = RGB(115, 168, 220);  // section-header blue
constexpr COLORREF kStatus    = RGB(170, 178, 195);

constexpr int kWidth  = 480;
constexpr int kHeight = 270;

// Draw a string with PixelFont (5×7 bitmap glyphs at `scale`). Each "on" pixel
// becomes a `scale × scale` filled rectangle — same look as the in-engine UI.
void drawPixelText(HDC hdc, int x, int y, int scale,
                   const std::string& text, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    int cx = x;
    for (char c : text) {
        const uint8_t* rows = PixelFont::glyphRows(c);
        if (rows) {
            for (int r = 0; r < PixelFont::CELL_H; ++r) {
                uint8_t bits = rows[r];
                for (int col = 0; col < PixelFont::CELL_W; ++col) {
                    if (bits & (1 << (PixelFont::CELL_W - 1 - col))) {
                        RECT px = { cx + col * scale,
                                    y  + r   * scale,
                                    cx + (col + 1) * scale,
                                    y  + (r   + 1) * scale };
                        FillRect(hdc, &px, brush);
                    }
                }
            }
        }
        cx += PixelFont::ADVANCE * scale;
    }
    DeleteObject(brush);
}

int pixelTextWidth(const std::string& text, int scale) {
    return static_cast<int>(text.size()) * PixelFont::ADVANCE * scale;
}

// Trim from the FRONT with a leading "…" if the string would overflow a width
// budget. Front-trim matches how a file path overflows in the engine's content
// browser — keep the tail so the file name stays visible.
std::string fitToWidth(const std::string& s, int maxPxWidth, int scale) {
    if (pixelTextWidth(s, scale) <= maxPxWidth) return s;
    const std::string ell = "...";
    int budget = maxPxWidth - pixelTextWidth(ell, scale);
    if (budget <= 0) return ell;
    int perGlyph = PixelFont::ADVANCE * scale;
    int keep = budget / perGlyph;
    if (keep >= (int)s.size()) return s;
    return ell + s.substr(s.size() - keep);
}
} // namespace

LRESULT CALLBACK Splash::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        Splash* sp = reinterpret_cast<Splash*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        // Manual double-buffer — drawing straight to hdc flickers as the
        // status line updates several times per second during startup.
        HDC     mem  = CreateCompatibleDC(hdc);
        HBITMAP bmp  = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldB = SelectObject(mem, bmp);

        // Background fill
        HBRUSH bgBrush = CreateSolidBrush(kBg);
        FillRect(mem, &rc, bgBrush);
        DeleteObject(bgBrush);

        // 1px outer border + 2px top accent strip.
        HBRUSH borderBrush = CreateSolidBrush(kBorder);
        RECT topB    = { rc.left,        rc.top,         rc.right,        rc.top + 1     };
        RECT bottomB = { rc.left,        rc.bottom - 1,  rc.right,        rc.bottom      };
        RECT leftB   = { rc.left,        rc.top,         rc.left + 1,     rc.bottom      };
        RECT rightB  = { rc.right - 1,   rc.top,         rc.right,        rc.bottom      };
        FillRect(mem, &topB,    borderBrush);
        FillRect(mem, &bottomB, borderBrush);
        FillRect(mem, &leftB,   borderBrush);
        FillRect(mem, &rightB,  borderBrush);
        DeleteObject(borderBrush);

        HBRUSH accentBrush = CreateSolidBrush(kAccent);
        RECT accent = { rc.left + 1, rc.top + 1, rc.right - 1, rc.top + 3 };
        FillRect(mem, &accent, accentBrush);
        DeleteObject(accentBrush);

        // Everything at the in-editor UI scale (1) so the splash visually
        // belongs with every other panel. Title is only differentiated by
        // colour, not size.
        const int scale = 1;

        // ── Title: NYX ENGINE ──
        {
            const std::string title = "NYX ENGINE";
            int w = pixelTextWidth(title, scale);
            drawPixelText(mem, (rc.right - w) / 2, 95, scale, title, kTitle);
        }

        // ── Subtitle: version line ──
        {
            const std::string sub = "v0.3.0  PHASE 3B";
            int w = pixelTextWidth(sub, scale);
            drawPixelText(mem, (rc.right - w) / 2, 115, scale, sub, kSubtitle);
        }

        // ── Status line (truncated from the front if too long) ──
        {
            const int padX   = 18;
            const int budget = rc.right - 2 * padX;
            std::string raw = sp ? sp->m_status : std::string("Starting...");
            if (raw.empty()) raw = "Starting...";
            std::string text = fitToWidth(raw, budget, scale);
            drawPixelText(mem, padX, 190, scale, text, kStatus);
        }

        // ── Progress bar ──
        {
            const int barH = 6;
            const int padX = 18;
            const int x0   = padX;
            const int x1   = rc.right - padX;
            const int y0   = 210;
            const int y1   = y0 + barH;

            // Bar background + 1px border.
            HBRUSH bgBar = CreateSolidBrush(kBgDeep);
            RECT bgR = { x0, y0, x1, y1 };
            FillRect(mem, &bgR, bgBar);
            DeleteObject(bgBar);

            HBRUSH bordBar = CreateSolidBrush(kBorder);
            RECT bT = { x0, y0,       x1, y0 + 1 };
            RECT bB = { x0, y1 - 1,   x1, y1     };
            RECT bL = { x0, y0,       x0 + 1, y1 };
            RECT bR = { x1 - 1, y0,   x1, y1     };
            FillRect(mem, &bT, bordBar); FillRect(mem, &bB, bordBar);
            FillRect(mem, &bL, bordBar); FillRect(mem, &bR, bordBar);
            DeleteObject(bordBar);

            // Mint fill, clipped to [0,1] of the inner width.
            float p = sp ? sp->m_progress : 0.0f;
            if (p < 0.0f) p = 0.0f;
            if (p > 1.0f) p = 1.0f;
            int innerW = (x1 - x0) - 2;
            int fillW  = static_cast<int>(innerW * p + 0.5f);
            if (fillW > 0) {
                HBRUSH fillBrush = CreateSolidBrush(kAccent);
                RECT fillR = { x0 + 1, y0 + 1, x0 + 1 + fillW, y1 - 1 };
                FillRect(mem, &fillR, fillBrush);
                DeleteObject(fillBrush);
            }
        }

        // Blit the off-screen buffer to the real DC in one go.
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldB);
        DeleteObject(bmp);
        DeleteDC(mem);

        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;   // painted in WM_PAINT — skip flicker fill
    return DefWindowProcW(hwnd, msg, wp, lp);
}

Splash::~Splash() { close(); }

void Splash::show() {
    if (m_hwnd) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &Splash::WndProc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // Centre on the primary monitor.
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    int x  = (sx - kWidth)  / 2;
    int y  = (sy - kHeight) / 2;

    m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                             kClass, L"Nyx", WS_POPUP,
                             x, y, kWidth, kHeight, nullptr, nullptr, hi, nullptr);
    if (!m_hwnd) return;
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    m_status   = "Starting...";
    m_progress = 0.0f;
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);   // don't steal focus from anything else
    UpdateWindow(m_hwnd);
    pumpMessages();
}

void Splash::setStatus(const std::string& message, float progress) {
    if (!m_hwnd) return;
    m_status = message;
    if (progress >= 0.0f) m_progress = progress;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    UpdateWindow(m_hwnd);
    pumpMessages();
}

void Splash::close() {
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    pumpMessages();
}

void Splash::pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace Nyx

#else   // !_WIN32 — splash is a no-op on non-Windows builds

namespace Nyx {
Splash::~Splash() = default;
void Splash::show() {}
void Splash::setStatus(const std::string&, float) {}
void Splash::close() {}
} // namespace Nyx

#endif
