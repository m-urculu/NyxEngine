#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;

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

layout(location = 0) out vec4 outColor;

void main() {
    vec3 normal   = normalize(fragNormal);
    vec3 lightDir = normalize(-ubo.lightDirection.xyz);
    vec3 viewDir  = normalize(ubo.cameraPosition.xyz - fragWorldPos);

    // Ambient
    vec3 ambient = ubo.ambientColor.xyz * ubo.ambientColor.w;

    // Diffuse (Lambertian)
    float diff    = max(dot(normal, lightDir), 0.0);
    vec3  diffuse = diff * ubo.lightColor.xyz * ubo.lightColor.w;

    // Specular (Blinn-Phong)
    vec3  halfDir  = normalize(lightDir + viewDir);
    float spec     = pow(max(dot(normal, halfDir), 0.0), 32.0);
    vec3  specular = spec * ubo.lightColor.xyz * ubo.lightColor.w;

    vec3 result = (ambient + diffuse + specular) * fragColor;
    outColor = vec4(result, 1.0);
}
