#pragma once

// SkinComponent.h — Skeletal-skinning data for a skinned mesh entity. The joints
// are other entities (the skeleton nodes, animated by the normal node-animation
// system); each frame the Engine computes jointMatrix[i] = jointWorld * inverseBind
// and uploads it to jointBuffer, which the skinned pipeline binds at set 2.

#include "ecs/Entity.h"
#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>

namespace Nyx {

struct SkinComponent {
    std::vector<Entity>    joints;       // joint node entities (in joint order)
    std::vector<glm::mat4> inverseBind;  // inverse-bind matrix per joint
    VkDescriptorSet        jointSet = VK_NULL_HANDLE;  // set 2 (joint matrices UBO)
    Buffer*                jointBuffer = nullptr;      // owned by Descriptors (stable address)
};

} // namespace Nyx
