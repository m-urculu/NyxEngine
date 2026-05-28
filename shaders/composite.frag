#version 450

// Composite fragment shader — reads the HDR scene (and later bloom), applies the
// ACES tonemap, and writes LDR sRGB to the swapchain. This is where tonemapping
// lives now; mesh.frag and sky.frag stopped applying it inline so that bright
// values survive into the bloom chain in their full HDR range.

layout(set = 0, binding = 0) uniform sampler2D hdrScene;
layout(set = 0, binding = 1) uniform sampler2D bloom;       // mip 0 of the bloom chain

layout(location = 0) in  vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 acesTonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr   = texture(hdrScene, vUv).rgb;
    vec3 bloom = texture(bloom,    vUv).rgb;
    // Bloom contribution. The mip-chain upsample already summed N levels of energy,
    // so this stays small. Typical filmic strength is 0.02-0.05.
    // Bloom contribution. With the brightpass threshold lowered to 0.6 (analytic-sky
    // IBL stays in 0.5–1.5 range), strength sits at 0.08 so the glow reads clearly
    // around bright sky areas and metal highlights. Dial down to 0.03 for a subtle
    // filmic look, up to 0.15 for a stylized cinematic glow.
    const float BLOOM_STRENGTH = 0.08;
    vec3 combined = hdr + bloom * BLOOM_STRENGTH;
    outColor = vec4(acesTonemap(combined), 1.0);
}
