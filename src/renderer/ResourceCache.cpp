#include "renderer/ResourceCache.h"
#include "renderer/VulkanContext.h"
#include "renderer/Mesh.h"
#include "renderer/Texture.h"
#include "renderer/Vertex.h"
#include "renderer/ObjLoader.h"
#include "Logger.h"

#include <stdexcept>

namespace Nyx {

void ResourceCache::init(VulkanContext& context) {
    m_defaultTexture = std::make_unique<Texture>();
    m_defaultTexture->createDefault(context);
}

void ResourceCache::cleanup(VulkanContext& context) {
    for (auto& [key, mesh] : m_meshes) {
        mesh->cleanup(context.getAllocator());
    }
    m_meshes.clear();

    for (auto& [key, tex] : m_textures) {
        tex->cleanup(context.getDevice(), context.getAllocator());
    }
    m_textures.clear();

    if (m_defaultTexture) {
        m_defaultTexture->cleanup(context.getDevice(), context.getAllocator());
        m_defaultTexture.reset();
    }
}

Mesh* ResourceCache::getOrCreateMeshFromOBJ(VulkanContext& context, const std::string& filepath) {
    auto it = m_meshes.find(filepath);
    if (it != m_meshes.end()) {
        return it->second.get();
    }

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    if (!ObjLoader::load(filepath, vertices, indices)) {
        throw std::runtime_error("Failed to load OBJ: " + filepath);
    }

    auto mesh = std::make_unique<Mesh>();
    mesh->init(context, vertices, indices);
    Mesh* ptr = mesh.get();
    m_meshes[filepath] = std::move(mesh);
    return ptr;
}

Mesh* ResourceCache::getOrCreateMesh(VulkanContext& context, const std::string& key,
                                      const std::vector<Vertex>& vertices,
                                      const std::vector<uint32_t>& indices) {
    auto it = m_meshes.find(key);
    if (it != m_meshes.end()) {
        return it->second.get();
    }

    auto mesh = std::make_unique<Mesh>();
    mesh->init(context, vertices, indices);
    Mesh* ptr = mesh.get();
    m_meshes[key] = std::move(mesh);
    return ptr;
}

Texture* ResourceCache::getOrCreateTexture(VulkanContext& context, const std::string& filepath, bool srgb) {
    // sRGB and linear views of the same file are distinct GPU textures → distinct keys.
    std::string key = (srgb ? "srgb:" : "lin:") + filepath;
    auto it = m_textures.find(key);
    if (it != m_textures.end()) {
        return it->second.get();
    }

    auto tex = std::make_unique<Texture>();
    tex->loadFromFile(context, filepath, srgb);
    Texture* ptr = tex.get();
    m_textures[key] = std::move(tex);
    return ptr;
}

} // namespace Nyx
