#pragma once

// Camera.h — FPS camera with WASD + mouse look
//
// Uses Vulkan's coordinate system: Y-axis flipped in projection matrix.

#include <glm/glm.hpp>

namespace Nyx {

class Camera {
public:
    void init(glm::vec3 position, float aspectRatio);

    void update(float deltaTime);

    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjectionMatrix() const;
    glm::vec3 getPosition() const { return m_position; }
    glm::vec3 getFront()    const { return m_front; }

    // Turntable orbit around a pivot (middle-mouse drag). Rotates the camera
    // rigidly about the pivot so the pivot stays put on screen — no view snap.
    void orbit(const glm::vec3& pivot, float dx, float dy);

    // Dolly toward / away from a pivot (scroll wheel). steps>0 zooms in; the
    // step size scales with distance so it's smooth near and far.
    void dolly(float steps, const glm::vec3& pivot);

    void setAspectRatio(float aspectRatio);
    void  setFov(float fovDegrees) { m_fov = fovDegrees; }   // proj rebuilt from m_fov each frame
    float getFov() const { return m_fov; }

private:
    glm::vec3 m_position = {0.0f, 0.0f, 3.0f};
    float m_yaw   = -90.0f;   // Looking along -Z
    float m_pitch = 0.0f;

    float m_speed       = 3.0f;
    float m_sensitivity = 0.1f;
    float m_fov         = 45.0f;
    float m_nearPlane   = 0.1f;
    float m_farPlane    = 100.0f;
    float m_aspectRatio = 16.0f / 9.0f;

    glm::vec3 m_front = {0.0f, 0.0f, -1.0f};
    glm::vec3 m_up    = {0.0f, 1.0f, 0.0f};
    glm::vec3 m_right = {1.0f, 0.0f, 0.0f};

    void updateVectors();
};

} // namespace Nyx
