#include "renderer/GltfLoader.h"
#include "Logger.h"

#include <cgltf.h>
#include <stdexcept>
#include <cstring>

namespace Talos {

std::vector<GltfMeshData> GltfLoader::load(const std::string& filepath) {
    cgltf_options options{};
    cgltf_data* data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, filepath.c_str(), &data);
    if (result != cgltf_result_success) {
        throw std::runtime_error("Failed to parse glTF file: " + filepath);
    }

    result = cgltf_load_buffers(&options, data, filepath.c_str());
    if (result != cgltf_result_success) {
        cgltf_free(data);
        throw std::runtime_error("Failed to load glTF buffers: " + filepath);
    }

    std::vector<GltfMeshData> meshes;

    for (cgltf_size mi = 0; mi < data->meshes_count; mi++) {
        const cgltf_mesh& mesh = data->meshes[mi];

        for (cgltf_size pi = 0; pi < mesh.primitives_count; pi++) {
            const cgltf_primitive& prim = mesh.primitives[pi];
            GltfMeshData meshData;

            // Find accessors for position, normal, texcoord
            const cgltf_accessor* posAccessor    = nullptr;
            const cgltf_accessor* normalAccessor = nullptr;
            const cgltf_accessor* uvAccessor     = nullptr;

            for (cgltf_size ai = 0; ai < prim.attributes_count; ai++) {
                const cgltf_attribute& attr = prim.attributes[ai];
                if (attr.type == cgltf_attribute_type_position)  posAccessor    = attr.data;
                if (attr.type == cgltf_attribute_type_normal)    normalAccessor = attr.data;
                if (attr.type == cgltf_attribute_type_texcoord)  uvAccessor     = attr.data;
            }

            if (!posAccessor) {
                LOG_WARN("glTF primitive has no position data, skipping");
                continue;
            }

            size_t vertexCount = posAccessor->count;
            meshData.vertices.resize(vertexCount);

            // Positions
            for (size_t v = 0; v < vertexCount; v++) {
                float pos[3];
                cgltf_accessor_read_float(posAccessor, v, pos, 3);
                meshData.vertices[v].position = {pos[0], pos[1], pos[2]};
                meshData.vertices[v].color    = {1.0f, 1.0f, 1.0f}; // default white
                meshData.vertices[v].normal   = {0.0f, 1.0f, 0.0f}; // default up
                meshData.vertices[v].texCoord = {0.0f, 0.0f};        // default
            }

            // Normals
            if (normalAccessor) {
                for (size_t v = 0; v < vertexCount; v++) {
                    float n[3];
                    cgltf_accessor_read_float(normalAccessor, v, n, 3);
                    meshData.vertices[v].normal = {n[0], n[1], n[2]};
                }
            }

            // Texcoords
            if (uvAccessor) {
                for (size_t v = 0; v < vertexCount; v++) {
                    float uv[2];
                    cgltf_accessor_read_float(uvAccessor, v, uv, 2);
                    meshData.vertices[v].texCoord = {uv[0], uv[1]};
                }
            }

            // Indices
            if (prim.indices) {
                meshData.indices.resize(prim.indices->count);
                for (size_t i = 0; i < prim.indices->count; i++) {
                    meshData.indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
                }
            } else {
                // No index buffer — generate sequential indices
                meshData.indices.resize(vertexCount);
                for (size_t i = 0; i < vertexCount; i++) {
                    meshData.indices[i] = static_cast<uint32_t>(i);
                }
            }

            // Base color texture URI
            if (prim.material &&
                prim.material->has_pbr_metallic_roughness &&
                prim.material->pbr_metallic_roughness.base_color_texture.texture &&
                prim.material->pbr_metallic_roughness.base_color_texture.texture->image) {
                const cgltf_image* img = prim.material->pbr_metallic_roughness.base_color_texture.texture->image;
                if (img->uri) {
                    meshData.baseColorTextureURI = img->uri;
                }
            }

            LOG_INFO("glTF mesh '{}' prim {}: {} vertices, {} indices, texture: {}",
                     mesh.name ? mesh.name : "unnamed", pi,
                     meshData.vertices.size(), meshData.indices.size(),
                     meshData.baseColorTextureURI.empty() ? "(none)" : meshData.baseColorTextureURI);

            meshes.push_back(std::move(meshData));
        }
    }

    cgltf_free(data);
    LOG_INFO("Loaded glTF: {} ({} meshes)", filepath, meshes.size());
    return meshes;
}

} // namespace Talos
