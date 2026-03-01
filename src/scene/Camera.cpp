#include "scene/Camera.h"
#include "Input.h"

#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include <algorithm>

namespace Talos {

void Camera::init(glm::vec3 position, float aspectRatio) {
    m_position = position;
    m_aspectRatio = aspectRatio;
    updateVectors();
}

void Camera::update(float deltaTime) {
    // Mouse look
    float dx = Input::getMouseDeltaX() * m_sensitivity;
    float dy = Input::getMouseDeltaY() * m_sensitivity;

    m_yaw   += dx;
    m_pitch -= dy;  // Inverted: mouse up = look up

    m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);

    updateVectors();

    // WASD movement
    float velocity = m_speed * deltaTime;

    if (Input::isKeyDown(GLFW_KEY_W)) m_position += m_front * velocity;
    if (Input::isKeyDown(GLFW_KEY_S)) m_position -= m_front * velocity;
    if (Input::isKeyDown(GLFW_KEY_A)) m_position -= m_right * velocity;
    if (Input::isKeyDown(GLFW_KEY_D)) m_position += m_right * velocity;
    if (Input::isKeyDown(GLFW_KEY_SPACE))       m_position += m_up * velocity;
    if (Input::isKeyDown(GLFW_KEY_LEFT_SHIFT))  m_position -= m_up * velocity;
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

void Camera::updateVectors() {
    glm::vec3 front;
    front.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    front.y = sin(glm::radians(m_pitch));
    front.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    m_front = glm::normalize(front);

    m_right = glm::normalize(glm::cross(m_front, glm::vec3(0.0f, 1.0f, 0.0f)));
    m_up    = glm::normalize(glm::cross(m_right, m_front));
}

} // namespace Talos
