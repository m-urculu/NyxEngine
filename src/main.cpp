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
#include <string>
#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>   // CommandLineToArgvW
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

// Parse the command line for `--play <scene>` (and optional `--project <proj>`).
// When --play is present the exe boots as the standalone game/play process the
// editor spawns when you hit Play, rather than as the editor itself. On Windows
// we read the wide command line so non-ASCII project paths survive.
struct LaunchArgs { std::string playScene; std::string playProject; };

LaunchArgs parseArgs(int argc, char** argv) {
    LaunchArgs out;
#ifdef _WIN32
    (void)argc; (void)argv;
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        auto toUtf8 = [](const wchar_t* w) -> std::string {
            int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (len <= 1) return {};
            std::string s(static_cast<size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
            return s;
        };
        for (int i = 1; i < wargc; ++i) {
            std::wstring a = wargv[i];
            if      (a == L"--play"    && i + 1 < wargc) out.playScene   = toUtf8(wargv[++i]);
            else if (a == L"--project" && i + 1 < wargc) out.playProject = toUtf8(wargv[++i]);
        }
        LocalFree(wargv);
    }
#else
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--play"    && i + 1 < argc) out.playScene   = argv[++i];
        else if (a == "--project" && i + 1 < argc) out.playProject = argv[++i];
    }
#endif
    return out;
}

} // namespace

int main(int argc, char** argv) {
    setWorkingDirToProjectRoot();

    LaunchArgs la = parseArgs(argc, argv);
    const bool gameMode = !la.playScene.empty();

    // Splash first (editor only) — shown before any heavy work so the user sees
    // something immediately after double-clicking the exe. The game process boots
    // straight into the scene with no splash.
    Nyx::Splash splash;
    if (!gameMode) splash.show();

    try {
        Nyx::Engine engine;
        if (gameMode) engine.setGameMode(la.playScene, la.playProject);

        Nyx::Engine::StatusFn statusCb;
        if (!gameMode) statusCb = [&](const std::string& s, float p){ splash.setStatus(s, p); };
        engine.init(statusCb);

        if (!gameMode) splash.close();
        engine.run();
    } catch (const std::exception& e) {
        if (!gameMode) splash.close();
        // If anything goes wrong, print the error
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
