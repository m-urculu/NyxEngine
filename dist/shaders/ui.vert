#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inLocal;
layout(location = 3) in vec4 inData0;
layout(location = 4) in vec4 inData1;

layout(push_constant) uniform PushConstants {
    vec2 screenSize;
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragLocal;
layout(location = 2) flat out vec4 fragData0;
layout(location = 3) flat out vec4 fragData1;

void main() {
    // Convert pixel coordinates to NDC [-1, 1]
    vec2 ndc = (inPosition / pc.screenSize) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);

    fragColor = inColor;
    fragLocal = inLocal;   // interpolates to a per-pixel offset from shape center
    fragData0 = inData0;   // flat: identical across the shape's 4 vertices
    fragData1 = inData1;
}
