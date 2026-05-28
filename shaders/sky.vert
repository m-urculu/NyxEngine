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

    // World-space ray from the camera through this corner of the far plane.
    mat4 invVP    = inverse(ubo.projection * ubo.view);
    vec4 worldFar = invVP * vec4(ndc, 1.0, 1.0);
    vDir          = (worldFar.xyz / worldFar.w) - ubo.cameraPosition.xyz;
}
