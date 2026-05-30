#include "renderer/AssimpLoader.h"
#include "Logger.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>

#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_image_resize2.h>

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <vector>

namespace Nyx {

namespace {

// ── Hair-card texture baking ────────────────────────────────────────────────
// The gladiator's hair cards ship as FBX geometry with a separate "Opacity"
// PNG in a parallel Maps/HairCards/<Part>/Opacity/ folder. FBX has no
// canonical alpha mode and the engine's material system reads the alpha mask
// from baseColorTexture.a, so we composite a fresh RGBA texture here:
//     RGB = flat warm brown, A = opacity mask
// and write it next to the FBX once. Subsequent loads pick up the cached PNG.
struct HairPart { const char* needle; const char* mapsSubdir; };
const HairPart kHairParts[] = {
    {"BeltFurHairCards",    "Belt"},
    {"CapeFurHairCards",    "Cape"},
    {"HelmetFurHairCards",  "Helmet"},
    {"HelmetFur",           "Helmet"},
    {"FullFacialHairCards", "FacialGroom"},
};

// Each UDIM tile in the bake gets resized down to TILE_PX × TILE_PX before
// stitching into a 2×2 atlas. Source tiles are usually 4K each — atlasing at
// full size would mean an 8K × 8K image, ~256 MB raw. 1024² per tile keeps
// the atlas at 2048², ~16 MB on disk + GPU, which is plenty for hair masks.
constexpr int TILE_PX = 1024;

// Result of a hair-bake: cache filename (relative to FBX dir) + which tiles
// the mesh's UV layout actually covers (so the UV remapper knows whether u/v
// in [1,2] is real or off the atlas).
struct HairBakeResult { std::string cacheName; bool tile1002, tile1011, tile1012; };

HairBakeResult bakeHairCardTexture(const std::string& fbxPath) {
    namespace fs = std::filesystem;
    HairBakeResult fail{};

    std::string filename = fs::path(fbxPath).filename().string();
    std::string dir      = fs::path(fbxPath).parent_path().string();

    const HairPart* part = nullptr;
    for (const auto& p : kHairParts)
        if (filename.find(p.needle) != std::string::npos) { part = &p; break; }
    if (!part) return fail;

    // Walk up from the FBX until we find Maps/HairCards/<part>/, then look for
    // opacity tiles in either the part dir itself (single-tile packs like Belt)
    // or in an Opacity/ subdir (multi-tile packs like FacialGroom).
    std::string partDir;
    for (fs::path cur = fs::path(fbxPath).parent_path();
         !cur.empty(); cur = cur.parent_path()) {
        fs::path candidate = cur / "Maps" / "HairCards" / part->mapsSubdir;
        std::error_code ec;
        if (fs::is_directory(candidate, ec)) { partDir = candidate.string(); break; }
        if (cur == cur.root_path()) break;
    }
    if (partDir.empty()) {
        LOG_WARN("hair bake: no Maps/HairCards/{}/ dir found near '{}'",
                 part->mapsSubdir, filename);
        return fail;
    }
    std::string opacityDir = partDir + "/Opacity";
    std::error_code ec;
    if (!fs::is_directory(opacityDir, ec)) opacityDir = partDir;

    // Find which UDIM tile PNGs actually exist (1001 / 1002 / 1011 / 1012).
    // Single-tile packs (no UDIM number in the filename) treat the one matching
    // "Opacity.png" as tile 1001.
    std::string tilePath[4];
    const char* tileTag[4] = {"1001", "1002", "1011", "1012"};
    for (const auto& entry : fs::directory_iterator(opacityDir, ec)) {
        std::string n = entry.path().filename().string();
        std::string lo = n;
        for (auto& c : lo) c = static_cast<char>(std::tolower(c));
        if (lo.size() < 4 || lo.substr(lo.size() - 4) != ".png") continue;
        // Only consider files that look like opacity maps (skip Depht/Tangent
        // siblings in single-folder packs).
        if (lo.find("opacity") == std::string::npos) continue;
        bool matchedTile = false;
        for (int t = 0; t < 4; ++t)
            if (n.find(tileTag[t]) != std::string::npos) { tilePath[t] = entry.path().string(); matchedTile = true; break; }
        if (!matchedTile && tilePath[0].empty())
            tilePath[0] = entry.path().string();           // single-tile fallback
    }
    if (tilePath[0].empty()) {
        LOG_WARN("hair bake: opacity PNG missing in {}", opacityDir);
        return fail;
    }

    HairBakeResult ok{};
    ok.cacheName = std::string("GeneratedHair_") + part->mapsSubdir + "_BCA.png";
    ok.tile1002  = !tilePath[1].empty();
    ok.tile1011  = !tilePath[2].empty();
    ok.tile1012  = !tilePath[3].empty();
    std::string cachePath = dir + "/" + ok.cacheName;
    if (fs::exists(cachePath)) return ok;   // already baked

    // Atlas: 2×2 layout, top half = v∈[1,2] (tiles 1011/1012), bottom = v∈[0,1].
    // Atlas image rows are top-down, so tiles for higher v values sit at lower
    // image y. Each tile is downscaled to TILE_PX × TILE_PX before placement.
    const int aw = TILE_PX * 2;
    const int ah = TILE_PX * 2;
    std::vector<uint8_t> atlas(static_cast<size_t>(aw) * ah * 4, 0);

    // Atlas quadrant offsets (atlas_x, atlas_y) per UDIM tile index 0..3.
    int qx[4] = { 0,        TILE_PX,  0,       TILE_PX };
    int qy[4] = { TILE_PX,  TILE_PX,  0,       0       };

    int maskCh = -1;   // pick the variance-winning channel from tile 1001 only;
                       // reused for the rest so we don't flip mid-atlas.
    for (int t = 0; t < 4; ++t) {
        if (tilePath[t].empty()) continue;
        int sw = 0, sh = 0, sch = 0;
        stbi_uc* px = stbi_load(tilePath[t].c_str(), &sw, &sh, &sch, 4);
        if (!px) { LOG_WARN("hair bake: stbi_load failed for tile {}", tileTag[t]); continue; }

        // Resize to TILE_PX × TILE_PX.
        std::vector<uint8_t> resized(static_cast<size_t>(TILE_PX) * TILE_PX * 4);
        stbir_resize_uint8_linear(px, sw, sh, 0,
                                  resized.data(), TILE_PX, TILE_PX, 0,
                                  STBIR_RGBA);
        stbi_image_free(px);

        if (maskCh < 0) {
            // First tile (1001) decides which channel carries the mask.
            double sumR = 0, sumA = 0, sumR2 = 0, sumA2 = 0;
            const int N = TILE_PX * TILE_PX;
            for (int i = 0; i < N; ++i) {
                double r = resized[i * 4 + 0], a = resized[i * 4 + 3];
                sumR += r; sumA += a; sumR2 += r * r; sumA2 += a * a;
            }
            double vR = (sumR2 - sumR * sumR / N) / N;
            double vA = (sumA2 - sumA * sumA / N) / N;
            maskCh = (vR >= vA) ? 0 : 3;
        }

        // Blit into atlas quadrant.
        for (int y = 0; y < TILE_PX; ++y) {
            uint8_t* dst = &atlas[((qy[t] + y) * aw + qx[t]) * 4];
            const uint8_t* src = &resized[y * TILE_PX * 4];
            for (int x = 0; x < TILE_PX; ++x) {
                dst[x * 4 + 3] = src[x * 4 + maskCh];   // alpha = mask
            }
        }
    }

    // Composite near-pure-black hair RGB everywhere; alpha already in place. The
    // scene's warm sun + sky IBL lifts dark colors significantly through ACES
    // tonemap, so we sit at (5,5,5) to land on perceived black after lighting.
    // Neutral grey (no R bias) avoids the warm-brown tint shifting under sun.
    constexpr uint8_t br = 5, bg = 5, bb = 5;
    for (int i = 0; i < aw * ah; ++i) {
        atlas[i * 4 + 0] = br;
        atlas[i * 4 + 1] = bg;
        atlas[i * 4 + 2] = bb;
    }

    if (!stbi_write_png(cachePath.c_str(), aw, ah, 4, atlas.data(), aw * 4)) {
        LOG_ERROR("hair bake: stbi_write_png failed for {}", cachePath);
        return fail;
    }
    LOG_INFO("hair bake: created {} ({}×{} atlas, tiles 1001{}{}{})",
             cachePath, aw, ah,
             ok.tile1002 ? " 1002" : "",
             ok.tile1011 ? " 1011" : "",
             ok.tile1012 ? " 1012" : "");
    return ok;
}


glm::vec3 toGlm(const aiVector3D& v) { return {v.x, v.y, v.z}; }
glm::vec2 toGlm2(const aiVector3D& v) { return {v.x, v.y}; }
glm::vec4 toGlm(const aiColor4D& c) { return {c.r, c.g, c.b, c.a}; }

void decompose(const aiMatrix4x4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    aiVector3D ait, ais;
    aiQuaternion aiq;
    m.Decompose(ais, aiq, ait);
    t = {ait.x, ait.y, ait.z};
    s = {ais.x, ais.y, ais.z};
    // glm::quat takes (w,x,y,z); aiQuaternion stores (w,x,y,z) too.
    r = glm::quat(aiq.w, aiq.x, aiq.y, aiq.z);
}

std::string firstTexturePath(const aiMaterial* m, aiTextureType type) {
    aiString p;
    if (m->GetTextureCount(type) > 0 && m->GetTexture(type, 0, &p) == AI_SUCCESS)
        return std::string(p.C_Str());
    return {};
}

// Translate a single aiMesh into a GltfMeshData (positions + normals + UV0 +
// indices + material/PBR params + texture URIs). Skinning weights are not
// extracted yet — those rigs come through Assimp on FBX but Nyx's skinning
// loader still expects glTF skin tables.
GltfMeshData buildPrim(const aiScene* scene, const aiMesh* m) {
    GltfMeshData md;
    md.vertices.reserve(m->mNumVertices);
    for (unsigned v = 0; v < m->mNumVertices; ++v) {
        Vertex vert{};
        vert.position = toGlm(m->mVertices[v]);
        vert.normal   = m->HasNormals()             ? toGlm(m->mNormals[v])             : glm::vec3(0, 1, 0);
        vert.color    = glm::vec3(1.0f);
        vert.texCoord = m->HasTextureCoords(0)      ? toGlm2(m->mTextureCoords[0][v])   : glm::vec2(0.0f);
        md.vertices.push_back(vert);
    }
    md.indices.reserve(static_cast<size_t>(m->mNumFaces) * 3);
    for (unsigned f = 0; f < m->mNumFaces; ++f) {
        const aiFace& face = m->mFaces[f];
        for (unsigned k = 0; k < face.mNumIndices; ++k)
            md.indices.push_back(face.mIndices[k]);
    }

    if (m->mMaterialIndex < scene->mNumMaterials) {
        const aiMaterial* mat = scene->mMaterials[m->mMaterialIndex];
        aiColor4D bc;
        // glTF PBR base color first; fall back to legacy diffuse.
        if (aiGetMaterialColor(mat, AI_MATKEY_BASE_COLOR, &bc) == AI_SUCCESS)
            md.baseColorFactor = toGlm(bc);
        else if (aiGetMaterialColor(mat, AI_MATKEY_COLOR_DIFFUSE, &bc) == AI_SUCCESS)
            md.baseColorFactor = toGlm(bc);
        float f = 0.0f;
        if (aiGetMaterialFloat(mat, AI_MATKEY_METALLIC_FACTOR, &f)  == AI_SUCCESS) md.metallicFactor  = f;
        if (aiGetMaterialFloat(mat, AI_MATKEY_ROUGHNESS_FACTOR, &f) == AI_SUCCESS) md.roughnessFactor = f;

        // Texture URIs: try the modern PBR slots first, then the legacy ones.
        std::string base = firstTexturePath(mat, aiTextureType_BASE_COLOR);
        if (base.empty()) base = firstTexturePath(mat, aiTextureType_DIFFUSE);
        md.baseColorTextureURI = base;

        std::string nrm = firstTexturePath(mat, aiTextureType_NORMALS);
        if (nrm.empty())  nrm = firstTexturePath(mat, aiTextureType_HEIGHT);   // FBX sometimes
        md.normalTextureURI = nrm;

        // glTF packs Roughness in G, Metallic in B (and AO in R). Assimp exposes
        // either as METALNESS+DIFFUSE_ROUGHNESS or a combined GLTF_METALLIC_ROUGHNESS.
        std::string mr = firstTexturePath(mat, aiTextureType_METALNESS);
        if (mr.empty()) mr = firstTexturePath(mat, aiTextureType_DIFFUSE_ROUGHNESS);
        if (mr.empty()) mr = firstTexturePath(mat, aiTextureType_UNKNOWN);     // some loaders dump here
        md.metalRoughTextureURI = mr;

        md.occlusionTextureURI = firstTexturePath(mat, aiTextureType_AMBIENT_OCCLUSION);
        if (md.occlusionTextureURI.empty())
            md.occlusionTextureURI = firstTexturePath(mat, aiTextureType_LIGHTMAP);

        // FBX has no canonical alpha mode — opacity comes via the opacity
        // texture which we'd need to fold into baseColor.a separately. For
        // now hair cards still need the hairconv.py path; this loader emits
        // opaque materials by default.
    }
    return md;
}

} // namespace

GltfImport AssimpLoader::loadImport(const std::string& filepath) {
    namespace fs = std::filesystem;

    // Hair detection happens before import: hair-card FBX uses UDIM UVs (tiles
    // spanning u/v∈[0,2]), and aiProcess_FlipUVs would clobber any v>1 by
    // computing v_new=1-v_old per vertex regardless of tile. We instead leave
    // the UVs in the artist's OpenGL convention and apply our own atlas remap
    // (which folds the V-flip into the same step).
    std::string filename = fs::path(filepath).filename().string();
    bool isHair = false;
    for (const auto& p : kHairParts)
        if (filename.find(p.needle) != std::string::npos) { isHair = true; break; }

    unsigned flags = aiProcess_Triangulate
                   | aiProcess_GenSmoothNormals
                   | aiProcess_GenUVCoords
                   | aiProcess_JoinIdenticalVertices
                   | aiProcess_LimitBoneWeights
                   | aiProcess_ImproveCacheLocality;
    if (!isHair) flags |= aiProcess_FlipUVs;   // glTF/Vulkan UV (origin top-left)

    Assimp::Importer imp;
    const aiScene* scene = imp.ReadFile(filepath, flags);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 || !scene->mRootNode) {
        throw std::runtime_error(std::string("Assimp: ") + imp.GetErrorString());
    }

