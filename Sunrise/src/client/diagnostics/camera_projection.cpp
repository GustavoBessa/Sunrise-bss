/**
 * Turns a world position into a point on screen, using the camera pose the teleport hook resolves.
 *
 * The basis is built from the forward vector and world up rather than read from the game, because the
 * camera block exposes position and forward and nothing else. That costs the roll: a camera rolled
 * about its own forward axis would put the marker at the right place with the wrong tilt. Nothing in
 * ordinary play rolls the view, so the trade buys a marker with no further reverse engineering.
 */

#include "camera_projection.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "../hooks/teleport/runtime.h"

namespace sunrise::client::diagnostics::camera_projection {
namespace {

using Vector = std::array<float, 3>;

/** Only the presentation thread reads or writes this, as the HUD does with its own switches. */
float g_fieldOfView{kDefaultFieldOfView};

[[nodiscard]] float dot(const Vector& left, const Vector& right) noexcept {
    return (left[0] * right[0]) + (left[1] * right[1]) + (left[2] * right[2]);
}

[[nodiscard]] Vector cross(const Vector& left, const Vector& right) noexcept {
    return {(left[1] * right[2]) - (left[2] * right[1]),
            (left[2] * right[0]) - (left[0] * right[2]),
            (left[0] * right[1]) - (left[1] * right[0])};
}

/** @return The vector scaled to unit length, or zeroes when it has no length to scale. */
[[nodiscard]] Vector normalized(const Vector& value) noexcept {
    const float length = std::sqrt(dot(value, value));
    if (!(length > 0.0F)) {
        // Also catches a non-finite lane: every comparison against one is false.
        return {};
    }
    return {value[0] / length, value[1] / length, value[2] / length};
}

[[nodiscard]] Vector difference(const Vector& left, const Vector& right) noexcept {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

/** @return Degrees for one angle in radians. */
[[nodiscard]] float degrees(float radians) noexcept {
    return radians * (180.0F / std::numbers::pi_v<float>);
}

} // namespace

/** Projects one world position onto the current frame. */
bool project(const std::array<float, 3>& world,
             float viewportWidth,
             float viewportHeight,
             Projected& output) noexcept {
    output = {};
    Vector camera{};
    Vector forward{};
    if (!hooks::teleport::current_camera_pose(camera, forward)
        || !(viewportWidth > 0.0F) || !(viewportHeight > 0.0F)) {
        return false;
    }
    const Vector view = normalized(forward);
    // A forward vector parallel to world up leaves no horizontal axis to build the basis on.
    const Vector right = normalized(cross(view, kWorldUp));
    if (dot(view, view) <= 0.0F || dot(right, right) <= 0.0F) {
        return false;
    }
    const Vector up = cross(right, view);

    const Vector delta = difference(world, camera);
    const float depth = dot(delta, view);
    const float lateral = dot(delta, right);
    const float vertical = dot(delta, up);
    output.depth = depth;
    output.range = std::sqrt(dot(delta, delta));
    // Bearing is filled whether or not the point projects, because an off-screen marker still has to
    // say which way to turn. Measured in the horizontal plane, so a point overhead reads as ahead.
    output.bearing = degrees(std::atan2(lateral, depth));
    output.valid = true;
    if (!(depth > kNearPlane)) {
        return true;
    }

    // Half-height of the frustum at unit depth. The horizontal half-width follows from the aspect
    // ratio, so one field of view covers both axes.
    const float halfFov = (g_fieldOfView * 0.5F) * (std::numbers::pi_v<float> / 180.0F);
    const float tangent = std::tan(halfFov);
    if (!(tangent > 0.0F)) {
        return true;
    }
    const float aspect = viewportWidth / viewportHeight;
    const float normalizedX = lateral / (depth * tangent * aspect);
    // Screen y grows downwards while world up does not, so the vertical axis is inverted here.
    const float normalizedY = -vertical / (depth * tangent);
    output.x = (normalizedX + 1.0F) * 0.5F * viewportWidth;
    output.y = (normalizedY + 1.0F) * 0.5F * viewportHeight;
    output.onScreen = output.x >= 0.0F && output.x <= viewportWidth && output.y >= 0.0F
                      && output.y <= viewportHeight;
    return true;
}

/** Reports the vertical field of view the projection assumes. */
float field_of_view() noexcept {
    return g_fieldOfView;
}

/** Replaces the vertical field of view the projection assumes. */
void set_field_of_view(float degrees) noexcept {
    if (!(degrees > 0.0F)) {
        // A non-finite or non-positive value would divide the projection by zero.
        return;
    }
    g_fieldOfView = (std::clamp)(degrees, kMinimumFieldOfView, kMaximumFieldOfView);
}

} // namespace sunrise::client::diagnostics::camera_projection
