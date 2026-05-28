#include "renderer/GltfLoader.h"
#include "Logger.h"

#include <cgltf.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <unordered_map>
#include <stdexcept>

namespace Nyx {

namespace {

// Extract one primitive's geometry + base-color material (the original behaviour).
GltfMeshData extractPrimitive(const cgltf_primitive& prim) {
    GltfMeshData meshData;

    const cgltf_accessor* posAccessor     = nullptr;
    const cgltf_accessor* normalAccessor  = nullptr;
    const cgltf_accessor* uvAccessor      = nullptr;
    const cgltf_accessor* jointsAccessor  = nullptr;
    const cgltf_accessor* weightsAccessor = nullptr;
    for (cgltf_size ai = 0; ai < prim.attributes_count; ai++) {
        const cgltf_attribute& attr = prim.attributes[ai];
        if (attr.type == cgltf_attribute_type_position) posAccessor     = attr.data;
        if (attr.type == cgltf_attribute_type_normal)   normalAccessor  = attr.data;
        if (attr.type == cgltf_attribute_type_texcoord) uvAccessor      = attr.data;
        if (attr.type == cgltf_attribute_type_joints)   jointsAccessor  = attr.data;
        if (attr.type == cgltf_attribute_type_weights)  weightsAccessor = attr.data;
    }
    if (!posAccessor) return meshData;

    size_t vertexCount = posAccessor->count;
    meshData.vertices.resize(vertexCount);
    for (size_t v = 0; v < vertexCount; v++) {
        float pos[3];
        cgltf_accessor_read_float(posAccessor, v, pos, 3);
        meshData.vertices[v].position = {pos[0], pos[1], pos[2]};
        meshData.vertices[v].color    = {1.0f, 1.0f, 1.0f};
        meshData.vertices[v].normal   = {0.0f, 1.0f, 0.0f};
        meshData.vertices[v].texCoord = {0.0f, 0.0f};
    }
    if (normalAccessor)
        for (size_t v = 0; v < vertexCount; v++) {
            float n[3]; cgltf_accessor_read_float(normalAccessor, v, n, 3);
            meshData.vertices[v].normal = {n[0], n[1], n[2]};
        }
    if (uvAccessor)
        for (size_t v = 0; v < vertexCount; v++) {
            float uv[2]; cgltf_accessor_read_float(uvAccessor, v, uv, 2);
            meshData.vertices[v].texCoord = {uv[0], uv[1]};
        }
    if (jointsAccessor)
        for (size_t v = 0; v < vertexCount; v++) {
            cgltf_uint j[4] = {0,0,0,0}; cgltf_accessor_read_uint(jointsAccessor, v, j, 4);
            meshData.vertices[v].joints = {j[0], j[1], j[2], j[3]};
        }
    if (weightsAccessor)
        for (size_t v = 0; v < vertexCount; v++) {
            float wt[4] = {0,0,0,0}; cgltf_accessor_read_float(weightsAccessor, v, wt, 4);
            meshData.vertices[v].weights = {wt[0], wt[1], wt[2], wt[3]};
        }

    if (prim.indices) {
        meshData.indices.resize(prim.indices->count);
        for (size_t i = 0; i < prim.indices->count; i++)
            meshData.indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
    } else {
        meshData.indices.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; i++) meshData.indices[i] = static_cast<uint32_t>(i);
    }

    // URI of a texture-view's image, or "" (embedded/missing — Nyx loads URI textures only).
    auto texURI = [](const cgltf_texture_view& tv) -> std::string {
        if (tv.texture && tv.texture->image && tv.texture->image->uri) return tv.texture->image->uri;
        return {};
    };
    if (prim.material) {
        const cgltf_material* mat = prim.material;
        if (mat->has_pbr_metallic_roughness) {
            const auto& pbr = mat->pbr_metallic_roughness;
            meshData.baseColorTextureURI  = texURI(pbr.base_color_texture);
            meshData.metalRoughTextureURI = texURI(pbr.metallic_roughness_texture); // G=rough, B=metal
            meshData.baseColorFactor = {pbr.base_color_factor[0], pbr.base_color_factor[1],
                                        pbr.base_color_factor[2], pbr.base_color_factor[3]};
            meshData.metallicFactor  = pbr.metallic_factor;
            meshData.roughnessFactor = pbr.roughness_factor;
        }
        meshData.normalTextureURI    = texURI(mat->normal_texture);
        meshData.occlusionTextureURI = texURI(mat->occlusion_texture);
        // Alpha-masked (cutout) transparency. No sorted blending yet, so BLEND is
        // treated as a 0.5 cutout (renders the opaque core of the card).
        if (mat->alpha_mode == cgltf_alpha_mode_mask)
            meshData.alphaCutoff = mat->alpha_cutoff > 0.0f ? mat->alpha_cutoff : 0.5f;
        else if (mat->alpha_mode == cgltf_alpha_mode_blend)
            meshData.alphaCutoff = 0.5f;
    }
    return meshData;
}

} // namespace

std::vector<GltfMeshData> GltfLoader::load(const std::string& filepath) {
    GltfImport imp = loadImport(filepath);
    return std::move(imp.primitives);
}

