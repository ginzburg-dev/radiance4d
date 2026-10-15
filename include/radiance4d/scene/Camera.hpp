// Camera.hpp
#pragma once

#include <radiance4d/core/Input.hpp>
#include <radiance4d/core/Math.hpp>

#include <string_view>

namespace r4d
{

    enum class CameraMode
    {
        Orbit,
        FreeFly,
    };

    constexpr std::string_view ToString(CameraMode mode)
    {
        switch (mode)
        {
            case CameraMode::Orbit:   return "Orbit";
            case CameraMode::FreeFly: return "FreeFly";
        }
        return "Unknown";
    }

    class Camera
    {
    public:
        void update(const InputState& input, double deltaSeconds);

        void setAspect(float aspect) { m_aspect = aspect; }
        void setMode(CameraMode mode) { m_mode = mode; }
        CameraMode mode() const { return m_mode; }

        // Frames the camera on a bounding sphere.
        void focusOn(Vec3 centre, float radius);

        // Aims the camera, in degrees: yaw turns it around the subject, pitch
        // lifts it above. The viewer exposes this so that a capture of a scene
        // whose default direction looks into clutter is still reproducible from
        // a command line.
        void setAngles(float yawDegrees, float pitchDegrees);

        float yawDegrees() const;
        float pitchDegrees() const;

        Mat4 view() const;
        Mat4 projection() const;
        Mat4 viewProjection() const { return projection() * view(); }

        Vec3 position() const;
        Vec3 target() const { return m_target; }

        float nearPlane() const { return m_near; }
        float farPlane() const { return m_far; }
        float fovDegrees() const { return m_fovDegrees; }

    private:
        Vec3 forward() const;

        CameraMode m_mode = CameraMode::Orbit;

        Vec3 m_target{0.0f, 0.0f, 0.0f};
        float m_distance = 3.0f;

        // Free-fly position, kept in sync when switching modes.
        Vec3 m_position{0.0f, 0.0f, -3.0f};

        float m_yaw = 0.0f;
        float m_pitch = 0.0f;

        float m_aspect = 16.0f / 9.0f;
        float m_fovDegrees = 60.0f;
        float m_near = 0.05f;
        float m_far = 500.0f;

        float m_moveSpeed = 3.0f;
        float m_orbitSpeed = 0.006f;
        float m_panSpeed = 0.002f;
        float m_zoomSpeed = 0.12f;
    };

} // namespace r4d
