// GameScriptDefault.cpp — default no-op gameplay hooks.
//
// The engine declares Nyx::game::onSpawn / update (GameScript.h) and calls them,
// but a PROJECT is expected to DEFINE them in projects/<NYX_PROJECT>/scripts/*.cpp.
// A stock engine (or any project that ships no scripts/) has no such definition,
// which would fail to link. This file supplies inert defaults and is compiled in
// ONLY when the active project provides no scripts — see NYX_HAS_GAME_SCRIPTS in
// CMakeLists.txt. When a project does ship scripts/, those override these.

#include "game/GameScript.h"

namespace Nyx {
namespace game {

void onSpawn(GameContext&) {}
void update(GameContext&)  {}

} // namespace game
} // namespace Nyx
