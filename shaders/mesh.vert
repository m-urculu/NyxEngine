#version 450

// Vertex inputs
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

// Light data struct (must match C++ GpuLightData, 48 bytes)
struct GpuLightData {
    vec4 positionAndType;
    vec4 colorAndIntensity;
    vec4 params;
};

// Global UBO — layout must match C++ UniformBufferObject exactly
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

// Per-object push constants
layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 normalMatrix;
} pc;

// Outputs to fragment shader
layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec2 fragTexCoord;
layout(location = 4) out vec3 fragDiffIBL;     // per-vertex sky irradiance, interpolated

// Cheap 3-color weighted blend — diffuse IBL is low-frequency so vertex interpolation
// is plenty. Sampling once per vertex (not per pixel) is one of the biggest wins.
vec3 sampleSky(vec3 dir) {
    float u = max( dir.y, 0.0);
    float d = max(-dir.y, 0.0);
    vec3 col = ubo.skyHorizon.rgb * (1.0 - u - d)
             + ubo.skyTop.rgb     * u
             + ubo.skyGround.rgb  * d;
    return col * ubo.skyHorizon.w;
}

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position   = ubo.projection * ubo.view * worldPos;

    fragColor    = inColor;
    vec3 worldN  = mat3(pc.normalMatrix) * inNormal;
    fragNormal   = worldN;
    fragWorldPos = worldPos.xyz;
    fragTexCoord = inTexCoord;
    fragDiffIBL  = sampleSky(normalize(worldN));
}
