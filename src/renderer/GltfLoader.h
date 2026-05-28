#pragma once

// GltfLoader.h — Load glTF 2.0 models (positions, normals, texcoords, indices)
//
// Uses cgltf (header-only, C99) for parsing.
// Extracts mesh geometry and base color texture URI.

#include "renderer/Vertex.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <string>

namespace Nyx {

struct GltfMeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::string           baseColorTextureURI;         // empty if none (sRGB)
    std::string           normalTextureURI;            // tangent-space normal map (linear)
    std::string           metalRoughTextureURI;        // G=roughness, B=metallic (linear)
    std::string           occlusionTextureURI;         // R=ambient occlusion (linear)

    // PBR material params
    glm::vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor  = 0.0f;
    float roughnessFactor = 0.5f;
    float alphaCutoff     = 0.0f;  // >0 = alpha-masked (cutout); 0 = opaque
};

// A node in the glTF hierarchy → becomes one entity. `firstPrim`/`primCount` index
// into the flat primitive list (matches GltfLoader::load order so mesh sources stay
// resolvable). parent = index into the node list, or -1 for a root.
struct GltfNode {
    std::string name;
    glm::vec3   translation{0.0f};
    glm::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3   scale{1.0f};
    int         parent    = -1;
    int         firstPrim = -1;
    int         primCount = 0;
    int         skin      = -1;   // index into GltfImport::skins, or -1 (not skinned)
};

// A skin = ordered joint nodes + their inverse-bind matrices (for GPU skinning).
struct GltfSkin {
    std::vector<int>       jointNodes;   // node indices of the joints, in joint order
    std::vector<glm::mat4> inverseBind;  // inverse bind matrix per joint
};

// One keyframe track: animates a node's translation/rotation/scale over time.
struct GltfAnimChannel {
    int  targetNode = -1;
    int  path  = 0;   // 0 = translation, 1 = rotation, 2 = scale
    int  interp = 0;  // 0 = linear, 1 = step
    std::vector<float>     times;   // keyframe times (seconds)
    std::vector<glm::vec4> values;  // T/S use xyz; R uses xyzw quaternion
};

struct GltfAnim {
    std::string                   name;
    float                         duration = 0.0f;
    std::vector<GltfAnimChannel>  channels;
};

struct GltfImport {
    std::vector<GltfMeshData> primitives;  // flat list (same order as load())
    std::vector<GltfNode>     nodes;
    std::vector<GltfAnim>     animations;
    std::vector<GltfSkin>     skins;
};

class GltfLoader {
public:
    // Load all mesh primitives (flat). Used to rebuild geometry by index.
    static std::vector<GltfMeshData> load(const std::string& filepath);

    // Full import: primitives + node hierarchy + animation clips.
    static GltfImport loadImport(const std::string& filepath);
};

} // namespace Nyx
