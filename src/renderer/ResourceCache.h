#pragma once

// ResourceCache.h — Deduplicates meshes and textures by string key
//
// Owns unique_ptr<Mesh> and unique_ptr<Texture>. Provides getOrCreate* methods.

#include "renderer/Vertex.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Nyx {

class VulkanContext;
class Mesh;
class Texture;

class ResourceCache {
public:
    void init(VulkanContext& context);
    void cleanup(VulkanContext& context);

    // Get or create a mesh from OBJ file
    Mesh* getOrCreateMeshFromOBJ(VulkanContext& context, const std::string& filepath);

    // Get or create a mesh from raw vertex/index data (keyed by name)
    Mesh* getOrCreateMesh(VulkanContext& context, const std::string& key,
                          const std::vector<Vertex>& vertices,
                          const std::vector<uint32_t>& indices);

    // Get or create a texture from file. srgb=false for data maps (normal /
    // metallic-roughness / occlusion) so they're sampled in linear space.
    Texture* getOrCreateTexture(VulkanContext& context, const std::string& filepath, bool srgb = true);

    // Get the 1x1 white default texture
    Texture* getDefaultTexture() { return m_defaultTexture.get(); }

private:
    std::unordered_map<std::string, std::unique_ptr<Mesh>>    m_meshes;
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_textures;
    std::unique_ptr<Texture> m_defaultTexture;
};

} // namespace Nyx
