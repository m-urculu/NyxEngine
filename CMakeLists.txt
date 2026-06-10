cmake_minimum_required(VERSION 3.21)
project(Nyx VERSION 0.3.0 LANGUAGES CXX C)   # C enabled for the tree-sitter runtime/grammar

# ── C++17 standard ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Distribution / runtime linkage ──────────────────────────────────────────
# Statically link the C runtime so the shipped exe needs no msvcp140/vcruntime140
# redistributable on a player's machine (CMP0091 lets CMAKE_MSVC_RUNTIME_LIBRARY
# select /MT vs /MD). Build the third-party libs (GLFW / spdlog / Assimp / zlib)
# static too, so there are no loose DLLs to ship — just Nyx.exe + shaders +
# content. Must be set BEFORE add_subdirectory / FetchContent so the deps inherit
# it. (vulkan-1.dll is not bundled — it ships with the GPU driver.)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ── Find Vulkan SDK (must be installed on the system) ───────────────────────
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan found: ${Vulkan_INCLUDE_DIRS}")

# ── GLFW — windowing library (built from submodule) ─────────────────────────
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glfw)

# ── spdlog — logging library (built from submodule) ─────────────────────────
add_subdirectory(external/spdlog)

# ── GLM — header-only math library ──────────────────────────────────────────
# GLM is header-only, we just need the include path
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE external/glm)

# ── VMA — Vulkan Memory Allocator (header-only, needs one implementation file)
add_library(vma INTERFACE)
target_include_directories(vma INTERFACE external/VulkanMemoryAllocator/include)

# ── tinyobjloader — OBJ model loading (header-only) ─────────────────────
add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE external/tinyobjloader)

# ── stb — stb_image for texture loading (header-only) ───────────────────
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE external/stb)

# ── cgltf — glTF 2.0 parser (header-only) ───────────────────────────────
add_library(cgltf INTERFACE)
target_include_directories(cgltf INTERFACE external/cgltf)

# ── Assimp — multi-format mesh importer (FBX / OBJ / COLLADA / …). Pulled in
#    via FetchContent so it sits alongside the rest of the externals without
#    needing a submodule. Trimmed to the few importers we actually use; no
#    exporters, no tools, no tests so the configure + first build stay light.
include(FetchContent)
set(ASSIMP_BUILD_TESTS               OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES             OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS        OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL                   OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT                 ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER        ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER        ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_COLLADA_IMPORTER    ON  CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS        OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL_PDB               OFF CACHE BOOL "" FORCE)
FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(assimp)

# ── tree-sitter — incremental parser runtime + C/C++ grammar (editor syntax
#    highlighting). Vendored sources; the generated parser.c is huge so MSVC needs
#    /bigobj. Warnings silenced (generated/3rd-party C). ──────────────────────────
add_library(tree_sitter STATIC
    external/tree-sitter/lib/src/lib.c
    external/tree-sitter-cpp/src/parser.c
    external/tree-sitter-cpp/src/scanner.c
)
target_include_directories(tree_sitter
    PUBLIC  external/tree-sitter/lib/include          # <tree_sitter/api.h> for consumers
    PRIVATE external/tree-sitter/lib/src              # runtime internal headers
            external/tree-sitter-cpp/src              # grammar's bundled tree_sitter/parser.h
)
if(MSVC)
    target_compile_options(tree_sitter PRIVATE /bigobj /w)
else()
    target_compile_options(tree_sitter PRIVATE -w)
endif()

