#pragma once

// Entity.h — Entity type for ECS
//
// Entity = lightweight uint32_t handle. NULL_ENTITY marks invalid/empty.

#include <cstdint>
#include <limits>

namespace Talos {

using Entity = uint32_t;
constexpr Entity NULL_ENTITY = std::numeric_limits<uint32_t>::max();

} // namespace Talos
