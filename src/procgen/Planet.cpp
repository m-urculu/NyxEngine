#include "procgen/Planet.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <random>
#include <cmath>
#include <array>
#include <unordered_map>

namespace Nyx::procgen {
namespace {

// ── Seeded Perlin (improved-noise) 3D + fractal sum ─────────────────────────
struct Noise {
    int perm[512];
    explicit Noise(uint32_t seed) {
        int p[256];
        for (int i = 0; i < 256; ++i) p[i] = i;
        std::mt19937 rng(seed);
        for (int i = 255; i > 0; --i) {                 // Fisher–Yates shuffle
            std::uniform_int_distribution<int> d(0, i);
            std::swap(p[i], p[d(rng)]);
        }
        for (int i = 0; i < 512; ++i) perm[i] = p[i & 255];
    }
    static float fade(float t)            { return t * t * t * (t * (t * 6 - 15) + 10); }
    static float lerp(float a, float b, float t) { return a + t * (b - a); }
    static float grad(int h, float x, float y, float z) {
        h &= 15;
        float u = h < 8 ? x : y;
        float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
        return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    }
    float operator()(float x, float y, float z) const {
        int X = (int)std::floor(x) & 255, Y = (int)std::floor(y) & 255, Z = (int)std::floor(z) & 255;
        x -= std::floor(x); y -= std::floor(y); z -= std::floor(z);
        float u = fade(x), v = fade(y), w = fade(z);
        int A = perm[X] + Y, AA = perm[A] + Z, AB = perm[A + 1] + Z;
        int B = perm[X + 1] + Y, BA = perm[B] + Z, BB = perm[B + 1] + Z;
        return lerp(lerp(lerp(grad(perm[AA],   x,     y,     z),
                              grad(perm[BA],   x - 1, y,     z), u),
                         lerp(grad(perm[AB],   x,     y - 1, z),
                              grad(perm[BB],   x - 1, y - 1, z), u), v),
                    lerp(lerp(grad(perm[AA+1], x,     y,     z - 1),
                              grad(perm[BA+1], x - 1, y,     z - 1), u),
                         lerp(grad(perm[AB+1], x,     y - 1, z - 1),
                              grad(perm[BB+1], x - 1, y - 1, z - 1), u), v), w);
    }
};

// Fractal sum (fbm). Returns roughly [-1, 1].
float fbm(const Noise& n, glm::vec3 p, int octaves, float lacunarity = 2.0f, float gain = 0.5f) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum  += amp * n(p.x * freq, p.y * freq, p.z * freq);
        norm += amp;
        amp  *= gain;
        freq *= lacunarity;
    }
    return sum / norm;
}

glm::vec3 lerp3(const glm::vec3& a, const glm::vec3& b, float t) { return a + (b - a) * glm::clamp(t, 0.0f, 1.0f); }

// ── Icosphere ───────────────────────────────────────────────────────────────
// Build an icosahedron, then subdivide each face into 4 `subdiv` times, sharing
// midpoint vertices via an edge cache. Returns unit-length directions.
void buildIcosphere(int subdiv, std::vector<glm::vec3>& verts, std::vector<glm::uvec3>& tris) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    verts = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1},
    };
    for (auto& v : verts) v = glm::normalize(v);
    tris = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1},
    };

    for (int s = 0; s < subdiv; ++s) {
        std::unordered_map<uint64_t, uint32_t> midCache;
        auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
            uint64_t key = (uint64_t)std::min(a, b) << 32 | std::max(a, b);
            auto it = midCache.find(key);
            if (it != midCache.end()) return it->second;
            glm::vec3 m = glm::normalize((verts[a] + verts[b]) * 0.5f);
            uint32_t idx = (uint32_t)verts.size();
            verts.push_back(m);
            midCache[key] = idx;
            return idx;
        };
        std::vector<glm::uvec3> next;
        next.reserve(tris.size() * 4);
        for (const glm::uvec3& f : tris) {
            uint32_t a = midpoint(f.x, f.y), b = midpoint(f.y, f.z), c = midpoint(f.z, f.x);
            next.push_back({f.x, a, c});
            next.push_back({f.y, b, a});
            next.push_back({f.z, c, b});
            next.push_back({a, b, c});
        }
        tris.swap(next);
    }
}

} // namespace

