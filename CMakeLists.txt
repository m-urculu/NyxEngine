cmake_minimum_required(VERSION 3.21)
project(Nyx VERSION 0.3.0 LANGUAGES CXX C)   # C enabled for the tree-sitter runtime/grammar

# ── C++17 standard ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# ── Windows app icon (compiled into the exe; GLFW reads the "GLFW_ICON" resource)
if(WIN32)
    list(APPEND ENGINE_SOURCES resources/nyx.rc)
endif()

# ── Create the executable ───────────────────────────────────────────────────
add_executable(${PROJECT_NAME} ${ENGINE_SOURCES})

# ── Include directories ─────────────────────────────────────────────────────
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
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

# ── Enable validation layers in Debug builds ────────────────────────────────
target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:ENABLE_VALIDATION_LAYERS>
    # Always enable for now (easier for learning)
    ENABLE_VALIDATION_LAYERS
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
