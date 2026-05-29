#version 450

// Composite fragment shader — reads the HDR scene (and later bloom), applies the
// configured tonemap, and writes LDR sRGB to the swapchain. Tonemap mode, bloom
// strength, and pre-tonemap exposure all come from the EnvironmentComponent via
// push constants so the editor can drive them live.

layout(set = 0, binding = 0) uniform sampler2D hdrScene;
layout(set = 0, binding = 1) uniform sampler2D bloom;       // mip 0 of the bloom chain

layout(push_constant) uniform PC {
    float bloomStrength;   // 0..1; 0 disables bloom
    float exposure;        // EV stops; final = 2^exposure
    float tonemap;         // 0 = ACES, 1 = Reinhard, 2 = None (clamp)
    float _pad;
} pc;

layout(location = 0) in  vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 acesTonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr   = texture(hdrScene, vUv).rgb;
    vec3 bloomC = texture(bloom,   vUv).rgb;
    vec3 combined = (hdr + bloomC * pc.bloomStrength) * exp2(pc.exposure);

    vec3 ldr;
    if (pc.tonemap < 0.5) {
        ldr = acesTonemap(combined);
    } else if (pc.tonemap < 1.5) {
        ldr = combined / (combined + vec3(1.0));                 // Reinhard
    } else {
        ldr = clamp(combined, 0.0, 1.0);                         // raw clamp
    }
    outColor = vec4(ldr, 1.0);
}
