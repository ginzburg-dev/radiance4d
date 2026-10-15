// Camera_test.cpp
//
// The viewer aims a capture with --orbit-yaw / --orbit-pitch, so where a given
// pair of angles puts the eye is a documented contract, not an implementation
// detail: a sign flip here would silently change every stored capture command.

#include <radiance4d/scene/Camera.hpp>

#include <gtest/gtest.h>

#include <cmath>

namespace
{
    // The angles only decide a direction, so compare the unit vector from the
    // subject to the eye and leave the framing distance out of it.
    r4d::Vec3 eyeDirection(const r4d::Camera& camera)
    {
        return glm::normalize(camera.position() - camera.target());
    }
} // namespace

TEST(Camera, DefaultAnglesLookDownPositiveZ)
{
    r4d::Camera camera;
    camera.focusOn(r4d::Vec3{0.0f, 0.0f, 0.0f}, 1.0f);

    EXPECT_FLOAT_EQ(camera.yawDegrees(), 0.0f);
    EXPECT_FLOAT_EQ(camera.pitchDegrees(), 0.0f);

    // Left-handed, +z forward at yaw 0, so the eye sits on -z.
    const r4d::Vec3 direction = eyeDirection(camera);
    EXPECT_NEAR(direction.x, 0.0f, 1e-5f);
    EXPECT_NEAR(direction.y, 0.0f, 1e-5f);
    EXPECT_NEAR(direction.z, -1.0f, 1e-5f);
}

TEST(Camera, YawTurnsTheEyeAroundTheSubject)
{
    r4d::Camera camera;
    camera.setAngles(90.0f, 0.0f);
    camera.focusOn(r4d::Vec3{0.0f, 0.0f, 0.0f}, 1.0f);

    EXPECT_FLOAT_EQ(camera.yawDegrees(), 90.0f);

    // Looking along +x puts the eye on -x, at the same height.
    const r4d::Vec3 direction = eyeDirection(camera);
    EXPECT_NEAR(direction.x, -1.0f, 1e-5f);
    EXPECT_NEAR(direction.y, 0.0f, 1e-5f);
    EXPECT_NEAR(direction.z, 0.0f, 1e-5f);
}

TEST(Camera, PitchIsTheViewDirectionNotTheEye)
{
    r4d::Camera camera;
    camera.setAngles(0.0f, 30.0f);
    camera.focusOn(r4d::Vec3{0.0f, 0.0f, 0.0f}, 1.0f);

    EXPECT_FLOAT_EQ(camera.pitchDegrees(), 30.0f);

    // Pitch tilts the view, as the mouse drag does, and the orbit eye sits
    // opposite it: a view pitched up looks at the subject from below. A
    // capture that wants a high camera therefore passes a negative pitch.
    const r4d::Vec3 direction = eyeDirection(camera);
    EXPECT_NEAR(direction.y, -std::sin(r4d::degreesToRadians(30.0f)), 1e-5f);
    EXPECT_LT(camera.position().y, 0.0f);

    r4d::Camera above;
    above.setAngles(0.0f, -30.0f);
    above.focusOn(r4d::Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
    EXPECT_GT(above.position().y, 0.0f);
}

TEST(Camera, PitchIsClampedShortOfStraightDown)
{
    r4d::Camera camera;
    camera.setAngles(0.0f, -400.0f);
    camera.focusOn(r4d::Vec3{0.0f, 0.0f, 0.0f}, 1.0f);

    // Clamped, not wrapped: a straight-down view would degenerate the basis
    // against a fixed +y up vector.
    EXPECT_LT(camera.pitchDegrees(), -88.0f);
    EXPECT_GT(camera.pitchDegrees(), -90.0f);

    const r4d::Vec3 direction = eyeDirection(camera);
    EXPECT_NEAR(glm::length(direction), 1.0f, 1e-5f);
}

TEST(Camera, AnglesDoNotMoveTheSubjectOrTheFramingDistance)
{
    const r4d::Vec3 centre{1.0f, 2.0f, 3.0f};

    r4d::Camera straight;
    straight.focusOn(centre, 2.5f);

    r4d::Camera aimed;
    aimed.setAngles(140.0f, 12.0f);
    aimed.focusOn(centre, 2.5f);

    EXPECT_FLOAT_EQ(aimed.target().x, centre.x);
    EXPECT_FLOAT_EQ(aimed.target().y, centre.y);
    EXPECT_FLOAT_EQ(aimed.target().z, centre.z);

    // Same sphere, same distance back from it: aiming is a rotation only.
    EXPECT_NEAR(glm::length(aimed.position() - centre),
                glm::length(straight.position() - centre), 1e-4f);
    EXPECT_FLOAT_EQ(aimed.nearPlane(), straight.nearPlane());
    EXPECT_FLOAT_EQ(aimed.farPlane(), straight.farPlane());
}
