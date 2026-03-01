#pragma once

// Vertex.h — Vertex format definition
//
// Each vertex has: position (vec3), normal (vec3), color (vec3)
// Total: 36 bytes per vertex.

#include <vulkan/vulkan.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <array>
#include <functional>

namespace VulkanEngine {

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;

    bool operator==(const Vertex& other) const {
        return position == other.position &&
               normal   == other.normal &&
               color    == other.color;
    }

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription binding{};
        binding.binding   = 0;
        binding.stride    = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attrs{};

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

        return attrs;
    }
};

} // namespace VulkanEngine

// Hash specialization for vertex deduplication
namespace std {
    template<> struct hash<VulkanEngine::Vertex> {
        size_t operator()(const VulkanEngine::Vertex& v) const {
            size_t h = 0;
            h ^= hash<glm::vec3>()(v.position);
            h ^= hash<glm::vec3>()(v.normal)   << 1;
            h ^= hash<glm::vec3>()(v.color)    << 2;
            return h;
        }
    };
}
