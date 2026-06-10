#version 450

// Textured-quad vertex shader for the asset preview (flat image + shaded ball).
layout(location = 0) in vec2 inPos;   // pixel coordinates
layout(location = 1) in vec2 inUV;

layout(push_constant) uniform PushConstants {
    vec2  screenSize;
    vec2  ballCenter;   // pixels (mode 1)
    float ballRadius;   // pixels (mode 1)
    int   mode;         // 0 = flat image, 1 = shaded textured ball
} pc;

layout(location = 0) out vec2 fragUV;

void main() {
    vec2 ndc = (inPos / pc.screenSize) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragUV = inUV;
}