    // Stage prims keyed by aiMesh index so a node referencing mesh i can copy
    // it into a contiguous slice (loadGltfScene assumes firstPrim..firstPrim+N
    // are the node's primitives — true for glTF, not for raw aiNode meshes).
    std::vector<GltfMeshData> src;
    src.reserve(scene->mNumMeshes);
    for (unsigned i = 0; i < scene->mNumMeshes; ++i)
        src.push_back(buildPrim(scene, scene->mMeshes[i]));

    GltfImport out;

    // FBX (and most DCC tools) emit a dense tree of armatures, locators,
    // joints, etc — most of them with no mesh attached. Creating one editor
    // entity per aiNode floods the hierarchy with hundreds of empty rows.
    //
    // Instead, walk the tree and only emit a flat list of mesh-bearing
    // entities, with the cumulative world transform baked into each one's
    // TRS. Hierarchy stays clean (one row per actual mesh, all root-parented),
    // and individual meshes still sit at their authored world positions.
    std::function<void(const aiNode*, const aiMatrix4x4&)> visit =
        [&](const aiNode* n, const aiMatrix4x4& parentXform) {
        aiMatrix4x4 world = parentXform * n->mTransformation;
        if (n->mNumMeshes > 0) {
            GltfNode gn{};
            gn.name   = n->mName.C_Str();
            gn.parent = -1;
            decompose(world, gn.translation, gn.rotation, gn.scale);
            gn.firstPrim = static_cast<int>(out.primitives.size());
            gn.primCount = static_cast<int>(n->mNumMeshes);
            for (unsigned m = 0; m < n->mNumMeshes; ++m) {
                unsigned mi = n->mMeshes[m];
                if (mi < src.size()) out.primitives.push_back(src[mi]);
            }
            out.nodes.push_back(gn);
        }
        for (unsigned c = 0; c < n->mNumChildren; ++c)
            visit(n->mChildren[c], world);
    };
    visit(scene->mRootNode, aiMatrix4x4{});

