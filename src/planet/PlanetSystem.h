#pragma once

// PlanetSystem.h — chunked-LOD streaming planet renderer (engine subsystem).
//
// Turns a procgen::PlanetField (the project-side terrain function) into a
// fly-down planet: a cube-sphere whose 6 faces are each a quadtree, subdivided
// by distance to the camera. Each quadtree leaf is a grid chunk mesh displaced by
// the field, with skirts to hide cracks between neighbouring LOD levels.
//
// Chunks render OUTSIDE the ECS entity loop (so they don't pollute the scene /
// hierarchy / undo): the Renderer asks this system for a per-frame draw list and
// a shared material descriptor set.
//
// Streaming (Phase 2): chunk *geometry* is built on worker threads (the noise
// sampling is the expensive part and PlanetField is const/thread-safe), then
// uploaded to the GPU on the main thread a few per frame. An LRU cache evicts
// off-screen chunks, and freed meshes go through a frame-tagged deferred-deletion
// queue so an in-flight command buffer never references destroyed buffers.

#ifdef NYX_HAS_PLANET
#include "procgen/Planet.h"   // project-side terrain field (only when a project provides it)
#endif
#include "renderer/Mesh.h"
#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

namespace Nyx {

class VulkanContext;
class Descriptors;
class ResourceCache;

class PlanetSystem {
public:
    struct ChunkDraw { Mesh* mesh; glm::mat4 model; };

    void init(VulkanContext& ctx, Descriptors& desc, ResourceCache& cache,
              uint32_t seed, const glm::vec3& center, float radius);
    void cleanup(VmaAllocator allocator);

    // Refresh the visible-leaf set against the camera: upload finished chunks,
    // rebuild the draw list (queuing missing chunks for the workers), evict
    // off-screen chunks. Safe to call every frame.
    void update(const glm::vec3& camPos);

    bool active() const { return m_active; }
    const std::vector<ChunkDraw>& draws() const { return m_draws; }
    VkDescriptorSet materialSet() const { return m_materialSet; }

    glm::vec3 center() const { return m_center; }
    float     radius() const { return m_radius; }
    uint32_t  chunkCount() const { return static_cast<uint32_t>(m_chunks.size()); }

    // World distance from the planet centre to the terrain surface beneath worldPos
    // (samples the field directly — exact, not the chunk LOD). Used for collision.
    float surfaceDistance(const glm::vec3& worldPos) const;
    // Clamp a camera/world position so it stays at least `clearance` above the
    // terrain. Returns the (possibly lifted) position.
    glm::vec3 collide(const glm::vec3& worldPos, float clearance) const;

    void setLogStreaming(bool b) { m_logStreaming = b; }

private:
    struct Node { int face; int level; uint32_t i, j; };
    struct ReadyChunk { uint64_t key; std::vector<Vertex> verts; std::vector<uint32_t> idx; };
    struct Trash { std::unique_ptr<Mesh> mesh; uint64_t tag; };

    static constexpr int   kGrid         = 16;    // chunk grid resolution (G×G quads)
    static constexpr int   kMaxLevel     = 16;    // deepest quadtree level (~0.4u detail at r=150000)
    static constexpr float kSplitFactor  = 2.0f;  // larger = split sooner (more detail)
    // Near-field uniform LOD: every chunk within kNearUniform of the camera is forced to
    // the SAME level (kUniformLevel) so there are no LOD transitions where you can see
    // them — detail only steps down farther out (~past the walking horizon). Raise the
    // radius to cover more view (costs chunks); raise the level for finer near detail.
    // Uniform detail across the whole walking VIEW so there are no LOD terraces in sight.
    // At radius 150000 the surface horizon is ~2000u, so the uniform field must reach that
    // far — which caps the level (finer would blow the chunk budget). Level 12 ≈ 4.6u tris.
    static constexpr int   kUniformLevel = 12;
    static constexpr float kNearUniform  = 2000.0f;
    // Whole-planet overview (camera far away): distance LOD alone would keep the planet
    // coarse/blocky. Force the VISIBLE hemisphere to this level so the preview is crisp;
    // the hidden back side stays coarse to spare the chunk budget.
    static constexpr int   kOverviewLevel = 4;
    static constexpr int   kUploadBudget = 8;     // GPU mesh uploads per frame (each blocks)
    static constexpr size_t kMaxChunks   = 5000;  // LRU cache cap (holds the uniform near field + overview)
    static constexpr uint64_t kTrashDelay = 3;    // frames to hold a freed mesh (> frames-in-flight)

    static uint64_t key(const Node& n) {
        return ((uint64_t)n.face << 56) | ((uint64_t)n.level << 48)
             | ((uint64_t)n.i << 24) | (uint64_t)n.j;
    }
    static glm::vec3  cubeDir(int face, float u, float v);
    static glm::dvec3 cubeDirD(int face, double u, double v);   // double for placement precision
    glm::dvec3 centerLocalD(const Node& n) const;               // planet-local chunk centre (double)
    double radiusUnit(float elevation) const;

    Mesh* getMesh(uint64_t k) const;
    void  generateSync(const Node& n);                  // build + upload immediately (roots)
    void  buildChunkMesh(const Node& n, std::vector<Vertex>& V, std::vector<uint32_t>& I) const;
    bool  shouldSplit(const Node& n, const glm::vec3& camPos) const;
    void  traverse(const Node& n, const glm::vec3& camPos);

    void  requestChunk(const Node& n);                  // queue for the workers (once)
    void  drainReady();                                 // upload finished chunks (budgeted)
    void  evictIfNeeded();                              // LRU evict over the cap
    void  collectTrash();                               // free meshes past kTrashDelay
    void  workerLoop();                                 // background: build chunk geometry
    void  startWorkers();
    void  stopWorkers();

    VulkanContext* m_ctx = nullptr;
#ifdef NYX_HAS_PLANET
    std::unique_ptr<procgen::PlanetField> m_field;
#endif
    glm::vec3 m_center{0.0f};
    float     m_radius = 1.0f;
    glm::vec3 m_camPos{0.0f};      // camera world pos captured at update() (for camera-relative models)
    bool      m_active = false;

    VkDescriptorPool m_descPool    = VK_NULL_HANDLE;
    VkDescriptorSet  m_materialSet = VK_NULL_HANDLE;
    Buffer           m_materialUBO;

    std::unordered_map<uint64_t, std::unique_ptr<Mesh>> m_chunks;   // main thread only
    std::unordered_map<uint64_t, uint64_t>              m_lastUsed; // key → last visited frame
    std::vector<Trash>     m_trash;
    std::vector<ChunkDraw> m_draws;
    uint64_t m_frame = 0;

    // Worker pool + queues.
    std::vector<std::thread>      m_workers;
    std::atomic<bool>             m_running{false};
    std::mutex                    m_reqMutex;
    std::condition_variable       m_reqCv;
    std::deque<Node>              m_requests;     // guarded by m_reqMutex
    std::mutex                    m_readyMutex;
    std::deque<ReadyChunk>        m_ready;        // guarded by m_readyMutex
    std::mutex                    m_pendingMutex;
    std::unordered_set<uint64_t>  m_pending;      // queued or building, guarded by m_pendingMutex

    bool     m_logStreaming = false;
    uint32_t m_lastLogged   = 0;
};

} // namespace Nyx
