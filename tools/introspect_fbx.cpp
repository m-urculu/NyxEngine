// One-shot tool: load an FBX with the same Assimp postprocess flags the
// engine uses for hair imports (no FlipUVs, triangulated, world-flattened),
// and print mesh + node layout. Used to figure out how to author scene
// entries for a hair FBX without launching the editor.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cstdio>
#include <functional>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: introspect_fbx <path>\n"); return 1; }
    Assimp::Importer imp;
    unsigned flags = aiProcess_Triangulate
                   | aiProcess_GenSmoothNormals
                   | aiProcess_GenUVCoords
                   | aiProcess_JoinIdenticalVertices
                   | aiProcess_LimitBoneWeights
                   | aiProcess_ImproveCacheLocality;
    const aiScene* s = imp.ReadFile(argv[1], flags);
    if (!s) { std::fprintf(stderr, "load failed: %s\n", imp.GetErrorString()); return 1; }

    std::printf("MESHES: %u\n", s->mNumMeshes);
    for (unsigned i = 0; i < s->mNumMeshes; ++i) {
        const aiMesh* m = s->mMeshes[i];
        float mnx=1e9f,mny=1e9f,mnz=1e9f,mxx=-1e9f,mxy=-1e9f,mxz=-1e9f;
        for (unsigned v = 0; v < m->mNumVertices; ++v) {
            const auto& p = m->mVertices[v];
            if (p.x<mnx) mnx=p.x; if (p.y<mny) mny=p.y; if (p.z<mnz) mnz=p.z;
            if (p.x>mxx) mxx=p.x; if (p.y>mxy) mxy=p.y; if (p.z>mxz) mxz=p.z;
        }
        // UV range — important for detecting which UDIM tiles a mesh uses.
        float umin=1e9f,umax=-1e9f,vmin=1e9f,vmax=-1e9f;
        if (m->HasTextureCoords(0)) {
            for (unsigned v = 0; v < m->mNumVertices; ++v) {
                float u = m->mTextureCoords[0][v].x, vv = m->mTextureCoords[0][v].y;
                if (u<umin) umin=u; if (u>umax) umax=u;
                if (vv<vmin) vmin=vv; if (vv>vmax) vmax=vv;
            }
        }
        std::printf("  #%u name='%s' verts=%u tris=%u  bbox=[%.3f,%.3f,%.3f]..[%.3f,%.3f,%.3f]  uv=[%.2f..%.2f, %.2f..%.2f]\n",
                    i, m->mName.C_Str(), m->mNumVertices, m->mNumFaces,
                    mnx,mny,mnz,mxx,mxy,mxz, umin,umax,vmin,vmax);
    }

    // Walk node tree, print world transform of each mesh-bearing node.
    std::function<void(const aiNode*, aiMatrix4x4, int)> visit =
        [&](const aiNode* n, aiMatrix4x4 parent, int depth) {
        aiMatrix4x4 world = parent * n->mTransformation;
        if (n->mNumMeshes > 0) {
            aiVector3D t, s; aiQuaternion r;
            world.Decompose(s, r, t);
            std::printf("NODE depth=%d name='%s' meshes=[",
                        depth, n->mName.C_Str());
            for (unsigned k = 0; k < n->mNumMeshes; ++k)
                std::printf("%s%u", k?",":"", n->mMeshes[k]);
            std::printf("]\n  T=(%.3f,%.3f,%.3f)  R=(%.4f,%.4f,%.4f,%.4f)  S=(%.4f,%.4f,%.4f)\n",
                        t.x,t.y,t.z, r.w,r.x,r.y,r.z, s.x,s.y,s.z);
        }
        for (unsigned c = 0; c < n->mNumChildren; ++c)
            visit(n->mChildren[c], world, depth+1);
    };
    visit(s->mRootNode, aiMatrix4x4{}, 0);
    return 0;
}
