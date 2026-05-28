#pragma once

// LightComponent.h — Per-entity light source (directional or point)

#include <glm/glm.hpp>
#include <cstdint>

namespace Nyx {

struct LightComponent {
    enum class Type : uint32_t { Directional = 0, Point = 1 };
    Type type = Type::Directional;
    glm::vec3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    glm::vec3 direction = {0.0f, -1.0f, 0.0f}; // for directional
    float radius = 10.0f; // attenuation range for point lights
};

} // namespace Nyx
