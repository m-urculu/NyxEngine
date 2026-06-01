#pragma once

// NameComponent — user-editable label shown in the SceneHierarchy and Inspector.
// Absent on most entities; SceneHierarchy falls back to a kind-derived label
// ("Mesh N" / "Group N" / "Point Light" / ...) when this component is missing.
// Persisted as "name <text>" in the .scene file (text runs to end of line so
// spaces are fine).

#include <string>

namespace Nyx {

struct NameComponent {
    std::string name;
};

} // namespace Nyx
