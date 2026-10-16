// Camera.cpp
#include <radiance4d/scene/Camera.hpp>

#include <algorithm>
#include <cmath>

namespace r4d
{
    namespace
    {
        // Just short of straight up or down, so the view basis never degenerates.
        constexpr float kPitchLimit = 1.55334f;
    } // namespace

    Vec3 Camera::forward() const
    {
        // Left-handed, +z forward at yaw 0.
        return Vec3{std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                    std::cos(m_pitch) * std::cos(m_yaw)};
    }

    Vec3 Camera::position() const
    {
        return m_mode == CameraMode::Orbit ? m_target - forward() * m_distance
                                           : m_position;
    }

    Mat4 Camera::view() const
    {
        const Vec3 eye = position();
        return glm::lookAt(eye, eye + forward(), Vec3{0.0f, 1.0f, 0.0f});
    }

    Mat4 Camera::projection() const
    {
        return glm::perspective(degreesToRadians(m_fovDegrees), m_aspect, m_near, m_far);
    }

    void Camera::focusOn(Vec3 centre, float radius)
    {
        m_target = centre;
        const float safeRadius = std::max(radius, 0.001f);
        m_distance = safeRadius / std::tan(degreesToRadians(m_fovDegrees) * 0.5f) * 1.3f;
        m_near = std::max(m_distance * 0.001f, 0.01f);
        m_far = m_distance * 20.0f + safeRadius * 10.0f;
        m_position = m_target - forward() * m_distance;
    }

    void Camera::setAngles(float yawDegrees, float pitchDegrees)
    {
        m_yaw = degreesToRadians(yawDegrees);
        m_pitch = std::clamp(degreesToRadians(pitchDegrees), -kPitchLimit, kPitchLimit);
        m_position = m_target - forward() * m_distance;
    }

    float Camera::yawDegrees() const
    {
        return radiansToDegrees(m_yaw);
    }

    float Camera::pitchDegrees() const
    {
        return radiansToDegrees(m_pitch);
    }

    void Camera::update(const InputState& input, double deltaSeconds)
    {
        const float dt = static_cast<float>(deltaSeconds);
        const Vec2 mouseDelta = input.mouseDelta();
        const float wheel = input.wheelDelta();

        if (m_mode == CameraMode::Orbit)
        {
            if (input.mouseDown(MouseButton::Left))
            {
                // The drag grabs the subject, as the pitch and the pan below do:
                // it turns with the cursor, not against it.
                m_yaw += mouseDelta.x * m_orbitSpeed;
                m_pitch = std::clamp(m_pitch - mouseDelta.y * m_orbitSpeed, -kPitchLimit,
                                     kPitchLimit);
            }

            if (input.mouseDown(MouseButton::Right) ||
                input.mouseDown(MouseButton::Middle))
            {
                // Pan scales with distance so the grab point tracks the cursor.
                const Vec3 right =
                    glm::normalize(glm::cross(Vec3{0.0f, 1.0f, 0.0f}, forward()));
                const Vec3 up = glm::cross(forward(), right);
                const float scale = m_panSpeed * m_distance;
                m_target += right * (-mouseDelta.x * scale) + up * (mouseDelta.y * scale);
            }

            if (wheel != 0.0f)
            {
                // Multiplicative, so each notch is the same proportional step.
                m_distance =
                    std::max(m_distance * std::pow(1.0f - m_zoomSpeed, wheel), 0.01f);
            }

            m_position = m_target - forward() * m_distance;
        }
        else
        {
            if (input.mouseDown(MouseButton::Left) || input.mouseDown(MouseButton::Right))
            {
                m_yaw += mouseDelta.x * m_orbitSpeed;
                m_pitch = std::clamp(m_pitch - mouseDelta.y * m_orbitSpeed, -kPitchLimit,
                                     kPitchLimit);
            }

            const Vec3 ahead = forward();
            const Vec3 right = glm::normalize(glm::cross(Vec3{0.0f, 1.0f, 0.0f}, ahead));

            Vec3 motion{0.0f, 0.0f, 0.0f};
            if (input.down(Key::W))
                motion += ahead;
            if (input.down(Key::S))
                motion -= ahead;
            if (input.down(Key::D))
                motion += right;
            if (input.down(Key::A))
                motion -= right;
            if (input.down(Key::E) || input.down(Key::Space))
                motion += Vec3{0.0f, 1.0f, 0.0f};
            if (input.down(Key::Q) || input.down(Key::Control))
                motion -= Vec3{0.0f, 1.0f, 0.0f};

            if (glm::dot(motion, motion) > 0.0f)
            {
                const float speed = m_moveSpeed * (input.down(Key::Shift) ? 4.0f : 1.0f);
                m_position += glm::normalize(motion) * speed * dt;
            }

            if (wheel != 0.0f)
            {
                m_moveSpeed =
                    std::clamp(m_moveSpeed * std::pow(1.1f, wheel), 0.05f, 500.0f);
            }

            m_target = m_position + ahead * m_distance;
        }
    }

} // namespace r4d
