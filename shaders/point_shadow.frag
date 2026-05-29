#version 450

// Point-light shadow fragment shader. Writes linear distance from the light to
// the fragment normalised by the light's far radius — sampled later in mesh.frag
// to compare against the actual fragment-to-light distance. Single-channel R32F
// colour attachment (no hardware shadow-compare; we do it manually in mesh.frag).

layout(location = 0) in  vec3  vWorldPos;
layout(location = 0) out float outDistance;

layout(push_constant) uniform PC {
    mat4  viewProj;
    mat4  model;
    vec4  lightPosAndFar;   // xyz = light world pos, w = far radius
} pc;

void main() {
    float dist = length(vWorldPos - pc.lightPosAndFar.xyz);
    outDistance = dist / max(pc.lightPosAndFar.w, 1e-4);
}
