# Project status — Nyx engine

_Last updated: 2026-05-25_

## What this is
Nyx — a learning-focused **C++17 / Vulkan** renderer (GLFW, GLM, spdlog, VMA,
tinyobjloader, stb_image, cgltf). Progression so far: triangle → 3D meshes +
Blinn-Phong → ECS architecture → multi-light + per-material PBR → custom
borderless title bar (drag / resize / min-max-close). Reports as v0.3.0.

## Active work: going native Windows (MSVC)
We are moving off the WSL2/WSLg Linux build to a **native Windows app built with
Visual Studio 2022 + MSVC + the Windows Vulkan SDK**.

**Why:** the custom title bar had a "two clicks to focus" bug after minimize →
taskbar-restore. Root cause was **not** the title bar — it was running a Linux
binary through the WSLg X/Wayland→Windows bridge, where the activation click is
eaten and `glfwFocusWindow` is ignored. Native Win32 handles focus/activation
correctly, so the bug disappears and the custom title bar stays.

## What's been done
- Removed the WSLg focus band-aids (`pumpFocus`, iconify/focus re-focus hacks,
  forced-X11 init hint) and all debug logging. Window stays borderless
  (`GLFW_DECORATED FALSE`) for the custom title bar.
- `CMakeLists.txt`: MSVC-aware (`/W4`, `/utf-8`, `NOMINMAX`,
  `WIN32_LEAN_AND_MEAN`, `_CRT_SECURE_NO_WARNINGS`), sets the VS debugger working
  directory to the project root, and **auto-compiles `shaders/*.vert|frag` to
  `.spv`** via the Vulkan SDK's `glslc` (the `.spv` are intentionally not
  committed).
- `CMakePresets.json` — `windows-msvc` (VS 2022, x64) and `linux` presets.
- `WINDOWS_SETUP.md` — full install + build instructions.
- Verified it still configures / compiles / links with GCC (Linux sanity build).

## This is the canonical copy
`C:\Users\mrcel\NyxEngine` is the working copy (build it here with MSVC).
The original WSL copy at `/home/marcelo/games/VulkanEngine` is the leftover and
can be deleted once the Windows build runs.

## Open / next
- **First MSVC build not yet verified** — Claude Code runs in WSL and can't drive
  MSVC. Likely first-build snags: a submodule that needs different flags on MSVC,
  or a `/W4` warning. Build, then paste any errors.
- Decide later: keep custom title bar vs. native OS decorations (custom works on
  Win32; native is one line if preferred).
- Cosmetic: the on-disk folder is renamed to `NyxEngine`; the app/product is "Nyx Engine".
