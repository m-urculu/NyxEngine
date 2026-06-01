# Building Nyx as a native Windows app (MSVC)

This builds Nyx as a **real native Windows executable** using Visual Studio +
MSVC — not the WSL/WSLg Linux bridge. On native Win32, window focus, taskbar
restore, and the custom title bar all behave correctly.

## 1. Install the prerequisites (one time)

1. **Visual Studio 2022** (Community is free) — during install, check the
   **"Desktop development with C++"** workload. This gives you MSVC, CMake, and
   Ninja. (Visual Studio *Build Tools* + VS Code with the CMake Tools extension
   also works if you prefer a lighter editor.)
2. **Vulkan SDK for Windows** — https://vulkan.lunarg.com/sdk/home#windows
   Run the installer; it sets the `VULKAN_SDK` environment variable and provides
   the loader, validation layers, and `glslc.exe` (the shader compiler CMake
   uses). Reboot or re-open your shell afterward so `VULKAN_SDK` is visible.
3. A GPU with up-to-date Vulkan drivers (any modern NVIDIA/AMD/Intel).

## 2. Get the project onto the Windows filesystem

The project must live on your Windows drive (e.g. `C:\Users\mrcel\NyxEngine`),
**not** under `\\wsl$\...`, for MSVC to build it cleanly.

There is no git remote, so this is a file copy. Two options:

- **Easiest:** ask me (in the WSL session) to copy it for you — I'll place it at
  `C:\Users\mrcel\NyxEngine` with the WSL `build/` folder excluded.
- **Manual:** copy the whole folder there yourself, then delete the `build/`
  directory (it contains the old Linux binary and CMake cache).

> The 5 git submodules (`external/glfw`, `spdlog`, `VulkanMemoryAllocator`,
> `glm`, `tinyobjloader`) already have their contents present, so they come
> along with the copy. `external/stb` and `external/cgltf` are vendored directly.

## 3. Configure & build

### Option A — Visual Studio (GUI)
1. `File > Open > Folder…` and pick the `NyxEngine` folder.
2. VS detects `CMakePresets.json`. Pick the **"Windows x64 (MSVC)"** configuration
   in the toolbar.
3. `Build > Build All`. The build also compiles the shaders to `.spv`.
4. Set **Nyx** as the startup item and press **F5**. The debugger working
   directory is preconfigured to the project root, so `shaders/` and `assets/`
   resolve automatically.

### Option B — command line (Developer PowerShell for VS 2022)
```powershell
cd C:\Users\mrcel\NyxEngine
cmake --preset windows-msvc
cmake --build --preset windows-debug
.\build\Debug\Nyx.exe        # run from the project root
```

## 4. Running outside the debugger
The app loads `shaders\*.spv` and `projects\Sandbox\assets\...` via relative
paths, but at startup it locates the project root from the executable's own
location (the nearest ancestor containing `shaders/` and `projects/`) and sets
it as the working directory. So it runs correctly regardless of how it's
launched — the desktop shortcut, a double-clicked `build\Debug\Nyx.exe`, or from
any directory:
```powershell
.\build\Debug\Nyx.exe        # works from anywhere
```

## Notes
- Validation layers are enabled (they come with the Vulkan SDK). Console output
  uses spdlog.
- Window controls (custom title bar): drag to move, edges/corners to resize,
  minimize / maximize / close buttons, **F11** toggles fullscreen.
- If CMake reports *"glslc not found"*, your Vulkan SDK isn't on `PATH` /
  `VULKAN_SDK` isn't set — re-open the shell after installing the SDK.