# ── Collect all engine source files ─────────────────────────────────────────
set(ENGINE_SOURCES
    src/main.cpp
    src/Engine.cpp
    src/Logger.cpp
    src/Window.cpp
    src/Input.cpp
    src/core/Time.cpp
    src/core/LogStore.cpp
    src/ecs/Registry.cpp
    src/ecs/systems/TransformSystem.cpp
    src/renderer/VulkanContext.cpp
    src/renderer/Swapchain.cpp
    src/renderer/Pipeline.cpp
    src/renderer/ImagePipeline.cpp
    src/renderer/MaterialPreviewPipeline.cpp
    src/renderer/Renderer.cpp
    src/renderer/VmaUsage.cpp
    src/renderer/Buffer.cpp
    src/renderer/Mesh.cpp
    src/renderer/DepthBuffer.cpp
    src/renderer/Descriptors.cpp
    src/renderer/ObjLoader.cpp
    src/renderer/StbImageUsage.cpp
    src/renderer/Texture.cpp
    src/renderer/CgltfUsage.cpp
    src/renderer/GltfLoader.cpp
    src/renderer/AssimpLoader.cpp
    src/renderer/ResourceCache.cpp
    src/renderer/ShadowMap.cpp
    src/renderer/PointShadowMap.cpp
    src/renderer/HdrTarget.cpp
    src/renderer/BloomPass.cpp
    src/planet/PlanetSystem.cpp
    src/scene/Camera.cpp
    src/ui/UIPipeline.cpp
    src/ui/TitleBar.cpp
    src/ui/ContentBrowser.cpp
    src/ui/PixelFont.cpp
    src/ui/Console.cpp
    src/ui/CodeEditor.cpp
    src/ui/Highlighter.cpp
    src/ui/SceneHierarchy.cpp
    src/ui/Inspector.cpp
    src/ui/Gizmo.cpp
    src/ui/Splash.cpp
)

# ── Project procgen scripts (C++, owned by the active project) ───────────────
# Procgen generators are PROJECT scripts, not engine code: they live in
# projects/<NYX_PROJECT>/procgen/*.cpp. Nyx has no runtime C++ scripting, so they
# are compiled into the build — drop a .cpp in that folder and reconfigure (or just
# rebuild; CONFIGURE_DEPENDS re-globs on build) and it's picked up. The project root
# is added to the include path so a script's "procgen/Foo.h" header resolves, while
# engine headers ("renderer/Vertex.h", …) resolve via src/.
set(NYX_PROJECT "New project" CACHE STRING
    "Active project whose procgen/*.cpp scripts are compiled into the build")
set(NYX_PROJECT_DIR "${CMAKE_SOURCE_DIR}/projects/${NYX_PROJECT}")
file(GLOB PROJECT_PROCGEN_SOURCES CONFIGURE_DEPENDS "${NYX_PROJECT_DIR}/procgen/*.cpp")
if(PROJECT_PROCGEN_SOURCES)
    list(APPEND ENGINE_SOURCES ${PROJECT_PROCGEN_SOURCES})
    message(STATUS "Nyx: compiling project procgen scripts from ${NYX_PROJECT_DIR}/procgen")
endif()

# Planet subsystem is OPTIONAL: the engine's src/planet code needs a project-side
# procgen::PlanetField (procgen/Planet.h). Only enable it when the active project
# actually provides that header — otherwise the engine builds planet-free (a stock
# engine has no planet; a project adds one by dropping in procgen/Planet.h + .cpp).
if(EXISTS "${NYX_PROJECT_DIR}/procgen/Planet.h")
    set(NYX_HAS_PLANET TRUE)
endif()

# Project gameplay scripts (C++): everything "in-game" — player controls, character
# behaviour, game rules — lives in projects/<NYX_PROJECT>/scripts/*.cpp, NOT in the
# engine. Same compiled-in model as procgen (no runtime scripting). The engine
# exposes a GameContext (src/game/GameScript.h) that these scripts drive each frame.
file(GLOB PROJECT_GAME_SOURCES CONFIGURE_DEPENDS "${NYX_PROJECT_DIR}/scripts/*.cpp")
if(PROJECT_GAME_SOURCES)
    list(APPEND ENGINE_SOURCES ${PROJECT_GAME_SOURCES})
    message(STATUS "Nyx: compiling project gameplay scripts from ${NYX_PROJECT_DIR}/scripts")
else()
    # No project scripts → link default no-op gameplay hooks so the engine still
    # builds standalone (a project overrides these by shipping its own scripts/).
    list(APPEND ENGINE_SOURCES src/game/GameScriptDefault.cpp)
    message(STATUS "Nyx: no project scripts in '${NYX_PROJECT}' — linking default no-op gameplay hooks")
