#pragma once

// Splash.h — Native borderless splash window shown during engine startup.
//
// Comes up instantly when the user runs the exe (pure Win32 — no GLFW, no
// Vulkan, no GPU work), then updates a status line + progress bar as the
// engine progresses through its init phases. Closed right before the main
// editor window is revealed, so the user is never staring at a blank desktop.
//
// Rendered with the engine's PixelFont (Win32-side blits via GDI), so the look
// matches the rest of the editor — same chunky pixel glyphs, same palette.

#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Nyx {

class Splash {
public:
    Splash() = default;
    ~Splash();

    Splash(const Splash&) = delete;
    Splash& operator=(const Splash&) = delete;

    // Create and show the splash window. Centred on the primary monitor,
    // topmost, no taskbar entry.
    void show();

    // Update the status line and progress bar. progress in [0,1] fills the bar;
    // progress < 0 leaves the previous fill in place (use for phases where you
    // only want to change the label). Repaints + drains the Win32 message queue
    // so the update isn't delayed by a long blocking phase that follows.
    void setStatus(const std::string& message, float progress = -1.0f);

    // Destroy the window and clean up GDI resources.
    void close();

private:
#ifdef _WIN32
    HWND        m_hwnd     = nullptr;
    std::string m_status;
    float       m_progress = 0.0f;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void pumpMessages();
#endif
};

} // namespace Nyx
