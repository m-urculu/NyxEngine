#pragma once

// Vertex.h — Vertex format definition
//
// position (vec3), normal (vec3), color (vec3), texCoord (vec2), plus skinning:
// joints (uvec4 = up to 4 bone indices) + weights (vec4). joints/weights default
// to 0 for non-skinned meshes (which use the non-skinned pipeline that ignores them).

#include <vulkan/vulkan.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <array>
#include <functional>

namespace Nyx {

struct Vertex {
    glm::vec3  position;
    glm::vec3  normal;
    glm::vec3  color;
    glm::vec2  texCoord;
    glm::uvec4 joints{0, 0, 0, 0};        // bone indices (skinning)
    glm::vec4  weights{0.0f, 0.0f, 0.0f, 0.0f};

    bool operator==(const Vertex& other) const {
        return position == other.position &&
               normal   == other.normal &&
               color    == other.color &&
               texCoord == other.texCoord &&
               joints   == other.joints &&
               weights  == other.weights;
    }

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription binding{};
        binding.binding   = 0;
        binding.stride    = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    static std::array<VkVertexInputAttributeDescription, 6> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 6> attrs{};

        attrs[0].binding = 0; attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[0].offset = offsetof(Vertex, position);
        attrs[1].binding = 0; attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[1].offset = offsetof(Vertex, normal);
        attrs[2].binding = 0; attrs[2].location = 2; attrs[2].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[2].offset = offsetof(Vertex, color);
        attrs[3].binding = 0; attrs[3].location = 3; attrs[3].format = VK_FORMAT_R32G32_SFLOAT;       attrs[3].offset = offsetof(Vertex, texCoord);
        attrs[4].binding = 0; attrs[4].location = 4; attrs[4].format = VK_FORMAT_R32G32B32A32_UINT;   attrs[4].offset = offsetof(Vertex, joints);
        attrs[5].binding = 0; attrs[5].location = 5; attrs[5].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[5].offset = offsetof(Vertex, weights);

        return attrs;
    }
};

} // namespace Nyx

// Hash specialization for vertex deduplication
namespace std {
    template<> struct hash<Nyx::Vertex> {
        size_t operator()(const Nyx::Vertex& v) const {
            size_t h = 0;
            h ^= hash<glm::vec3>()(v.position);
            h ^= hash<glm::vec3>()(v.normal)    << 1;
            h ^= hash<glm::vec3>()(v.color)     << 2;
            h ^= hash<glm::vec2>()(v.texCoord)  << 3;
            return h;
        }
    };
}
