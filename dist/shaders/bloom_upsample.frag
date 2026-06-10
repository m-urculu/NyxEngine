#version 450

// Bloom upsample with a 3×3 tent filter (Call of Duty presentation). The pipeline
// uses additive blend (ONE,ONE), so this fragment's output is added on top of
// whatever's already in the target mip — that's the previous downsample result,
// which is exactly what we want for the standard bloom mip-chain accumulation.

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform PC {
    float invTexelX;
    float invTexelY;
    float strength;    // scales bloom contribution (could grow per level later)
    float mode;        // unused here
} pc;

layout(location = 0) in  vec2 vUv;
layout(location = 0) out vec3 outColor;

void main() {
    vec2 t = vec2(pc.invTexelX, pc.invTexelY);
    // 3×3 tent (1 2 1 / 2 4 2 / 1 2 1) / 16
    vec3 col;
    col  = texture(src, vUv + t * vec2(-1.0, -1.0)).rgb;
    col += texture(src, vUv + t * vec2( 0.0, -1.0)).rgb * 2.0;
    col += texture(src, vUv + t * vec2( 1.0, -1.0)).rgb;
    col += texture(src, vUv + t * vec2(-1.0,  0.0)).rgb * 2.0;
    col += texture(src, vUv + t * vec2( 0.0,  0.0)).rgb * 4.0;
    col += texture(src, vUv + t * vec2( 1.0,  0.0)).rgb * 2.0;
    col += texture(src, vUv + t * vec2(-1.0,  1.0)).rgb;
    col += texture(src, vUv + t * vec2( 0.0,  1.0)).rgb * 2.0;
    col += texture(src, vUv + t * vec2( 1.0,  1.0)).rgb;
    col *= 1.0 / 16.0;

    outColor = col * pc.strength;
}
