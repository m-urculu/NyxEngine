#pragma once

// Registry.h — ECS registry: entity manager + component pool storage
//
// Creates/destroys entities. Stores typed ComponentPools keyed by type_index.

#include "ecs/Entity.h"
#include "ecs/ComponentPool.h"
#include "ecs/IComponentPoolBase.h"

#include <memory>
#include <typeindex>
#include <unordered_map>

namespace Talos {

class Registry {
public:
    Entity createEntity();
    void destroyEntity(Entity entity);

    template <typename T>
    T& assign(Entity entity, const T& component) {
        return pool<T>().assign(entity, component);
    }

    template <typename T>
    T& assign(Entity entity, T&& component) {
        return pool<T>().assign(entity, std::forward<T>(component));
    }

    template <typename T>
    T& get(Entity entity) {
        return pool<T>().get(entity);
    }

    template <typename T>
    const T& get(Entity entity) const {
        return pool<T>().get(entity);
    }

    template <typename T>
    bool has(Entity entity) const {
        auto it = m_pools.find(std::type_index(typeid(T)));
        if (it == m_pools.end()) return false;
        return static_cast<const ComponentPool<T>*>(it->second.get())->has(entity);
    }

    template <typename T>
    ComponentPool<T>& pool() {
        auto key = std::type_index(typeid(T));
        auto it = m_pools.find(key);
        if (it == m_pools.end()) {
            auto [inserted, _] = m_pools.emplace(key, std::make_unique<ComponentPool<T>>());
            return *static_cast<ComponentPool<T>*>(inserted->second.get());
        }
        return *static_cast<ComponentPool<T>*>(it->second.get());
    }

    template <typename T>
    const ComponentPool<T>& pool() const {
        auto key = std::type_index(typeid(T));
        auto it = m_pools.find(key);
        // Return empty static pool if type never registered
        static const ComponentPool<T> empty;
        if (it == m_pools.end()) return empty;
        return *static_cast<const ComponentPool<T>*>(it->second.get());
    }

private:
    Entity m_nextEntity = 0;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPoolBase>> m_pools;
};

} // namespace Talos
