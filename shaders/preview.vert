#version 450

// Material-preview sphere: a single unit sphere at the origin, transformed by a
// fixed preview camera passed as a push constant (model = identity).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 baseColor;
    vec4 params;     // x = metallic, y = roughness
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    vNormal   = inNormal;
    vUV       = inUV;
    vWorldPos = inPos;     // model is identity
}
