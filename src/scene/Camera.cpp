#include "scene/Camera.h"
#include "Input.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

namespace Nyx {

void Camera::init(glm::vec3 position, float aspectRatio) {
    m_position = position;
    m_aspectRatio = aspectRatio;
    updateVectors();
}

void Camera::update(float deltaTime) {
    // Rotation is handled by orbit() (middle-mouse drag); update() only flies the
    // camera with WASD. Suppressed while a sub-window (editor/hierarchy/content
    // browser/console) holds focus, so panel shortcuts don't also fly the camera.
    if (Input::cameraMovementSuppressed()) return;

    // Ctrl+<key> is always a shortcut (Ctrl+S save, Ctrl+Z undo, Ctrl+Shift+Z redo,
    // Ctrl+C/V/X clipboard). If Ctrl is held, the user is composing a shortcut and
    // doesn't want Shift→down or W/A/S/D firing at the same time.
    if (Input::isKeyDown(GLFW_KEY_LEFT_CONTROL) || Input::isKeyDown(GLFW_KEY_RIGHT_CONTROL))
        return;

    float velocity = m_speed * deltaTime;

    if (Input::isKeyDown(GLFW_KEY_W)) m_position += m_front * velocity;
    if (Input::isKeyDown(GLFW_KEY_S)) m_position -= m_front * velocity;
    if (Input::isKeyDown(GLFW_KEY_A)) m_position -= m_right * velocity;
    if (Input::isKeyDown(GLFW_KEY_D)) m_position += m_right * velocity;
    if (Input::isKeyDown(GLFW_KEY_SPACE))       m_position += m_up * velocity;
    if (Input::isKeyDown(GLFW_KEY_LEFT_SHIFT))  m_position -= m_up * velocity;
}

void Camera::orbit(const glm::vec3& pivot, float dx, float dy) {
    // Rotation angles. Signs chosen to match the old drag feel: drag right turns
    // the view right; drag down looks down. The position revolves the same way so
    // the pivot stays fixed on screen.
    float yawAxis   = -dx * m_sensitivity;   // about world-up
    float pitchAxis = -dy * m_sensitivity;   // about the camera's right axis

    // Limit pitch so the view can't roll over the poles (matches free-look clamp).
    float newPitch = std::clamp(m_pitch + pitchAxis, -89.0f, 89.0f);
    pitchAxis = newPitch - m_pitch;

    glm::quat R = glm::angleAxis(glm::radians(yawAxis),   glm::vec3(0.0f, 1.0f, 0.0f))
                * glm::angleAxis(glm::radians(pitchAxis), m_right);

    // Revolve the camera position rigidly around the pivot.
    m_position = pivot + R * (m_position - pivot);

    // Rotate the look direction by the same amount, then re-derive yaw/pitch so
    // subsequent WASD / orbiting stays consistent.
    glm::vec3 f = glm::normalize(R * m_front);
    m_yaw   = glm::degrees(std::atan2(f.z, f.x));
    m_pitch = glm::degrees(std::asin(std::clamp(f.y, -1.0f, 1.0f)));
    updateVectors();
}

void Camera::dolly(float steps, const glm::vec3& pivot) {
    glm::vec3 toPivot = pivot - m_position;
    float dist = glm::length(toPivot);
    if (dist < 1e-4f) return;
    glm::vec3 dir = toPivot / dist;

    // Exponential zoom: each scroll step scales the distance by a constant factor,
    // so the absolute move shrinks naturally as you approach. At dist=10 one step
    // closes ~1.5 units; at dist=2 it closes ~0.3; at dist=0.5 it closes ~0.07.
    // This is the standard DCC-tool feel (Blender/Maya/Unity).
    constexpr float ZOOM_PER_STEP = 0.85f;       // <1 → step toward pivot; ~15% per notch
    float factor = std::pow(ZOOM_PER_STEP, steps);
    float newDist = dist * factor;

    // Don't cross past (or land on) the pivot when zooming in.
    constexpr float MIN_PIVOT_DIST = 0.05f;
    if (newDist < MIN_PIVOT_DIST) newDist = MIN_PIVOT_DIST;

    m_position = pivot - dir * newDist;
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(m_position, m_position + m_front, m_up);
}

glm::mat4 Camera::getProjectionMatrix() const {
    glm::mat4 proj = glm::perspective(glm::radians(m_fov), m_aspectRatio, m_nearPlane, m_farPlane);
    // Vulkan Y-flip: GLM was designed for OpenGL where Y is up.
    // In Vulkan, the Y axis is flipped (points down), so we negate Y.
    proj[1][1] *= -1.0f;
    return proj;
}

void Camera::setAspectRatio(float aspectRatio) {
    m_aspectRatio = aspectRatio;
}

void Camera::frame(const glm::vec3& center, float radius) {
    // Distance that fits a `radius` sphere within the vertical FOV with a
    // small margin so the object isn't pressed against the edge of the view.
    // For wide aspect ratios this is conservative (horizontal FOV is larger),
    // which is what we want — guaranteed to fit on any screen.
    float r       = std::max(0.05f, radius);
    float halfFov = glm::radians(m_fov * 0.5f);
    float dist    = (r / std::sin(halfFov)) * 1.25f;
    m_position    = center - m_front * dist;
}

void Camera::updateVectors() {
    glm::vec3 front;
    front.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    front.y = sin(glm::radians(m_pitch));
    front.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    m_front = glm::normalize(front);

    m_right = glm::normalize(glm::cross(m_front, glm::vec3(0.0f, 1.0f, 0.0f)));
    m_up    = glm::normalize(glm::cross(m_right, m_front));
}

} // namespace Nyx
