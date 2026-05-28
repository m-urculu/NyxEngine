#version 450

// Skinned mesh vertex shader — GPU skinning. Per glTF, the skinned mesh's own
// node transform is ignored; the joint matrices (jointWorld * inverseBind) place
// the vertex in world space directly.

layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec3  inColor;
layout(location = 3) in vec2  inTexCoord;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4  inWeights;

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

layout(push_constant) uniform PushConstants {
    mat4 model;          // ignored for skinned meshes (kept for layout compatibility)
    mat4 normalMatrix;
} pc;

#define MAX_JOINTS 128
layout(set = 2, binding = 0) uniform JointBuffer {
    mat4 jointMatrices[MAX_JOINTS];
} skin;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec2 fragTexCoord;
layout(location = 4) out vec3 fragDiffIBL;     // per-vertex sky irradiance

vec3 sampleSky(vec3 dir) {
    float u = max( dir.y, 0.0);
    float d = max(-dir.y, 0.0);
    vec3 col = ubo.skyHorizon.rgb * (1.0 - u - d)
             + ubo.skyTop.rgb     * u
             + ubo.skyGround.rgb  * d;
    return col * ubo.skyHorizon.w;
}

void main() {
    mat4 skinMat =
        inWeights.x * skin.jointMatrices[inJoints.x] +
        inWeights.y * skin.jointMatrices[inJoints.y] +
        inWeights.z * skin.jointMatrices[inJoints.z] +
        inWeights.w * skin.jointMatrices[inJoints.w];
    if (inWeights.x + inWeights.y + inWeights.z + inWeights.w < 0.0001)
        skinMat = mat4(1.0);   // unweighted vertex → leave in place

    vec4 worldPos = skinMat * vec4(inPosition, 1.0);
    gl_Position   = ubo.projection * ubo.view * worldPos;

    fragColor    = inColor;
    vec3 worldN  = mat3(skinMat) * inNormal;
    fragNormal   = worldN;
    fragWorldPos = worldPos.xyz;
    fragTexCoord = inTexCoord;
    fragDiffIBL  = sampleSky(normalize(worldN));
}
