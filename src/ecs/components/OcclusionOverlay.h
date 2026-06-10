#pragma once

// OcclusionOverlay.h — tag component. Entities with this are drawn a SECOND time
// after the main mesh pass with the occlusion-overlay pipeline (depth test GREATER,
// no depth write), so they appear as a uniform see-through silhouette wherever they
// are hidden behind other geometry. Used for the third-person player character so it
// stays visible behind terrain without zooming the camera or fading the terrain.

namespace Nyx {

struct OcclusionOverlay {};

} // namespace Nyx
