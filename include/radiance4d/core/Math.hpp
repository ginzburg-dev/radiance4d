// Math.hpp
#pragma once

// Depth range, handedness and matrix layout are set once in the root CMakeLists.
// Shaders are built with dxc -Zpc, so glm::mat4 uploads without a transpose and
// vectors are transformed as mul(M, v).

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace r4d
{
    using Vec2 = glm::vec2;
    using Vec3 = glm::vec3;
    using Vec4 = glm::vec4;
    using IVec2 = glm::ivec2;
    using UVec2 = glm::uvec2;
    using Mat3 = glm::mat3;
    using Mat4 = glm::mat4;
    using Quat = glm::quat;

    inline constexpr float kPi = 3.14159265358979323846f;

    constexpr float degreesToRadians(float degrees) { return degrees * (kPi / 180.0f); }
    constexpr float radiansToDegrees(float radians) { return radians * (180.0f / kPi); }
} // namespace r4d
