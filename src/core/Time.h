#pragma once

// Time.h — Fixed-timestep clock utility
//
// Provides a fixed-timestep accumulator for deterministic game logic updates.
// Rendering uses interpolation alpha for smooth visuals between ticks.

#include <GLFW/glfw3.h>

namespace Nyx {

class Time {
public:
    static constexpr float FIXED_DT = 1.0f / 60.0f;

    void init();
    void update();

    bool shouldTick() const { return m_accumulator >= FIXED_DT; }
    void consumeTick() { m_accumulator -= FIXED_DT; }
    float getAlpha() const { return m_accumulator / FIXED_DT; }

    float getDeltaTime() const { return m_deltaTime; }

private:
    float m_lastTime    = 0.0f;
    float m_deltaTime   = 0.0f;
    float m_accumulator = 0.0f;
};

} // namespace Nyx
