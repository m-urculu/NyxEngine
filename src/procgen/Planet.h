#pragma once

// Planet.h — procedural planet mesh generator.
//
// Builds an icosphere, displaces it by multi-octave Perlin (fractal) noise to
// form terrain, clamps everything below sea level to a flat ocean surface, and
// writes per-vertex biome colours (ocean / beach / plains / hills / rock / snow,
// plus polar caps). The result is a normal Vertex mesh — it renders through the
// regular PBR pipeline (the fragment shader multiplies albedo by vertex colour,
// so the biomes show up lit + shadowed with no shader changes). Rivers/erosion
// are intentionally left for a follow-up.

#include "renderer/Vertex.h"

#include <vector>
#include <cstdint>

namespace Nyx::procgen {

// Generate a planet into outVerts/outIdx. `seed` varies the terrain (same seed →
// same planet, so it round-trips through scene save/load). `subdivisions` sets
// the icosphere detail: 20 * 4^subdivisions triangles (5 ≈ 20k tris). The mesh is
// roughly unit radius (sea level = 1.0, peaks a little above); scale the entity to
// taste.
void makePlanet(std::vector<Vertex>& outVerts, std::vector<uint32_t>& outIdx,
                uint32_t seed, int subdivisions = 5);

} // namespace Nyx::procgen
