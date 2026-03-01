#pragma once

// MeshComponent.h — Associates an entity with a GPU mesh (non-owning)

namespace Talos {

class Mesh;

struct MeshComponent {
    Mesh* mesh = nullptr;
};

} // namespace Talos
