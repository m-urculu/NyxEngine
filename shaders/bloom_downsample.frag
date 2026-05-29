#version 450

// Bloom downsample. Two modes via push-constant.w:
//   mode = 0 → brightpass + downsample (from HDR scene into mip 0)
//   mode = 1 → plain downsample (mip N → mip N+1)
// Uses a 13-tap "box+13" filter (Call of Duty Advanced Warfare presentation) with
// a Karis-style average against fireflies on the first downsample only.

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform PC {
    float invTexelX;
    float invTexelY;
    float threshold;   // luminance below which contribution rolls off to 0
    float knee;        // soft-knee half-width around the threshold
    float strength;    // unused here; carried for parity with composite
    float mode;        // 0 = brightpass extract, 1 = plain downsample
} pc;

layout(location = 0) in  vec2 vUv;
layout(location = 0) out vec3 outColor;

// Luminance for the Karis weight (firefly suppression on the brightpass step).
float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float karisWeight(vec3 c) { return 1.0 / (1.0 + lum(c)); }

// Soft brightpass: smooth threshold with a small knee so values just above 1.0
// don't suddenly pop. Threshold 1.0 means we extract anything brighter than the
// LDR clamp would have allowed.
vec3 brightpass(vec3 c) {
    // Threshold + knee drive the soft-knee smoothstep: below threshold-knee the
    // contribution is 0, above threshold+knee it's full, smooth in between.
    // Values come from the EnvironmentComponent so the editor can tune them live.
    float l = lum(c);
    float t = clamp((l - pc.threshold + pc.knee) / max(pc.knee * 2.0, 1e-5), 0.0, 1.0);
    float w = t * t * (3.0 - 2.0 * t);
    return c * w;
}

void main() {
    vec2 t = vec2(pc.invTexelX, pc.invTexelY);

    // Centre + 4 inner-box + 8 outer-box samples (13 total, COD presentation).
    vec3 a = texture(src, vUv + t * vec2(-1.0, -1.0)).rgb;
    vec3 b = texture(src, vUv + t * vec2( 1.0, -1.0)).rgb;
    vec3 c = texture(src, vUv + t * vec2(-1.0,  1.0)).rgb;
    vec3 d = texture(src, vUv + t * vec2( 1.0,  1.0)).rgb;

    vec3 e = texture(src, vUv + t * vec2(-2.0,  0.0)).rgb;
    vec3 f = texture(src, vUv + t * vec2( 2.0,  0.0)).rgb;
    vec3 g = texture(src, vUv + t * vec2( 0.0, -2.0)).rgb;
    vec3 h = texture(src, vUv + t * vec2( 0.0,  2.0)).rgb;

    vec3 i = texture(src, vUv + t * vec2(-2.0, -2.0)).rgb;
    vec3 j = texture(src, vUv + t * vec2( 2.0, -2.0)).rgb;
    vec3 k = texture(src, vUv + t * vec2(-2.0,  2.0)).rgb;
    vec3 l = texture(src, vUv + t * vec2( 2.0,  2.0)).rgb;
    vec3 m = texture(src, vUv).rgb;

    // 4 inner-box samples each weighted 0.5; outer-box quartets each weighted 0.125.
    // Sum of weights = 1.0.
    vec3 result;
    if (pc.mode < 0.5) {
        // Brightpass + Karis average on the four inner quads (anti-firefly).
        vec3 inner = a + b + c + d;
        inner *= karisWeight(inner * 0.25);
        vec3 outer  = (i + e + g + m) * karisWeight((i + e + g + m) * 0.25);
        vec3 outer2 = (e + j + m + h) * karisWeight((e + j + m + h) * 0.25);
        vec3 outer3 = (g + m + k + f) * karisWeight((g + m + k + f) * 0.25);
        vec3 outer4 = (m + h + f + l) * karisWeight((m + h + f + l) * 0.25);
        result = inner * 0.5 + (outer + outer2 + outer3 + outer4) * 0.125;
        result = brightpass(result);
    } else {
        result = (a + b + c + d) * 0.125
               + (e + f + g + h) * 0.0625
               + (i + j + k + l) * 0.03125
               + m * 0.125;
    }

    outColor = result;
}
