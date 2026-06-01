#pragma once

// Camera.h — FPS camera with WASD + mouse look
//
// Uses Vulkan's coordinate system: Y-axis flipped in projection matrix.

#include <glm/glm.hpp>

namespace Nyx {

class Camera {
public:
    void init(glm::vec3 position, float aspectRatio);

    // WASD fly. flyPivot is the focus point (selected object's centre, or a point
    // ahead of the camera when nothing is selected); fly speed scales with the
    // distance to it so movement feels right at any zoom level.
    void update(float deltaTime, const glm::vec3& flyPivot);

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

    // Pose accessors so the editor can persist camera state across sessions.
    void  setPose(glm::vec3 position, float yawDegrees, float pitchDegrees) {
        m_position = position; m_yaw = yawDegrees; m_pitch = pitchDegrees;
        updateVectors();
    }
    float getYaw()   const { return m_yaw; }
    float getPitch() const { return m_pitch; }

    // Reposition along the current view direction so a sphere of `radius`
    // around `center` fits comfortably in the FOV. Yaw / pitch unchanged.
    void frame(const glm::vec3& center, float radius);

private:
    glm::vec3 m_position = {0.0f, 0.0f, 3.0f};
    float m_yaw   = -90.0f;   // Looking along -Z
    float m_pitch = 0.0f;

    float m_sensitivity = 0.1f;
    float m_fov         = 45.0f;
    // Dynamic WASD fly speed: scales with distance to the focus pivot (selected
    // object, or a point ahead when nothing is selected) so it's fast when zoomed
    // out and slow when zoomed in — the standard DCC feel. Clamped to a usable
    // band so it never crawls up close or rockets off when way out.
    float m_flySpeedPerDist = 0.5f;    // units/sec per unit of pivot distance
    float m_minFlySpeed     = 0.3f;
    float m_maxFlySpeed     = 50.0f;
    // Wider depth range than the typical 0.1..100 so the screen-constant
    // light/direction gizmos don't clip when zoomed in tight, and so distant
    // geometry isn't clipped by an "invisible wall" when the user pulls way out.
    // Depth is standard Vulkan [0,1] (GLM_FORCE_DEPTH_ZERO_TO_ONE), backed by a
    // D32_SFLOAT buffer — float depth keeps far-distance precision usable across
    // this large range. (A true reverse-Z setup would be the way to push the far
    // plane to infinity; not currently wired up.)
    float m_nearPlane   = 0.01f;
    float m_farPlane    = 10000.0f;
    float m_aspectRatio = 16.0f / 9.0f;

    glm::vec3 m_front = {0.0f, 0.0f, -1.0f};
    glm::vec3 m_up    = {0.0f, 1.0f, 0.0f};
    glm::vec3 m_right = {1.0f, 0.0f, 0.0f};

    void updateVectors();
};

} // namespace Nyx
