#pragma once

// EnvironmentComponent.h — Per-scene aesthetic rendering settings.
//
// One singleton entity per scene holds this component (the "Environment" entity,
// shown at the top of the Scene Hierarchy with a 🌍 icon). It owns the values
// that define how the scene LOOKS — sky gradient (drives the skybox + IBL),
// ambient floor, bloom shape, exposure, tonemap choice — as opposed to hardware
// quality settings (shadow map resolution, MSAA, render scale) which live in a
// separate project-wide settings file.
//
// Mirrors the Godot WorldEnvironment / Unity Volume Profile pattern: scene-local
// aesthetic, edited in the Inspector like any other component, round-trips
// through the .scene file.

#include <glm/glm.hpp>
#include <cstdint>

namespace Nyx {

struct EnvironmentComponent {
    // ── Sky / IBL — drives the procedural skybox AND the in-shader image-based
    //    lighting. .w on horizon doubles as the overall sky-intensity multiplier.
    //    Neutral grayscale dome by default (white zenith → gray horizon → black
    //    ground), so a new scene is lit by a colour-free environment light only.
    glm::vec3 skyTop      = {1.00f, 1.00f, 1.00f};   // white zenith
    glm::vec3 skyHorizon  = {0.50f, 0.50f, 0.50f};   // gray horizon
    glm::vec3 skyGround   = {0.00f, 0.00f, 0.00f};   // black ground
    float     skyIntensity = 0.8f;

    // ── Ambient floor — a flat additive term, mostly relevant where the sky
    //    can't reach (under overhangs etc.). Low by default; the analytic IBL
    //    does most of the indirect lighting now.
    glm::vec3 ambient      = {0.15f, 0.15f, 0.15f};

    // ── Bloom — thresholded brightpass + 5-mip downsample → tent-filtered
    //    upsample → additive composite. Strength is the post-tonemap weight.
    float bloomThreshold   = 0.6f;
    float bloomKnee        = 0.4f;
    float bloomStrength    = 0.08f;

    // ── Tonemap — currently composite.frag hardcodes ACES. Reserved for a
    //    future tonemap-choice + exposure stop control.
    enum class Tonemapper : uint32_t { ACES = 0, Reinhard = 1, None = 2 };
    Tonemapper tonemapper  = Tonemapper::ACES;
    float      exposure    = 0.0f;   // EV stops (0 = neutral)

    // ── Walkable planet (scene world) — when set, this scene IS a planet: the
    //    editor previews it (free-fly) and PLAY mode / the exported game spawn the
    //    character on its surface. Round-trips through the .scene env line.
    bool     planetActive  = false;
    uint32_t planetSeed    = 0;
    float    planetRadius  = 1500.0f;
};

} // namespace Nyx
