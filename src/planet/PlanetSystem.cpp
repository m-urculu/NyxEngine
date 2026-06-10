#include "planet/PlanetSystem.h"

#include "renderer/VulkanContext.h"
#include "renderer/Descriptors.h"
#include "renderer/ResourceCache.h"
#include "renderer/Texture.h"
#include "renderer/UniformTypes.h"
#include "Logger.h"

#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <algorithm>
#include <cmath>

namespace Nyx {

// ── Cube → sphere ─────────────────────────────────────────────────────────────
// Map a face + (u,v) in [-1,1] to a point on the cube, then normalise to the unit
// sphere. Plain normalisation (not area-corrected) — fine for procedural terrain.
glm::vec3 PlanetSystem::cubeDir(int face, float u, float v) {
    glm::vec3 p;
    switch (face) {
        case 0: p = { 1.0f,    v,   -u }; break;  // +X
        case 1: p = {-1.0f,    v,    u }; break;  // -X
        case 2: p = {   u, 1.0f,   -v }; break;  // +Y
        case 3: p = {   u,-1.0f,    v }; break;  // -Y
        case 4: p = {   u,    v, 1.0f }; break;  // +Z
        default:p = {  -u,    v,-1.0f }; break;  // -Z
    }
    return glm::normalize(p);
}

// Double-precision cube→sphere. Used for vertex/chunk-centre PLACEMENT so the
// planet stays jitter-free at large radii (the float version is fine for sampling
// the terrain field, where mm precision is irrelevant).
glm::dvec3 PlanetSystem::cubeDirD(int face, double u, double v) {
    glm::dvec3 p;
    switch (face) {
        case 0: p = { 1.0,   v,  -u }; break;  // +X
        case 1: p = {-1.0,   v,   u }; break;  // -X
        case 2: p = {   u, 1.0,  -v }; break;  // +Y
        case 3: p = {   u,-1.0,   v }; break;  // -Y
        case 4: p = {   u,   v, 1.0 }; break;  // +Z
        default:p = {  -u,   v,-1.0 }; break;  // -Z
    }
    return glm::normalize(p);
}

// Planet-local centre of a node (no elevation displacement) — the reference point
// chunk vertices are stored relative to, in double for placement precision.
glm::dvec3 PlanetSystem::centerLocalD(const Node& n) const {
    double nodeSize = 2.0 / double(1 << n.level);
    double cu = -1.0 + (n.i + 0.5) * nodeSize;
    double cv = -1.0 + (n.j + 0.5) * nodeSize;
    return cubeDirD(n.face, cu, cv) * (double)m_radius;
}

double PlanetSystem::radiusUnit(float e) const {
    using F = procgen::PlanetField;
    if (e <= F::kSeaLevel) return 1.0;                        // flat water surface
    // Double throughout: at radius 150000 a float radial distance quantizes to ~0.015u
    // steps, which terraces gently-sloped terrain ("staircase"). Double keeps it smooth.
    double t      = ((double)e - F::kSeaLevel) / ((double)F::kMaxElev - F::kSeaLevel);
    double shaped = std::pow(glm::clamp(t, 0.0, 1.0), (double)F::kHeightExp);
    return 1.0 + shaped * (double)F::kRelief;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
void PlanetSystem::init(VulkanContext& ctx, Descriptors& desc, ResourceCache& cache,
                        uint32_t seed, const glm::vec3& center, float radius) {
    m_ctx    = &ctx;
    m_field  = std::make_unique<procgen::PlanetField>(seed);
    m_center = center;
    m_radius = radius;

    // Chunk material set, owned here so the scene's resetMaterials() can't free it.
    // Default white texture for all maps (vertex colour carries the biome; the frag
    // shader multiplies baseColor × vertex colour). Matte, no normal/metal-rough maps.
    VkDevice     device = ctx.getDevice();
    VmaAllocator alloc  = ctx.getAllocator();
    Texture*     def    = cache.getDefaultTexture();

    std::array<VkDescriptorPoolSize, 2> psizes{};
    psizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; psizes[0].descriptorCount = 4;
    psizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         psizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.poolSizeCount = (uint32_t)psizes.size();
    pci.pPoolSizes    = psizes.data();
    pci.maxSets       = 1;
    vkCreateDescriptorPool(device, &pci, nullptr, &m_descPool);

    VkDescriptorSetLayout layout = desc.getMaterialLayout();
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = m_descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &layout;
    vkAllocateDescriptorSets(device, &ai, &m_materialSet);

    // Material UBO via staging → GPU_ONLY (the GTX 960 reads once-written host-visible
    // UBOs stale; see the PBR/UBO staging bug). Written once, never mutated.
    MaterialParams params{};
    params.baseColorFactor = glm::vec4(1.0f);
    params.metallic        = 0.0f;
    params.roughness       = 0.95f;
    m_materialUBO.init(alloc, sizeof(MaterialParams),
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VMA_MEMORY_USAGE_GPU_ONLY);
    {
        Buffer staging;
        staging.init(alloc, sizeof(MaterialParams), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
        staging.uploadData(alloc, &params, sizeof(MaterialParams));
        Buffer::copyBuffer(ctx, staging.getBuffer(), m_materialUBO.getBuffer(), sizeof(MaterialParams));
        staging.cleanup(alloc);
    }

    VkDescriptorImageInfo img{};
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img.imageView   = def->getImageView();
    img.sampler     = def->getSampler();
    VkDescriptorBufferInfo buf{};
    buf.buffer = m_materialUBO.getBuffer();
    buf.offset = 0;
    buf.range  = sizeof(MaterialParams);

    std::array<VkWriteDescriptorSet, 5> w{};
    auto samp = [&](int i, uint32_t binding) {
        w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet          = m_materialSet;
        w[i].dstBinding      = binding;
        w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].descriptorCount = 1;
        w[i].pImageInfo      = &img;
    };
    samp(0, 0); samp(2, 2); samp(3, 3); samp(4, 4);     // baseColor / normal / metalRough / occlusion
    w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet          = m_materialSet;
    w[1].dstBinding      = 1;
    w[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].descriptorCount = 1;
    w[1].pBufferInfo     = &buf;
    vkUpdateDescriptorSets(device, (uint32_t)w.size(), w.data(), 0, nullptr);

    // Force the 6 root faces synchronously so traverse() always has something to
    // draw; finer chunks then stream in on the worker threads.
    for (int face = 0; face < 6; ++face) generateSync(Node{face, 0, 0, 0});
    startWorkers();

    m_active = true;
    LOG_INFO("PlanetSystem: planet seed {} radius {} ({} root chunks, {} workers)",
             seed, radius, m_chunks.size(), m_workers.size());
}

void PlanetSystem::cleanup(VmaAllocator allocator) {
    stopWorkers();                                  // join before freeing field/meshes
    for (auto& t : m_trash) t.mesh->cleanup(allocator);
    m_trash.clear();
    for (auto& [k, mesh] : m_chunks) mesh->cleanup(allocator);
    m_chunks.clear();
    m_lastUsed.clear();
    m_draws.clear();
    m_frame = 0;
    m_materialUBO.cleanup(allocator);
    if (m_descPool != VK_NULL_HANDLE && m_ctx) {
        vkDestroyDescriptorPool(m_ctx->getDevice(), m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
    }
    m_materialSet = VK_NULL_HANDLE;
    m_field.reset();
    m_active = false;
    m_lastLogged = 0;
}

Mesh* PlanetSystem::getMesh(uint64_t k) const {
    auto it = m_chunks.find(k);
    return it == m_chunks.end() ? nullptr : it->second.get();
}

float PlanetSystem::surfaceDistance(const glm::vec3& worldPos) const {
    if (!m_field) return m_radius;
    glm::vec3 d = worldPos - m_center;
    float len = glm::length(d);
    glm::vec3 dir = len < 1e-4f ? glm::vec3(0, 1, 0) : d / len;
    return m_radius * m_field->surfaceRadius(dir);
}

glm::vec3 PlanetSystem::collide(const glm::vec3& worldPos, float clearance) const {
    if (!m_field) return worldPos;
    glm::vec3 d = worldPos - m_center;
    float len = glm::length(d);
    if (len < 1e-4f) return worldPos;
    glm::vec3 dir   = d / len;
    float minLen = m_radius * m_field->surfaceRadius(dir) + clearance;
    return (len < minLen) ? m_center + dir * minLen : worldPos;
}

// ── Chunk meshing ─────────────────────────────────────────────────────────────
void PlanetSystem::buildChunkMesh(const Node& nd, std::vector<Vertex>& V,
                                  std::vector<uint32_t>& I) const {
    const int    G        = kGrid;
    const int    stride   = G + 1;
    const double nodeSize = 2.0 / double(1 << nd.level);
    const double u0       = -1.0 + nd.i * nodeSize;
    const double v0       = -1.0 + nd.j * nodeSize;
    const procgen::PlanetField& f = *m_field;

    // Vertices are stored RELATIVE to the chunk's local centre (double-computed) so
    // their float coords stay small (chunk-sized) regardless of planet radius — the
    // floating-origin trick. The per-chunk model matrix (built in traverse) adds the
    // centre back, camera-relative, in double. radialApprox is the chunk's outward
    // direction, used for winding/normal orientation now that positions are local.
    const glm::dvec3 centerD = centerLocalD(nd);
    const glm::vec3  radialApprox = glm::normalize(glm::vec3(centerD));

    std::vector<float> elevG(stride * stride);
    V.reserve(stride * stride + stride * 8);
    for (int b = 0; b <= G; ++b) {
        for (int a = 0; a <= G; ++a) {
            double u = u0 + (a / double(G)) * nodeSize;
            double v = v0 + (b / double(G)) * nodeSize;
            glm::dvec3 dirD = cubeDirD(nd.face, u, v);
            glm::vec3  dir  = glm::vec3(dirD);
            float e = f.elevation(dir);
            glm::dvec3 posD = dirD * ((double)m_radius * radiusUnit(e));   // radial in double → no terracing
            Vertex vert{};
            vert.position = glm::vec3(posD - centerD);          // chunk-local
            vert.normal   = glm::vec3(0.0f);
            vert.texCoord = glm::vec2(a / float(G), b / float(G));
            V.push_back(vert);
            elevG[b * stride + a] = e;
        }
    }

    // Winding: pick the order that makes face normals point outward (the opaque
    // pipeline culls back faces). Test the first quad against its radial direction.
    glm::vec3 n0 = glm::cross(V[stride].position - V[0].position,
                              V[1].position - V[0].position);
    bool flip = glm::dot(n0, radialApprox) < 0.0f;

    I.reserve(G * G * 6 + G * 24);
    for (int b = 0; b < G; ++b) {
        for (int a = 0; a < G; ++a) {
            uint32_t i00 = b * stride + a,       i10 = b * stride + a + 1;
            uint32_t i01 = (b + 1) * stride + a, i11 = (b + 1) * stride + a + 1;
            if (!flip) {
                I.push_back(i00); I.push_back(i01); I.push_back(i10);
                I.push_back(i10); I.push_back(i01); I.push_back(i11);
            } else {
                I.push_back(i00); I.push_back(i10); I.push_back(i01);
                I.push_back(i10); I.push_back(i11); I.push_back(i01);
            }
        }
    }

    // Normals via central differences of VERTEX POSITIONS on the grid. Interior taps reuse
    // the positions we already built (free); only the 4 edge rings sample one step OUTSIDE
    // the chunk from the field. This is ~15× cheaper than re-sampling the field 4× per
    // vertex (which backed up chunk generation so badly the planet streamed full of holes),
    // and the normals now match the actual rendered geometry (no finite-step mismatch →
    // no jaggedness). Edges sample the SAME world point the neighbour chunk's interior uses,
    // so same-level chunks still get identical edge normals → no lighting seam.
    auto worldAt = [&](double uu, double vv) -> glm::vec3 {
        glm::dvec3 dd = cubeDirD(nd.face, uu, vv);
        float      ee = f.elevation(glm::vec3(dd));
        glm::dvec3 pp = dd * ((double)m_radius * radiusUnit(ee));
        return glm::vec3(pp - centerD);
    };
    auto posAt = [&](int a, int b) -> glm::vec3 {
        if (a >= 0 && a <= G && b >= 0 && b <= G) return V[b * stride + a].position;  // reuse
        double u = u0 + (a / double(G)) * nodeSize;
        double v = v0 + (b / double(G)) * nodeSize;
        return worldAt(u, v);                                                          // one step out
    };
    for (int b = 0; b <= G; ++b) {
        for (int a = 0; a <= G; ++a) {
            glm::vec3 tu = posAt(a + 1, b) - posAt(a - 1, b);
            glm::vec3 tv = posAt(a, b + 1) - posAt(a, b - 1);
            glm::vec3 n  = glm::cross(tu, tv);
            Vertex& vert = V[b * stride + a];
            vert.normal  = (glm::dot(n, n) > 1e-20f) ? glm::normalize(n) : radialApprox;
            if (glm::dot(vert.normal, radialApprox) < 0.0f) vert.normal = -vert.normal;  // outward
        }
    }

    // Biome colours. Positions are chunk-local, so recover the radial direction by
    // adding the centre back (in double) before normalising.
    for (int idx = 0; idx < stride * stride; ++idx) {
        glm::vec3 dir   = glm::normalize(glm::vec3(glm::dvec3(V[idx].position) + centerD));
        float     lat   = std::fabs(dir.y);
        float     slope = 1.0f - glm::clamp(glm::dot(V[idx].normal, dir), 0.0f, 1.0f);
        V[idx].color = f.shade(elevG[idx], slope, lat, f.moisture(dir), procgen::PlanetField::kMaxElev);
    }

    // ── Skirts ──
    // Drop a wall straight down (toward the planet centre) around the perimeter so
    // a finer neighbour's edge can't show a gap. Emitted double-sided (both tri
    // windings) so the wall is visible regardless of back-face culling.
    float skirtDrop = (float)(m_radius * (nodeSize / double(G)) * 2.0);
    auto addSkirt = [&](const std::vector<uint32_t>& edge) {
        uint32_t base = (uint32_t)V.size();
        for (uint32_t ci : edge) {
            Vertex sv = V[ci];
            sv.position = V[ci].position - radialApprox * skirtDrop;   // drop toward planet centre
            V.push_back(sv);
        }
        for (size_t k = 0; k + 1 < edge.size(); ++k) {
            uint32_t a = edge[k], b = edge[k + 1], sa = base + (uint32_t)k, sb = base + (uint32_t)k + 1;
            I.push_back(a); I.push_back(b);  I.push_back(sb);   // front
            I.push_back(a); I.push_back(sb); I.push_back(sa);
            I.push_back(a); I.push_back(sb); I.push_back(b);    // back
            I.push_back(a); I.push_back(sa); I.push_back(sb);
        }
    };
    std::vector<uint32_t> top, bottom, left, right;
    for (int a = 0; a <= G; ++a) { top.push_back(a); bottom.push_back(G * stride + a); }
    for (int b = 0; b <= G; ++b) { left.push_back(b * stride); right.push_back(b * stride + G); }
    addSkirt(top); addSkirt(bottom); addSkirt(left); addSkirt(right);
}

void PlanetSystem::generateSync(const Node& nd) {
    std::vector<Vertex>   V;
    std::vector<uint32_t> I;
    buildChunkMesh(nd, V, I);
    if (V.empty() || I.empty()) return;
    auto mesh = std::make_unique<Mesh>();
    mesh->init(*m_ctx, V, I);
    m_chunks[key(nd)] = std::move(mesh);
}

// ── Streaming: worker generation + budgeted upload + LRU eviction ─────────────
void PlanetSystem::workerLoop() {
    for (;;) {
        Node nd;
        {
            std::unique_lock<std::mutex> lk(m_reqMutex);
            m_reqCv.wait(lk, [&] { return !m_running || !m_requests.empty(); });
            if (!m_running) return;
            nd = m_requests.front();
            m_requests.pop_front();
        }
        ReadyChunk rc;
        rc.key = key(nd);
        buildChunkMesh(nd, rc.verts, rc.idx);          // const, thread-safe (reads field only)
        {
            std::lock_guard<std::mutex> lk(m_readyMutex);
            m_ready.push_back(std::move(rc));
        }
    }
}

void PlanetSystem::startWorkers() {
    m_running = true;
    unsigned n = std::thread::hardware_concurrency();
    n = n > 3 ? (n - 2) : 1;                            // leave room for main + render
    if (n > 4) n = 4;
    for (unsigned i = 0; i < n; ++i) m_workers.emplace_back([this] { workerLoop(); });
}

void PlanetSystem::stopWorkers() {
    { std::lock_guard<std::mutex> lk(m_reqMutex); m_running = false; }
    m_reqCv.notify_all();
    for (std::thread& t : m_workers) if (t.joinable()) t.join();
    m_workers.clear();
    m_requests.clear();
    m_ready.clear();
    m_pending.clear();
}

void PlanetSystem::requestChunk(const Node& nd) {
    uint64_t k = key(nd);
    if (getMesh(k)) return;
    {
        std::lock_guard<std::mutex> lk(m_pendingMutex);
        if (m_pending.count(k)) return;
        m_pending.insert(k);
    }
    {
        std::lock_guard<std::mutex> lk(m_reqMutex);
        m_requests.push_back(nd);
    }
    m_reqCv.notify_one();
}

void PlanetSystem::drainReady() {
    std::vector<ReadyChunk> batch;
    {
        std::lock_guard<std::mutex> lk(m_readyMutex);
        while ((int)batch.size() < kUploadBudget && !m_ready.empty()) {
            batch.push_back(std::move(m_ready.front()));
            m_ready.pop_front();
        }
    }
    for (ReadyChunk& rc : batch) {
        if (!rc.verts.empty() && !rc.idx.empty()) {
            auto mesh = std::make_unique<Mesh>();
            mesh->init(*m_ctx, rc.verts, rc.idx);
            m_chunks[rc.key] = std::move(mesh);
        }
        std::lock_guard<std::mutex> lk(m_pendingMutex);
        m_pending.erase(rc.key);
    }
}

void PlanetSystem::evictIfNeeded() {
    if (m_chunks.size() <= kMaxChunks) return;
    // Candidates = chunks not visited this frame, oldest first.
    std::vector<std::pair<uint64_t, uint64_t>> cand;   // (lastUsed, key)
    cand.reserve(m_chunks.size());
    for (auto& [k, mesh] : m_chunks) {
        auto it = m_lastUsed.find(k);
        uint64_t lu = it == m_lastUsed.end() ? 0 : it->second;
        if (lu != m_frame) cand.push_back({lu, k});
    }
    size_t targetSize = (size_t)(kMaxChunks * 0.9);
    if (m_chunks.size() <= targetSize) return;
    size_t toEvict = std::min(cand.size(), m_chunks.size() - targetSize);
    std::partial_sort(cand.begin(), cand.begin() + toEvict, cand.end(),
                      [](auto& a, auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < toEvict; ++i) {
        uint64_t k = cand[i].second;
        auto it = m_chunks.find(k);
        if (it == m_chunks.end()) continue;
        m_trash.push_back({std::move(it->second), m_frame});   // defer the actual free
        m_chunks.erase(it);
        m_lastUsed.erase(k);
    }
}

void PlanetSystem::collectTrash() {
    if (!m_ctx) return;
    VmaAllocator alloc = m_ctx->getAllocator();
    for (size_t i = 0; i < m_trash.size();) {
        if (m_frame - m_trash[i].tag > kTrashDelay) {
            m_trash[i].mesh->cleanup(alloc);
            m_trash[i] = std::move(m_trash.back());
            m_trash.pop_back();
        } else {
            ++i;
        }
    }
}

// ── LOD traversal ─────────────────────────────────────────────────────────────
bool PlanetSystem::shouldSplit(const Node& nd, const glm::vec3& camPos) const {
    if (nd.level >= kMaxLevel) return false;
    float nodeSize = 2.0f / float(1 << nd.level);
    float cu = -1.0f + (nd.i + 0.5f) * nodeSize;
    float cv = -1.0f + (nd.j + 0.5f) * nodeSize;
    glm::vec3 dir     = cubeDir(nd.face, cu, cv);
    glm::vec3 centerW = m_center + dir * (float)((double)m_radius * radiusUnit(m_field->elevation(dir)));
    float worldSize = m_radius * nodeSize;
    float dist      = glm::length(camPos - centerW);

    // Whole-planet OVERVIEW (camera well outside the planet): the visible hemisphere
    // should be crisp despite the large viewing distance, so force it to kOverviewLevel.
    // The hidden back hemisphere stays coarse (level 1) so the chunk budget isn't blown.
    float camAlt = glm::length(camPos - m_center);
    if (camAlt > m_radius * 2.0f) {
        glm::vec3 camDir = glm::normalize(camPos - m_center);
        bool visible = glm::dot(dir, camDir) > -0.2f;          // front hemisphere + a little limb
        return nd.level < (visible ? kOverviewLevel : 1);
    }

    // Near field: force a single uniform level so there are no LOD seams in view. Every
    // chunk closer than kNearUniform subdivides to exactly kUniformLevel and stops — so
    // the whole walkable foreground is one resolution. Detail only changes farther out.
    if (dist < kNearUniform) return nd.level < kUniformLevel;
    // Beyond that, the usual distance-graded LOD (transitions happen out near/past the
    // horizon, where they're not noticeable).
    return worldSize * kSplitFactor > dist;
}

void PlanetSystem::traverse(const Node& nd, const glm::vec3& camPos) {
    Mesh* mesh = getMesh(key(nd));
    if (!mesh) return;                       // invariant: only reached for existing nodes

    m_lastUsed[key(nd)] = m_frame;           // mark the whole active path alive (anti-eviction)

    if (shouldSplit(nd, camPos)) {
        const Node ch[4] = {
            {nd.face, nd.level + 1, nd.i * 2,     nd.j * 2},
            {nd.face, nd.level + 1, nd.i * 2 + 1, nd.j * 2},
            {nd.face, nd.level + 1, nd.i * 2,     nd.j * 2 + 1},
            {nd.face, nd.level + 1, nd.i * 2 + 1, nd.j * 2 + 1},
        };
        bool all = true;
        for (const Node& c : ch) {
            if (!getMesh(key(c))) { requestChunk(c); all = false; }   // stream it in
        }
        if (all) { for (const Node& c : ch) traverse(c, camPos); return; }
    }
    // Camera-relative model (floating origin): world chunk-centre minus camera, in
    // double → small float. Chunk verts are stored relative to that centre, so the
    // GPU only ever sees small numbers. Pure translation → normalMatrix = identity.
    glm::dvec3 t = glm::dvec3(m_center) + centerLocalD(nd) - glm::dvec3(m_camPos);
    glm::mat4  model = glm::translate(glm::mat4(1.0f), glm::vec3(t));
    m_draws.push_back({mesh, model});        // draw this node until its children arrive
}

void PlanetSystem::update(const glm::vec3& camPos) {
    if (!m_active) return;
    m_camPos = camPos;                       // captured for camera-relative chunk models
    ++m_frame;
    drainReady();                            // upload finished chunks (budgeted)

    m_draws.clear();
    for (int face = 0; face < 6; ++face) traverse(Node{face, 0, 0, 0}, camPos);

    evictIfNeeded();
    collectTrash();

    if (m_logStreaming && (uint32_t)m_chunks.size() != m_lastLogged) {
        m_lastLogged = (uint32_t)m_chunks.size();
        LOG_INFO("planet: chunks={} visible={} pending={} trash={}",
                 m_lastLogged, m_draws.size(), m_pending.size(), m_trash.size());
    }
}

} // namespace Nyx
