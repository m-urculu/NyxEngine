#pragma once

// IComponentPoolBase.h — Virtual base for type-erased component pools
//
// Allows Registry to destroy entities without knowing concrete component types.

#include "ecs/Entity.h"

namespace Talos {

class IComponentPoolBase {
public:
    virtual ~IComponentPoolBase() = default;
    virtual void removeIfExists(Entity entity) = 0;
};

} // namespace Talos
