// main.cpp — Entry point for Nyx
//
// This is where the program starts. It creates the engine,
// initializes everything, runs the game loop, and handles errors.

#include "Engine.h"
#include "Logger.h"
#include "ui/Splash.h"

#include <iostream>
#include <stdexcept>
#include <filesystem>
#ifdef _WIN32
#  include <windows.h>
#endif

namespace {

// Resources (shaders/, projects/) are loaded via paths relative to the working
// directory. Rather than require the app be launched from the project root
// (which a desktop shortcut or a double-clicked exe won't guarantee), locate the
// project root from the executable's own location — the nearest ancestor folder
// that contains both shaders/ and projects/ — and make it the working directory.
void setWorkingDirToProjectRoot() {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path start;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    start = fs::path(buf).parent_path();
#else
    start = fs::read_symlink("/proc/self/exe", ec).parent_path();
    if (ec) return;
#endif

    for (fs::path d = start; ; ) {
        if (fs::exists(d / "shaders", ec) && fs::exists(d / "projects", ec)) {
            fs::current_path(d, ec);
            return;
        }
        fs::path parent = d.parent_path();
        if (parent == d) return;   // reached the filesystem root; give up (keep cwd)
        d = parent;
    }
}

} // namespace

int main() {
    setWorkingDirToProjectRoot();

    // Splash first — shown before any heavy work so the user sees something
    // immediately after double-clicking the exe. Closed once init is done.
    Nyx::Splash splash;
    splash.show();

    try {
        Nyx::Engine engine;
        engine.init([&](const std::string& s, float p){ splash.setStatus(s, p); });
        splash.close();
        engine.run();
    } catch (const std::exception& e) {
        splash.close();
        // If anything goes wrong, print the error
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
