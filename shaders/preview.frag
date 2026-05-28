#version 450

// Material-preview lighting — mirrors the engine's Blinn-Phong + metallic/rough
// model (mesh.frag) but with two fixed studio lights baked in. Albedo texture is
// SRGB (auto-linearized on sample); output linear for the sRGB swapchain.

layout(set = 0, binding = 0) uniform sampler2D albedoTex;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 baseColor;
    vec4 params;     // x = metallic, y = roughness
} pc;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;
layout(location = 0) out vec4 outColor;

void addLight(vec3 N, vec3 V, vec3 albedo, float metallic, float shininess,
              vec3 L, vec3 lightColor, inout vec3 diffuse, inout vec3 spec) {
    L = normalize(L);
    float d = max(dot(N, L), 0.0);
    diffuse += d * lightColor * (1.0 - metallic);
    vec3 H = normalize(L + V);
    float s = pow(max(dot(N, H), 0.0), shininess);
    spec += s * lightColor * mix(vec3(0.04), albedo, metallic);
}

void main() {
    vec3 N = normalize(vNormal);
    vec3 camPos = vec3(0.0, 0.0, 3.0);
    vec3 V = normalize(camPos - vWorldPos);

    vec3  albedo    = texture(albedoTex, vUV).rgb * pc.baseColor.rgb;
    float metallic  = pc.params.x;
    float roughness = pc.params.y;
    float shininess = mix(8.0, 128.0, 1.0 - roughness);

    vec3 ambient = vec3(0.17);
    vec3 diffuse = vec3(0.0);
    vec3 spec    = vec3(0.0);
    addLight(N, V, albedo, metallic, shininess, vec3( 0.6, 0.7, 0.6), vec3(1.00, 0.98, 0.92), diffuse, spec);  // key
    addLight(N, V, albedo, metallic, shininess, vec3(-0.7, 0.2, 0.5), vec3(0.30, 0.34, 0.46), diffuse, spec);  // cool fill

    vec3 result = albedo * (ambient + diffuse) + spec;
    outColor = vec4(result, 1.0);
}
