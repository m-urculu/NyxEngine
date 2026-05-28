#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragLocal;          // pixel offset from shape center
layout(location = 2) flat in vec4 fragData0;
layout(location = 3) flat in vec4 fragData1;

layout(location = 0) out vec4 outColor;

// Signed distance to a rounded box centered at the origin.
//   b = half-extents, r = corner radius
float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

// Signed distance to the segment a->b (distance to the line's spine).
float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

// UI colors are authored in sRGB; the swapchain is sRGB so the GPU gamma-encodes
// our output. Convert to linear here so the constants mean their on-screen value.
vec3 srgbToLinear(vec3 c) {
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 lower   = c / 12.92;
    vec3 higher  = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(higher, lower, vec3(cutoff));
}

void main() {
    vec3 rgb = srgbToLinear(fragColor.rgb);
    int shape = int(fragData1.x + 0.5);

    float coverage = 1.0;
    if (shape == 1 || shape == 2 || shape == 3) {
        float d;
        if (shape == 1) {
            // Filled rounded rect: half-extents = data0.xy, corner radius = data0.z
            d = sdRoundBox(fragLocal, fragData0.xy, fragData0.z);
        } else if (shape == 2) {
            // Rounded-box outline: extents/radius in data0.xyz, half stroke = data0.w
            d = abs(sdRoundBox(fragLocal, fragData0.xy, fragData0.z)) - fragData0.w;
        } else {
            // Capsule (round-capped line): endpoints in data0.xy/.zw, radius = data1.y
            d = sdSegment(fragLocal, fragData0.xy, fragData0.zw) - fragData1.y;
        }
        // 1px-wide analytic anti-aliasing centered on the edge (d == 0).
        coverage = clamp(0.5 - d / max(fwidth(d), 1e-5), 0.0, 1.0);
    } else if (shape == 4) {
        // Shaded sphere ("ball"): data0.x = radius. Build a hemisphere normal
        // from the in-circle position and light it for a 3D glossy look.
        float radius = fragData0.x;
        float dist   = length(fragLocal);
        coverage = clamp(0.5 - (dist - radius) / max(fwidth(dist), 1e-5), 0.0, 1.0);

        vec2  nxy = fragLocal / max(radius, 1e-5);
        float nz  = sqrt(max(1.0 - dot(nxy, nxy), 0.0));
        vec3  N   = vec3(nxy, nz);
        vec3  L   = normalize(vec3(-0.5, -0.6, 0.85)); // light from upper-left, toward viewer
        float diff = max(dot(N, L), 0.0);
        float hi   = pow(diff, 16.0);                  // tight specular highlight
        rgb = rgb * (0.30 + 0.85 * diff) + vec3(0.55) * hi;
    }

    outColor = vec4(rgb, fragColor.a * coverage);
}
