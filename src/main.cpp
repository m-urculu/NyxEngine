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
#include <fstream>
#include <sstream>
#include <string>
#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>   // CommandLineToArgvW
#endif

namespace {

// The directory the running executable lives in (empty on failure).
std::filesystem::path exeDirectory() {
    namespace fs = std::filesystem;
    std::error_code ec;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(std::wstring(buf, n)).parent_path();
#else
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : p.parent_path();
#endif
}

// Resources (shaders/, projects/) are loaded via paths relative to the working
// directory. Rather than require the app be launched from the project root
// (which a desktop shortcut or a double-clicked exe won't guarantee), locate the
// project root from the executable's own location — the nearest ancestor folder
// that contains both shaders/ and projects/ — and make it the working directory.
void setWorkingDirToProjectRoot() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path start = exeDirectory();
    if (start.empty()) return;

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
struct LaunchArgs { std::string playScene; std::string playProject; std::string exportDest; };

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
            else if (a == L"--export"  && i + 1 < wargc) out.exportDest  = toUtf8(wargv[++i]);
        }
        LocalFree(wargv);
    }
#else
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--play"    && i + 1 < argc) out.playScene   = argv[++i];
        else if (a == "--project" && i + 1 < argc) out.playProject = argv[++i];
        else if (a == "--export"  && i + 1 < argc) out.exportDest  = argv[++i];
    }
#endif
    return out;
}

// A shipped game has a game.cfg NEXT TO THE EXE (written by File > Export Game):
//   scene   projects/<Name>/scenes/<file>.scene
//   project projects/<Name>
// When present (and no explicit --play was given), boot straight into that scene
// like --play does. This is what turns an exported folder into a double-click game.
//
// We look beside the exe (not in the cwd) and chdir there: the exe's own folder
// IS the content root in an export, and relying on setWorkingDirToProjectRoot()
// could land the cwd on a different shaders/+projects/ root (e.g. the dev tree
// if the export sits inside it), which would silently fall back to the editor.
void readGameConfig(LaunchArgs& la) {
    namespace fs = std::filesystem;
    if (!la.playScene.empty()) return;                 // explicit --play wins
    std::error_code ec;
    fs::path dir = exeDirectory();
    fs::path cfg = dir / "game.cfg";
    if (dir.empty() || !fs::exists(cfg, ec)) return;
    // The exe's folder is the content root — make it the cwd so the config's
    // relative scene/project paths resolve regardless of where we were launched.
    fs::current_path(dir, ec);
    std::ifstream f(cfg);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string key; ss >> key;
        std::string rest; std::getline(ss, rest);
        size_t s = rest.find_first_not_of(" \t");
        rest = (s == std::string::npos) ? "" : rest.substr(s);
        if      (key == "scene")   la.playScene   = rest;
        else if (key == "project") la.playProject = rest;
    }
}

} // namespace

int main(int argc, char** argv) {
    setWorkingDirToProjectRoot();

    LaunchArgs la = parseArgs(argc, argv);
    readGameConfig(la);                                // exported build → boot the game
    const bool gameMode   = !la.playScene.empty();
    const bool exportMode = !gameMode && !la.exportDest.empty();  // headless: export then exit
    const bool showSplash = !gameMode && !exportMode;

    // Splash first (editor only) — shown before any heavy work so the user sees
    // something immediately after double-clicking the exe. The game process boots
    // straight into the scene with no splash.
    Nyx::Splash splash;
    if (showSplash) splash.show();

    try {
        Nyx::Engine engine;
        if (gameMode) engine.setGameMode(la.playScene, la.playProject);

        Nyx::Engine::StatusFn statusCb;
        if (showSplash) statusCb = [&](const std::string& s, float p){ splash.setStatus(s, p); };
        engine.init(statusCb);

        if (showSplash) splash.close();
        if (exportMode) { engine.exportGame(la.exportDest); return EXIT_SUCCESS; }
        engine.run();
    } catch (const std::exception& e) {
        if (showSplash) splash.close();
        // If anything goes wrong, print the error
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
