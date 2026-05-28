#pragma once

// TransformComponent.h — Position, rotation, scale, parent, world matrix

#include "ecs/Entity.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Nyx {

struct TransformComponent {
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity
    glm::vec3 scale    = {1.0f, 1.0f, 1.0f};

    Entity parent = NULL_ENTITY;

    // Computed by TransformSystem
    glm::mat4 worldMatrix = glm::mat4(1.0f);
};

} // namespace Nyx
