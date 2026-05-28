#include "ecs/Registry.h"

namespace Nyx {

Entity Registry::createEntity() {
    return m_nextEntity++;
}

void Registry::destroyEntity(Entity entity) {
    for (auto& [type, pool] : m_pools) {
        pool->removeIfExists(entity);
    }
}

} // namespace Nyx