    // Hair-card FBX → bake a 2×2 UDIM atlas (brown RGB + opacity mask alpha)
    // into a sibling PNG, override every primitive's material to use it as an
    // alpha-cutout map, then rescale UVs from UDIM (u/v∈[0,2]) into the atlas
    // (u/v∈[0,1]). The V flip from OpenGL → Vulkan convention is folded into
    // the rescale, so no aiProcess_FlipUVs is needed for hair. Vertices that
    // land in a tile that wasn't present in the source folder end up in the
    // zero-initialised atlas regions — alpha 0 → discarded by the cutout.
    if (isHair) {
        HairBakeResult bake = bakeHairCardTexture(filepath);
        if (!bake.cacheName.empty()) {
            for (auto& md : out.primitives) {
                md.baseColorTextureURI = bake.cacheName;
                md.alphaCutoff         = 0.5f;
                md.baseColorFactor     = glm::vec4(1.0f);
                for (auto& vx : md.vertices) {
                    vx.texCoord = glm::vec2(vx.texCoord.x * 0.5f,
                                            1.0f - vx.texCoord.y * 0.5f);
                }
            }
            LOG_INFO("hair bake: applied {} to {} primitives", bake.cacheName, out.primitives.size());
        }
    }

    // Per-prim summary — verifies the primitives the engine actually picks up
    // for downstream resolveMesh + cache lookup. Catches silent loses where a
    // mesh imports as 0 verts / 0 indices.
    for (size_t i = 0; i < out.primitives.size(); ++i) {
        const auto& p = out.primitives[i];
        LOG_INFO("  prim #{}: {} verts / {} indices / tex='{}' acut={}",
                 i, p.vertices.size(), p.indices.size(),
                 p.baseColorTextureURI, p.alphaCutoff);
    }
    LOG_INFO("Assimp: '{}' -> {} prims, {} nodes", filepath,
             out.primitives.size(), out.nodes.size());
    return out;
}

} // namespace Nyx
