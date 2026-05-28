#pragma once

// ComponentPool.h — Dense array + sparse map component storage
//
// Uses swap-and-pop for O(1) removal. Dense array is cache-friendly for iteration.
// Sparse map provides O(1) entity→component lookup.

#include "ecs/Entity.h"
#include "ecs/IComponentPoolBase.h"

#include <vector>
#include <unordered_map>
#include <cstddef>
#include <cassert>

namespace Nyx {

template <typename T>
class ComponentPool : public IComponentPoolBase {
public:
    T& assign(Entity entity, const T& component) {
        assert(m_sparse.find(entity) == m_sparse.end() && "Entity already has this component");
        size_t index = m_dense.size();
        m_dense.push_back(component);
        m_entities.push_back(entity);
        m_sparse[entity] = index;
        return m_dense[index];
    }

    T& assign(Entity entity, T&& component) {
        assert(m_sparse.find(entity) == m_sparse.end() && "Entity already has this component");
        size_t index = m_dense.size();
        m_dense.push_back(std::move(component));
        m_entities.push_back(entity);
        m_sparse[entity] = index;
        return m_dense[index];
    }

    void remove(Entity entity) {
        auto it = m_sparse.find(entity);
        assert(it != m_sparse.end() && "Entity does not have this component");

        size_t index = it->second;
        size_t last  = m_dense.size() - 1;

        if (index != last) {
            // Swap with last element
            m_dense[index]    = std::move(m_dense[last]);
            m_entities[index] = m_entities[last];
            m_sparse[m_entities[index]] = index;
        }

        m_dense.pop_back();
        m_entities.pop_back();
        m_sparse.erase(it);
    }

    void removeIfExists(Entity entity) override {
        if (m_sparse.find(entity) != m_sparse.end()) {
            remove(entity);
        }
    }

    bool has(Entity entity) const {
        return m_sparse.find(entity) != m_sparse.end();
    }

    T& get(Entity entity) {
        auto it = m_sparse.find(entity);
        assert(it != m_sparse.end() && "Entity does not have this component");
        return m_dense[it->second];
    }

    const T& get(Entity entity) const {
        auto it = m_sparse.find(entity);
        assert(it != m_sparse.end() && "Entity does not have this component");
        return m_dense[it->second];
    }

    // Iteration support
    size_t size() const { return m_dense.size(); }
    T& operator[](size_t index) { return m_dense[index]; }
    const T& operator[](size_t index) const { return m_dense[index]; }
    Entity getEntity(size_t index) const { return m_entities[index]; }

    const std::vector<T>& data() const { return m_dense; }
    const std::vector<Entity>& entities() const { return m_entities; }

private:
    std::vector<T>                          m_dense;     // Packed component data
    std::vector<Entity>                     m_entities;  // Parallel entity IDs
    std::unordered_map<Entity, size_t>      m_sparse;    // Entity → dense index
};

} // namespace Nyx
