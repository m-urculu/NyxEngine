#pragma once

// GameScript.h — the boundary between the ENGINE (runtime/machinery) and a
// PROJECT's gameplay (controls, character behaviour, rules).
//
// Everything "in-game" is a PROJECT script: files in projects/<NYX_PROJECT>/scripts/
// *.cpp compiled into the build (Nyx has no runtime C++ scripting — same model as
// procgen). The engine owns rendering, input, the ECS, and the planet streaming; it
// builds a GameContext each frame and calls the project-defined hooks below. The
// project moves the player, drives the camera, etc., through that context.
//
// A project provides these by DEFINING Nyx::game::onSpawn / update (the engine only
// declares them); the active project (NYX_PROJECT) is expected to ship scripts/, the
// same way it ships procgen/.

#include <glm/glm.hpp>

namespace Nyx {

class Camera;
class PlanetSystem;

// Player/character state owned by the engine, evolved by the project script. The
// engine renders the avatar at {pos, forward, up} after each update().
struct PlayerState {
    glm::vec3 pos     {0.0f};            // feet position (world)
    glm::vec3 forward {0.0f, 0.0f, 1.0f};
    glm::vec3 up      {0.0f, 1.0f, 0.0f};
    float heading  = 0.0f;               // degrees, yaw about the local up
    float pitch    = 0.0f;               // degrees, camera tilt
    float vertVel  = 0.0f;               // radial velocity (gravity/jump)
    bool  grounded = true;
    bool  firstLook = true;              // swallow the first captured-mouse delta
    float camDist  = 14.0f;              // third-person boom length (scroll wheel adjusts)
    float camDistCur = 14.0f;            // smoothed actual boom length (spring-arm easing)
};

// Everything a gameplay script needs for one frame. Input is reached via the
// engine's Input:: static API directly (scripts include "Input.h").
struct GameContext {
    float          dt        = 0.0f;
    Camera*        camera    = nullptr;  // setBasis / getPosition / getFront
    PlanetSystem*  planet    = nullptr;  // surfaceDistance / center / radius
    PlayerState*   player    = nullptr;  // read + write
    bool           cursorCaptured = false;  // mouse-look active (free-look)
    bool           walking   = false;    // true = surface walk; false = pre-walk planet overview
    bool           requestRegen = false; // project sets true → engine rerolls the world (new seed)
};

namespace game {
    // Called when the player enters surface-exploration: place them on the surface.
    void onSpawn(GameContext& ctx);
    // Called every fixed tick in play mode once a planet exists — in BOTH the surface-walk
    // state (ctx.walking == true) and the pre-walk overview (false). Branch on ctx.walking:
    // walking → input drives player + camera; overview → e.g. Space sets ctx.requestRegen.
    void update(GameContext& ctx);
}

} // namespace Nyx
