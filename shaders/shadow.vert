#version 450

// Shadow map vertex shader — transforms world-space mesh vertices into the sun's
// light-space clip volume. The light-space matrix lives in the global UBO; model
// comes from push constants. No varying outputs (depth-only render).

layout(location = 0) in vec3 inPosition;
// Locations 1-3 (normal/color/texcoord) ignored by this shader; the vertex layout
// still feeds them — Vulkan just doesn't bind the unused locations.

struct GpuLightData {
    vec4 positionAndType;
    vec4 colorAndIntensity;
    vec4 params;
};

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 projection;
    vec4 ambientColor;
    vec4 cameraPosition;
    GpuLightData lights[8];
    ivec4 lightCountAndPad;
    vec4 skyTop;
    vec4 skyHorizon;
    vec4 skyGround;
    mat4 lightSpace;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 normalMatrix;   // unused
} pc;

void main() {
    gl_Position = ubo.lightSpace * pc.model * vec4(inPosition, 1.0);
}
