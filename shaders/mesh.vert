#version 450

// Vertex inputs
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

// UBO
layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 projection;
    mat4 normalMatrix;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 cameraPosition;
} ubo;

// Outputs to fragment shader
layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;

void main() {
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    gl_Position   = ubo.projection * ubo.view * worldPos;

    fragColor    = inColor;
    fragNormal   = mat3(ubo.normalMatrix) * inNormal;
    fragWorldPos = worldPos.xyz;
}
