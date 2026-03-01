#pragma once

// TransformSystem.h — Computes world matrices, resolves parent chains

namespace Talos {

class Registry;

class TransformSystem {
public:
    static void update(Registry& registry);
};

} // namespace Talos
