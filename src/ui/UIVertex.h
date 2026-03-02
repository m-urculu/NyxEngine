#pragma once

// UIVertex.h — 2D UI vertex format: position (vec2) + color (vec4) = 24 bytes

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>

namespace Talos {

struct UIVertex {
    glm::vec2 position;
    glm::vec4 color;

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription binding{};
        binding.binding   = 0;
        binding.stride    = sizeof(UIVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    static std::array<VkVertexInputAttributeDescription, 2> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 2> attrs{};

        attrs[0].binding  = 0;
        attrs[0].location = 0;
        attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[0].offset   = offsetof(UIVertex, position);

        attrs[1].binding  = 0;
        attrs[1].location = 1;
        attrs[1].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[1].offset   = offsetof(UIVertex, color);

        return attrs;
    }
};

} // namespace Talos
