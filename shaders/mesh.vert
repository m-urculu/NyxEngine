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

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position   = ubo.projection * ubo.view * worldPos;

    fragColor    = inColor;
    fragNormal   = mat3(pc.normalMatrix) * inNormal;
    fragWorldPos = worldPos.xyz;
    fragTexCoord = inTexCoord;
}
