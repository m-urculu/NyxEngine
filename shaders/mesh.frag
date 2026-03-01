#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec2 fragTexCoord;

// Light data struct (must match C++ GpuLightData)
struct GpuLightData {
    vec4 positionAndType;
    vec4 colorAndIntensity;
    vec4 params;
};

// Global UBO — layout must match vertex shader and C++ exactly
layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 projection;
    vec4 ambientColor;
    vec4 cameraPosition;
    GpuLightData lights[8];
    ivec4 lightCountAndPad;
} ubo;

// Per-material texture (set 1, binding 0)
layout(set = 1, binding = 0) uniform sampler2D texSampler;

// Per-material params (set 1, binding 1)
layout(set = 1, binding = 1) uniform MaterialUBO {
    vec4 baseColorFactor;
    float metallic;
    float roughness;
} material;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 normal  = normalize(fragNormal);
    vec3 viewDir = normalize(ubo.cameraPosition.xyz - fragWorldPos);

    // Sample texture, modulate with vertex color and material base color
    vec4 texColor = texture(texSampler, fragTexCoord);
    vec3 albedo = texColor.rgb * fragColor * material.baseColorFactor.rgb;

    // Material-derived shading params
    float shininess = mix(8.0, 128.0, 1.0 - material.roughness);

    // Ambient
    vec3 ambient = ubo.ambientColor.xyz * ubo.ambientColor.w;

    // Accumulate lighting from all active lights
    vec3 diffuseSum  = vec3(0.0);
    vec3 specularSum = vec3(0.0);

    int lightCount = ubo.lightCountAndPad.x;
    for (int i = 0; i < lightCount; i++) {
        GpuLightData light = ubo.lights[i];

        vec3 lightColor     = light.colorAndIntensity.xyz * light.colorAndIntensity.w;
        float lightType     = light.positionAndType.w;
        float attenuation   = 1.0;
        vec3  lightDir;

        if (lightType < 0.5) {
            // Directional light: positionAndType.xyz is direction
            lightDir = normalize(-light.positionAndType.xyz);
        } else {
            // Point light: positionAndType.xyz is position
            vec3 toLight = light.positionAndType.xyz - fragWorldPos;
            float dist   = length(toLight);
            lightDir     = toLight / max(dist, 0.0001);
            float radius = light.params.x;
            attenuation  = 1.0 / (1.0 + (dist * dist) / (radius * radius));
        }

        // Diffuse (Lambertian), scaled by (1 - metallic)
        float diff = max(dot(normal, lightDir), 0.0);
        diffuseSum += diff * lightColor * attenuation * (1.0 - material.metallic);

        // Specular (Blinn-Phong), scaled by metallic
        vec3 halfDir = normalize(lightDir + viewDir);
        float spec   = pow(max(dot(normal, halfDir), 0.0), shininess);
        // Metallic surfaces tint specular with albedo, dielectrics use white
        vec3 specColor = mix(vec3(0.04), albedo, material.metallic);
        specularSum += spec * lightColor * attenuation * specColor;
    }

    vec3 result = albedo * (ambient + diffuseSum) + specularSum;
    outColor = vec4(result, texColor.a * material.baseColorFactor.a);
}
