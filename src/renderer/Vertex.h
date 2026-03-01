#pragma once

// Vertex.h — Vertex format definition
//
// Each vertex has: position (vec3), normal (vec3), color (vec3), texCoord (vec2)
// Total: 44 bytes per vertex.

#include <vulkan/vulkan.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <array>
#include <functional>

namespace Talos {

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
    glm::vec2 texCoord;

    bool operator==(const Vertex& other) const {
        return position == other.position &&
               normal   == other.normal &&
               color    == other.color &&
               texCoord == other.texCoord;
    }

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription binding{};
        binding.binding   = 0;
        binding.stride    = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    static std::array<VkVertexInputAttributeDescription, 4> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 4> attrs{};

        attrs[0].binding  = 0;
        attrs[0].location = 0;
        attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset   = offsetof(Vertex, position);

        attrs[1].binding  = 0;
        attrs[1].location = 1;
        attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset   = offsetof(Vertex, normal);

        attrs[2].binding  = 0;
        attrs[2].location = 2;
        attrs[2].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[2].offset   = offsetof(Vertex, color);

        attrs[3].binding  = 0;
        attrs[3].location = 3;
        attrs[3].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[3].offset   = offsetof(Vertex, texCoord);

        return attrs;
    }
};

} // namespace Talos

// Hash specialization for vertex deduplication
namespace std {
    template<> struct hash<Talos::Vertex> {
        size_t operator()(const Talos::Vertex& v) const {
            size_t h = 0;
            h ^= hash<glm::vec3>()(v.position);
            h ^= hash<glm::vec3>()(v.normal)    << 1;
            h ^= hash<glm::vec3>()(v.color)     << 2;
            h ^= hash<glm::vec2>()(v.texCoord)  << 3;
            return h;
        }
    };
}
