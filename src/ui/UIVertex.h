#pragma once

// UIVertex.h — 2D UI vertex format for the SDF-based UI renderer.
//   position : screen-space pixel position of this vertex
//   color    : RGBA tint
//   local    : pixel offset of this vertex from the shape's center (the value
//              interpolates to a per-pixel coordinate the fragment shader feeds
//              into the signed-distance functions)
//   data0/1  : per-shape SDF parameters (see ui.frag for the layout). data1.x
//              selects the shape: 0 solid fill, 1 filled rounded rect,
//              2 rounded-box outline, 3 capsule (round-capped line).

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>

namespace Nyx {

struct UIVertex {
    glm::vec2 position;
    glm::vec4 color;
    glm::vec2 local;
    glm::vec4 data0;
    glm::vec4 data1;

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription binding{};
        binding.binding   = 0;
        binding.stride    = sizeof(UIVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    static std::array<VkVertexInputAttributeDescription, 5> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 5> attrs{};

        attrs[0].binding  = 0;
        attrs[0].location = 0;
        attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[0].offset   = offsetof(UIVertex, position);

        attrs[1].binding  = 0;
        attrs[1].location = 1;
        attrs[1].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[1].offset   = offsetof(UIVertex, color);

        attrs[2].binding  = 0;
        attrs[2].location = 2;
        attrs[2].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].offset   = offsetof(UIVertex, local);

        attrs[3].binding  = 0;
        attrs[3].location = 3;
        attrs[3].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[3].offset   = offsetof(UIVertex, data0);

        attrs[4].binding  = 0;
        attrs[4].location = 4;
        attrs[4].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[4].offset   = offsetof(UIVertex, data1);

        return attrs;
    }
};

} // namespace Nyx