void makePlanet(std::vector<Vertex>& outVerts, std::vector<uint32_t>& outIdx,
                uint32_t seed, int subdivisions) {
    subdivisions = glm::clamp(subdivisions, 1, 7);
    Noise noise(seed);

    // A seed-derived sample offset so two seeds never share the lattice origin.
    std::mt19937 rng(seed ^ 0x9E3779B9u);
    std::uniform_real_distribution<float> off(-1000.0f, 1000.0f);
    glm::vec3 nOff{off(rng), off(rng), off(rng)};

    std::vector<glm::vec3>  dir;     // unit sphere directions
    std::vector<glm::uvec3> tris;
    buildIcosphere(subdivisions, dir, tris);

    // ── Terrain shaping constants ──
    const float baseFreq   = 1.7f;    // continent scale
    const int   octaves    = 7;
    const float seaLevel   = -0.06f;  // noise value below which it's ocean (~45% water)
    const float landHeight = 0.085f;  // peaks rise this fraction of the radius above sea
    const float seaRadius  = 1.0f;

    // Biome palette (authored sRGB; the shader converts to linear).
    const glm::vec3 deepOcean   {0.03f, 0.09f, 0.27f};
    const glm::vec3 shallowOcean{0.10f, 0.36f, 0.52f};
    const glm::vec3 beach       {0.78f, 0.71f, 0.50f};
    const glm::vec3 plains      {0.27f, 0.52f, 0.24f};
    const glm::vec3 hills       {0.32f, 0.40f, 0.20f};
    const glm::vec3 rock        {0.40f, 0.38f, 0.36f};
    const glm::vec3 snow        {0.93f, 0.94f, 0.96f};

    outVerts.resize(dir.size());
    for (size_t i = 0; i < dir.size(); ++i) {
        const glm::vec3 d = dir[i];
        float e = fbm(noise, d * baseFreq + nOff, octaves);          // ~[-1,1]

        glm::vec3 pos;
        glm::vec3 col;
        if (e < seaLevel) {
            // Ocean: flat surface at the sea radius, colour by depth.
            pos = d * seaRadius;
            float depth = glm::clamp((seaLevel - e) / 0.5f, 0.0f, 1.0f);
            col = lerp3(shallowOcean, deepOcean, depth);
        } else {
            // Land: rise above sea level. Sharpen the high end so plains stay
            // broad and mountains spike up.
            float land = (e - seaLevel) / (1.0f - seaLevel);          // [0,1]
            float shaped = std::pow(land, 1.5f);
            pos = d * (seaRadius + shaped * landHeight);

            float h = shaped;                                          // 0 = coast, 1 = highest
            if      (h < 0.04f) col = beach;
            else if (h < 0.30f) col = lerp3(plains, hills, (h - 0.04f) / 0.26f);
            else if (h < 0.60f) col = lerp3(hills,  rock,  (h - 0.30f) / 0.30f);
            else                col = lerp3(rock,   snow,  (h - 0.60f) / 0.40f);
        }

        // Polar ice caps: blend toward snow near the poles (cheap latitude term).
        float lat = std::abs(d.y);
        if (lat > 0.82f) col = lerp3(col, snow, (lat - 0.82f) / 0.18f);

        outVerts[i].position = pos;
        outVerts[i].color    = col;
        outVerts[i].normal   = glm::vec3(0.0f);   // accumulated below
        outVerts[i].texCoord = glm::vec2(0.0f);
    }

    // Indices + smooth normals (area-weighted face normals accumulated per vertex).
    outIdx.clear();
    outIdx.reserve(tris.size() * 3);
    for (const glm::uvec3& f : tris) {
        outIdx.push_back(f.x); outIdx.push_back(f.y); outIdx.push_back(f.z);
        const glm::vec3& p0 = outVerts[f.x].position;
        const glm::vec3& p1 = outVerts[f.y].position;
        const glm::vec3& p2 = outVerts[f.z].position;
        glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);   // unnormalised → area-weighted
        outVerts[f.x].normal += fn;
        outVerts[f.y].normal += fn;
        outVerts[f.z].normal += fn;
    }
    for (Vertex& v : outVerts) {
        v.normal = (glm::dot(v.normal, v.normal) > 1e-12f) ? glm::normalize(v.normal)
                                                           : glm::normalize(v.position);
    }
}

} // namespace Nyx::procgen
