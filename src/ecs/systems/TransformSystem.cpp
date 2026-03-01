#include "ecs/systems/TransformSystem.h"
#include "ecs/Registry.h"
#include "ecs/components/TransformComponent.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Talos {

static glm::mat4 computeLocalMatrix(const TransformComponent& t) {
    glm::mat4 T = glm::translate(glm::mat4(1.0f), t.position);
    glm::mat4 R = glm::toMat4(t.rotation);
    glm::mat4 S = glm::scale(glm::mat4(1.0f), t.scale);
    return T * R * S;
}

void TransformSystem::update(Registry& registry) {
    auto& pool = registry.pool<TransformComponent>();

    // First pass: compute local matrices for all entities
    for (size_t i = 0; i < pool.size(); i++) {
        pool[i].worldMatrix = computeLocalMatrix(pool[i]);
    }

    // Second pass: apply parent transforms
    // Simple approach: iterate until no more updates needed (handles deep hierarchies)
    // For the typical 1-2 level hierarchies this converges in 1-2 passes.
    bool changed = true;
    int maxIterations = 8; // prevent infinite loops
    while (changed && maxIterations-- > 0) {
        changed = false;
        for (size_t i = 0; i < pool.size(); i++) {
            TransformComponent& tc = pool[i];
            if (tc.parent != NULL_ENTITY && pool.has(tc.parent)) {
                glm::mat4 parentWorld = pool.get(tc.parent).worldMatrix;
                glm::mat4 expected = parentWorld * computeLocalMatrix(tc);
                if (tc.worldMatrix != expected) {
                    tc.worldMatrix = expected;
                    changed = true;
                }
            }
        }
    }
}

} // namespace Talos
