#version 450

// Point-light shadow vertex shader. Renders the scene from one of the six faces
// of a cube map centred on the light. The face's view-projection matrix and the
// model matrix come in through push constants; world-space position passes to
// the fragment shader so it can write linear distance from the light.

layout(location = 0) in vec3 inPosition;
// glTF skin / IBL attributes ignored here — point-light shadows only need
// positions, but the vertex format is shared with the main pipeline. The other
// attributes must still be declared so layout indices line up.
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4  inWeights;

layout(push_constant) uniform PC {
    mat4 viewProj;        // light's per-face view * projection
    mat4 model;           // world matrix of the mesh being drawn
    vec4 lightPosAndFar;  // fragment shader uses this; declared here for layout parity
} pc;

layout(location = 0) out vec3 vWorldPos;

void main() {
    vec4 wp   = pc.model * vec4(inPosition, 1.0);
    vWorldPos = wp.xyz;
    gl_Position = pc.viewProj * wp;
}