endif()

# ── Windows app icon (compiled into the exe; GLFW reads the "GLFW_ICON" resource)
if(WIN32)
    list(APPEND ENGINE_SOURCES resources/nyx.rc)
endif()

# ── Create the executable ───────────────────────────────────────────────────
add_executable(${PROJECT_NAME} ${ENGINE_SOURCES})

# Gate the planet subsystem (see the NYX_HAS_PLANET detection above).
if(NYX_HAS_PLANET)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NYX_HAS_PLANET)
    message(STATUS "Nyx: planet subsystem ENABLED (project '${NYX_PROJECT}' provides procgen/Planet.h)")
else()
    message(STATUS "Nyx: planet subsystem DISABLED — no procgen/Planet.h in project '${NYX_PROJECT}'; engine builds planet-free")
endif()

# ── Include directories ─────────────────────────────────────────────────────
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${NYX_PROJECT_DIR}            # project scripts' headers: #include "procgen/Foo.h"
)

# ── Link libraries ──────────────────────────────────────────────────────────
target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    glfw
    spdlog::spdlog
    glm
    vma
    tinyobjloader
    stb
    cgltf
    assimp
    tree_sitter
)

# Win32 COM for the native folder picker (IFileOpenDialog) + DWM for the
# Windows 11 corner-preference attribute (sharp window corners).
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ole32 dwmapi)
endif()

# Build as a GUI (WINDOWS subsystem) app so launching the engine does NOT spawn a
# separate console window alongside it. We keep the standard int main() entry point
# by pointing the linker at the CRT's console-startup stub (mainCRTStartup), which
# calls main() for us. Logs still appear in the in-engine Output console (LogStore);
# they just no longer get their own OS console window.
if(WIN32 AND MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(${PROJECT_NAME} PRIVATE /ENTRY:mainCRTStartup)
endif()

# ── Vulkan validation layers: DEBUG ONLY ────────────────────────────────────
# Shipping builds must NOT request validation layers: they live in the Vulkan
# SDK, which players don't have installed, so requesting them throws the
# "enabledLayerCount" error (and can fail device creation outright). Debug keeps
# them on for development.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:ENABLE_VALIDATION_LAYERS>
    # glm::perspective/ortho default to OpenGL clip-space depth [-1,1]; Vulkan needs
    # [0,1]. Define this so every projection (camera, shadow, sky) matches what the
    # Vulkan rasterizer writes — required for shadow sampling depth-compare to align.
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

# ── Per-compiler warnings and platform definitions ──────────────────────────
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
    # NOMINMAX: stop <windows.h> defining min/max macros that clash with glm/std.
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Run from the project root under the Visual Studio / VS Code debugger so the
# relative shaders/ and assets/ paths resolve when you hit F5.
set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# ── Compile GLSL shaders to SPIR-V (.spv) ───────────────────────────────────
# The runtime loads shaders/<name>.spv via relative paths, so the .spv must be
# generated before running (they are intentionally not committed). glslc ships
# with the Vulkan SDK.
if(NOT Vulkan_GLSLC_EXECUTABLE)
    find_program(Vulkan_GLSLC_EXECUTABLE glslc)
endif()

if(Vulkan_GLSLC_EXECUTABLE)
    file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/shaders/*.vert
        ${CMAKE_SOURCE_DIR}/shaders/*.frag)

    set(SPV_FILES)
    foreach(SHADER ${SHADER_SOURCES})
        add_custom_command(
            OUTPUT  ${SHADER}.spv
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${SHADER} -o ${SHADER}.spv
            DEPENDS ${SHADER}
            COMMENT "Compiling shader ${SHADER}"
            VERBATIM)
        list(APPEND SPV_FILES ${SHADER}.spv)
    endforeach()

    add_custom_target(shaders DEPENDS ${SPV_FILES})
    add_dependencies(${PROJECT_NAME} shaders)
else()
    message(WARNING "glslc not found — shaders will NOT be auto-compiled. "
                    "Install the Vulkan SDK or precompile shaders/*.spv manually.")
endif()
