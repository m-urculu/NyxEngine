#include "core/Time.h"

#include <algorithm>

namespace Talos {

void Time::init() {
    m_lastTime    = static_cast<float>(glfwGetTime());
    m_deltaTime   = 0.0f;
    m_accumulator = 0.0f;
}

void Time::update() {
    float currentTime = static_cast<float>(glfwGetTime());
    m_deltaTime = currentTime - m_lastTime;
    m_lastTime  = currentTime;

    // Clamp to avoid spiral of death (e.g., after breakpoint or window drag)
    m_deltaTime = std::min(m_deltaTime, 0.25f);

    m_accumulator += m_deltaTime;
}

} // namespace Talos
