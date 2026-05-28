#pragma once

// MeshComponent.h — Associates an entity with a GPU mesh (non-owning)

#include <string>

namespace Nyx {

class Mesh;

struct MeshComponent {
    Mesh* mesh = nullptr;

    // How to recreate this mesh when a scene is (re)loaded. One of:
    //   "prim:cube" / "prim:plane"  — procedural primitive
    //   "obj:<path>"                — OBJ file (path from project root)
    //   "gltf:<path>#<index>"       — the Nth primitive of a glTF file
    std::string source;
};

} // namespace Nyx
