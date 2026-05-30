#pragma once

// AssimpLoader.h — Multi-format model loader on top of Assimp. Returns the
// same GltfImport the glTF loader produces, so loadGltfScene downstream can
// consume FBX / OBJ / COLLADA / etc. without caring about the source format.
// Skinning + animation are not yet translated; static mesh + materials only.

#include "renderer/GltfLoader.h"
#include <string>

namespace Nyx {

class AssimpLoader {
public:
    // Throws std::runtime_error on parse failure.
    static GltfImport loadImport(const std::string& filepath);
};

} // namespace Nyx
