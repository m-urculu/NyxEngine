# Nyx Engine

A small **C++17 / Vulkan** game engine built for **procedural worlds on low-spec
machines** — the goal is to generate big, explorable scenes that still run well
on modest GPUs. It ships with an in-engine editor (ECS scene authoring, PBR
material editing, play mode) and a chunked-LOD planet you can fly down to and
walk around.

> Status: a personal/learning project, evolving fast. Native Windows (MSVC) is
> the primary target.

## Run it instantly (prebuilt, no build needed)

A ready-to-run Windows build of the **engine/editor** is committed under
[`dist/`](dist/). After cloning, just double-click (Windows x64):

```
dist\Nyx.exe
```

It launches the **Nyx editor** with a demo scene already loaded — a procedural
planet, a light, and the scene hierarchy / inspector / content browser panels.
The only requirement is a GPU with **Vulkan drivers** (which ship with any
modern NVIDIA/AMD/Intel driver); no Vulkan SDK or runtime install needed.

To build from source (and edit the engine itself) instead, see below.

## Features

- **Vulkan renderer** — depth pre-pass, full PBR (albedo / normal / roughness /
  metallic / AO), analytic procedural-sky IBL + matching skybox, directional
  sun **shadow mapping** (PCF), **HDR** scene target with mip-chain **bloom**,
  ACES tonemapping, and cheap wrap-diffuse **subsurface scattering** for skin.
- **Skeletal animation** — skinned glTF playback.
- **Asset pipeline** — glTF (cgltf) and OBJ (tinyobjloader); FBX/COLLADA import
  via Assimp; `stb_image` for textures. Helper Blender conversion scripts
  (`gladconv.py`, `hairconv.py`) for PBR/alpha-cutout assets.
- **Chunked-LOD planet** — a cube-sphere quadtree streamed on worker threads with
  LRU eviction, double-precision radial placement, and floating-origin
  (camera-relative) rendering, so you can go from standing on the surface to
  seeing the whole planet from space without popping or clipping.
- **In-engine editor** — ECS scene graph, `.scene` save/load, multi-light setup,
  per-material inspector sliders, full undo/redo, play mode, and project export.
- **Custom borderless window** — drag/resize, min/max/close, F11 fullscreen.

## Building (native Windows, MSVC)

**Prerequisites**

1. **Visual Studio 2022** with the *Desktop development with C++* workload
   (gives you MSVC, CMake, and Ninja).
2. **Vulkan SDK for Windows** — <https://vulkan.lunarg.com/sdk/home#windows>.
   It sets `VULKAN_SDK` and provides the loader, validation layers, and `glslc`
   (the shader compiler the build uses). Re-open your shell after installing.
3. A GPU with up-to-date Vulkan drivers.

**Clone (with submodules)**

```powershell
git clone --recurse-submodules https://github.com/m-urculu/NyxEngine.git
# already cloned without --recurse-submodules?
git submodule update --init --recursive
```

**Configure & build**

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release   # or windows-debug
```

The build also compiles `shaders/*.vert|frag` to `.spv` automatically. Or just
open the folder in Visual Studio 2022 — it picks up `CMakePresets.json`.

See [WINDOWS_SETUP.md](WINDOWS_SETUP.md) for the full walkthrough.

## Dependencies

Git submodules under `external/`: **glfw**, **glm**, **spdlog**,
**VulkanMemoryAllocator**, **tinyobjloader**. `stb` and `cgltf` are vendored
directly. Assimp is fetched by CMake.

## Repository layout

- `src/` — engine source (renderer, ECS, planet subsystem, window/input, editor).
- `shaders/` — GLSL sources (compiled to SPIR-V at build time).
- `assets/`, `resources/` — engine-side fonts, icon, and shared resources.
- `tools/` — dev tools and asset-conversion scripts.

This repository is **engine-only**. Game *projects* (their scenes, gameplay
scripts, and the procedural terrain field the planet subsystem samples) live and
ship separately, so the engine is consumed alongside a project rather than built
fully standalone.

## License

No license specified yet — all rights reserved by the author until one is added.
