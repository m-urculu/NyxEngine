#version 450

// Procedural skybox fragment shader — samples the same analytic 3-stop gradient that
// drives in-shader IBL in mesh.frag, so what metallics reflect matches the background.

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

layout(location = 0) in  vec3 vDir;
layout(location = 0) out vec4 outColor;

vec3 sampleSky(vec3 dir) {
    float y = clamp(dir.y, -1.0, 1.0);
    float horizonBand = 1.0 - smoothstep(0.0, 0.25, abs(y));
    vec3 base;
    if (y >= 0.0) base = mix(ubo.skyHorizon.rgb, ubo.skyTop.rgb,    smoothstep(0.0, 0.5,  y));
    else          base = mix(ubo.skyHorizon.rgb, ubo.skyGround.rgb, smoothstep(0.0, 0.5, -y));
    vec3 col = mix(base, ubo.skyHorizon.rgb, horizonBand * 0.5);
    return col * ubo.skyHorizon.w;
}

void main() {
    // Output linear HDR — composite owns tonemapping (so bloom can extract bright
    // sky values before dynamic range gets compressed).
    outColor = vec4(sampleSky(normalize(vDir)), 1.0);
}
