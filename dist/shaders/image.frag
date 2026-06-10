#version 450

// Asset preview fragment shader. mode 0: flat image (sample by UV). mode 1: map
// the texture onto a lit sphere (a material "ball"), discarding outside the disc.
// The texture is VK_FORMAT_..._SRGB, so sampling already yields linear color and
// the sRGB swapchain re-encodes on write — no manual gamma here.

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform PushConstants {
    vec2  screenSize;
    vec2  ballCenter;
    float ballRadius;
    int   mode;
} pc;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    if (pc.mode == 1) {
        vec2 p = (gl_FragCoord.xy - pc.ballCenter) / pc.ballRadius;  // [-1,1] in the disc
        float r2 = dot(p, p);
        if (r2 > 1.0) discard;
        float nz = sqrt(max(1.0 - r2, 0.0));
        vec3  N  = vec3(p, nz);
        vec3  c  = texture(tex, p * 0.5 + 0.5).rgb;           // planar map on the front hemisphere
        vec3  L  = normalize(vec3(-0.5, -0.6, 0.85));         // light from upper-left (matches UI ball)
        float d  = max(dot(N, L), 0.0);
        outColor = vec4(c * (0.32 + 0.78 * d) + vec3(0.25) * pow(d, 16.0), 1.0);
    } else {
        outColor = texture(tex, fragUV);
    }
}