GltfImport GltfLoader::loadImport(const std::string& filepath) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, filepath.c_str(), &data) != cgltf_result_success)
        throw std::runtime_error("Failed to parse glTF file: " + filepath);
    if (cgltf_load_buffers(&options, data, filepath.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        throw std::runtime_error("Failed to load glTF buffers: " + filepath);
    }

    GltfImport out;

    // Flat primitive list + mesh → first-flat-index map (so nodes can reference them).
    std::unordered_map<const cgltf_mesh*, int> meshFirst;
    for (cgltf_size mi = 0; mi < data->meshes_count; mi++) {
        const cgltf_mesh& mesh = data->meshes[mi];
        meshFirst[&mesh] = (int)out.primitives.size();
        for (cgltf_size pi = 0; pi < mesh.primitives_count; pi++)
            out.primitives.push_back(extractPrimitive(mesh.primitives[pi]));
    }

    // Node hierarchy (TRS, parent, mesh range).
    out.nodes.resize(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; i++) {
        const cgltf_node& n = data->nodes[i];
        GltfNode& gn = out.nodes[i];
        gn.name = n.name ? n.name : "";
        if (n.has_matrix) {
            glm::mat4 m = glm::make_mat4(n.matrix);
            glm::vec3 skew; glm::vec4 persp;
            glm::decompose(m, gn.scale, gn.rotation, gn.translation, skew, persp);
        } else {
            if (n.has_translation) gn.translation = {n.translation[0], n.translation[1], n.translation[2]};
            if (n.has_rotation)    gn.rotation = glm::quat(n.rotation[3], n.rotation[0], n.rotation[1], n.rotation[2]); // (w,x,y,z)
            if (n.has_scale)       gn.scale = {n.scale[0], n.scale[1], n.scale[2]};
        }
        gn.parent = n.parent ? (int)(n.parent - data->nodes) : -1;   // nodes are a contiguous array
        if (n.mesh) {
            auto it = meshFirst.find(n.mesh);
            if (it != meshFirst.end()) { gn.firstPrim = it->second; gn.primCount = (int)n.mesh->primitives_count; }
        }
        gn.skin = n.skin ? (int)(n.skin - data->skins) : -1;
    }

    // Skins: ordered joint nodes + inverse-bind matrices.
    out.skins.resize(data->skins_count);
    for (cgltf_size si = 0; si < data->skins_count; si++) {
        const cgltf_skin& sk = data->skins[si];
        GltfSkin& gs = out.skins[si];
        gs.jointNodes.resize(sk.joints_count);
        gs.inverseBind.assign(sk.joints_count, glm::mat4(1.0f));
        for (cgltf_size j = 0; j < sk.joints_count; j++) {
            gs.jointNodes[j] = (int)(sk.joints[j] - data->nodes);
            if (sk.inverse_bind_matrices) {
                float m[16]; cgltf_accessor_read_float(sk.inverse_bind_matrices, j, m, 16);
                gs.inverseBind[j] = glm::make_mat4(m);
            }
        }
    }

    // Animation clips.
    for (cgltf_size ai = 0; ai < data->animations_count; ai++) {
        const cgltf_animation& anim = data->animations[ai];
        GltfAnim clip;
        clip.name = anim.name ? anim.name : ("clip" + std::to_string(ai));
        for (cgltf_size ci = 0; ci < anim.channels_count; ci++) {
            const cgltf_animation_channel& ch = anim.channels[ci];
            if (!ch.target_node || !ch.sampler) continue;
            GltfAnimChannel c;
            if      (ch.target_path == cgltf_animation_path_type_translation) c.path = 0;
            else if (ch.target_path == cgltf_animation_path_type_rotation)    c.path = 1;
            else if (ch.target_path == cgltf_animation_path_type_scale)       c.path = 2;
            else continue;   // skip morph weights
            c.targetNode = (int)(ch.target_node - data->nodes);

            const cgltf_animation_sampler* s = ch.sampler;
            c.interp = (s->interpolation == cgltf_interpolation_type_step) ? 1 : 0;
            bool cubic = (s->interpolation == cgltf_interpolation_type_cubic_spline);
            int  comps = (c.path == 1) ? 4 : 3;
            size_t keys = s->input ? s->input->count : 0;
            c.times.resize(keys);
            c.values.resize(keys);
            for (size_t k = 0; k < keys; k++) {
                float t = 0.0f; cgltf_accessor_read_float(s->input, k, &t, 1);
                c.times[k] = t;
                float buf[4] = {0, 0, 0, 1};
                size_t outIdx = cubic ? (3 * k + 1) : k;     // cubic stores in/value/out → take value
                cgltf_accessor_read_float(s->output, outIdx, buf, comps);
                c.values[k] = {buf[0], buf[1], buf[2], comps == 4 ? buf[3] : 0.0f};
                clip.duration = std::max(clip.duration, t);
            }
            if (!c.times.empty()) clip.channels.push_back(std::move(c));
        }
        if (!clip.channels.empty()) out.animations.push_back(std::move(clip));
    }

    cgltf_free(data);
    LOG_INFO("Loaded glTF: {} ({} prims, {} nodes, {} clips, {} skins)",
             filepath, out.primitives.size(), out.nodes.size(), out.animations.size(), out.skins.size());
    return out;
}

} // namespace Nyx
