#version 450

// Procedural skybox vertex shader — emits a fullscreen triangle from gl_VertexIndex
// (no vertex buffer), and reconstructs the world-space view direction per corner so
// the fragment shader can sample the sky gradient. Drawn at depth=1 (far plane) with
// LESS_EQUAL depth test so opaque meshes always overdraw it.

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

layout(location = 0) out vec3 vDir;

void main() {
    // Classic fullscreen-triangle trick: 3 verts at (-1,-1), (3,-1), (-1,3) → covers
    // the entire screen with a single triangle (avoids the diagonal seam of a quad).
    vec2 ndc = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                    (gl_VertexIndex == 2) ? 3.0 : -1.0);
    gl_Position = vec4(ndc, 1.0, 1.0);                 // depth = 1 (far plane)

    // World-space view ray, reconstructed in a numerically stable way: unproject
    // to view space with the camera-INDEPENDENT inverse projection, then rotate to
    // world with the view's rotation only. Using inverse(projection*view) and a
    // far-plane point instead makes the ray jitter/flash as the camera moves once
    // the far/near ratio is large (the far plane is 10000) — the full inverse is
    // recomputed per frame and loses precision at z=1. Here inverse(projection) is
    // constant and mat3(inverse(view)) is an exact rotation, so the ray is stable.
    vec4 viewH   = inverse(ubo.projection) * vec4(ndc, 1.0, 1.0);
    vec3 viewDir = viewH.xyz / viewH.w;                // far-plane point in view space = dir from camera
    vDir         = mat3(inverse(ubo.view)) * viewDir;  // rotate into world (camera translation irrelevant for an infinite sky)
}
