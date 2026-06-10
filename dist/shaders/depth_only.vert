#version 450

// Depth-only vertex shader for the Z pre-pass — writes ONLY gl_Position, skipping the
// IBL sampleSky, normalMatrix, and varying outputs that the full mesh.vert computes.
// The pre-pass establishes opaque depth so the main pass shades each pixel only once
// with depth-equal testing (no overdraw cost on heavy regions like the head/helmet).

layout(location = 0) in vec3 inPosition;
// Vertex layout still feeds normal/color/texcoord at locations 1-3, but we ignore them.

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 projection;
    // Remaining UBO fields exist in the buffer but we don't access them here.
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 normalMatrix;   // unused, kept so layout matches the main mesh pipeline
} pc;

void main() {
    gl_Position = ubo.projection * ubo.view * (pc.model * vec4(inPosition, 1.0));
}
