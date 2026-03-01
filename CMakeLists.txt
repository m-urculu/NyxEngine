cmake_minimum_required(VERSION 3.20)
project(VulkanEngine VERSION 0.1.0 LANGUAGES CXX)

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

# ── Collect all engine source files ─────────────────────────────────────────
set(ENGINE_SOURCES
    src/main.cpp
    src/Engine.cpp
    src/Logger.cpp
    src/Window.cpp
    src/renderer/VulkanContext.cpp
    src/renderer/Swapchain.cpp
    src/renderer/Pipeline.cpp
    src/renderer/Renderer.cpp
)

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
)

# ── Enable validation layers in Debug builds ────────────────────────────────
target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:ENABLE_VALIDATION_LAYERS>
    # Always enable for now (easier for learning)
    ENABLE_VALIDATION_LAYERS
)

# ── Compiler warnings (helps catch bugs early) ─────────────────────────────
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra -Wpedantic
)
