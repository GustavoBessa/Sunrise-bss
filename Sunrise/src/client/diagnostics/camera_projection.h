#pragma once

#include <array>
#include <cstddef>

namespace sunrise::client::diagnostics::camera_projection {

/**
 * Turns a world position into a point on screen, using the game's own camera.
 *
 * This is what lets Sunrise mark a place in the world rather than only name it. The game's objective
 * marker system is not reached by anything in this project, so a marker is drawn by Sunrise over the
 * frame instead: the camera pose the teleport hook already resolves is enough to project a point.
 *
 * The one value the game does not hand over is the field of view. It is held here as a setting
 * because it has to match the player's own: a mismatch leaves the marker correct at the centre of the
 * screen and increasingly wrong towards its edges.
 */

/** World basis has X forward and Z up, which is what the camera's default forward of (1,0,0) says. */
inline constexpr std::array<float, 3> kWorldUp{0.0F, 0.0F, 1.0F};
/** The game's default vertical field of view, in degrees. */
inline constexpr float kDefaultFieldOfView = 60.0F;
/** Below this the projection divides by almost nothing and the point runs off to infinity. */
inline constexpr float kMinimumFieldOfView = 30.0F;
/** Above this the frustum is wider than any setting the game offers. */
inline constexpr float kMaximumFieldOfView = 120.0F;
/** Nearer than this along the view axis a point is treated as behind the camera. */
inline constexpr float kNearPlane = 0.05F;

/** One projected world point. */
struct Projected {
    /** Screen position in final framebuffer pixels. Meaningful only when `onScreen`. */
    float x{};
    float y{};
    /** Distance along the view axis, in world units. Negative behind the camera. */
    float depth{};
    /** Straight-line distance from the camera, in world units. */
    float range{};
    /**
     * Horizontal turn towards the point, in degrees: negative to the left, positive to the right.
     * Filled whether or not the point is on screen, so an off-screen marker can still point at it.
     */
    float bearing{};
    /** The point is in front of the camera and inside the viewport. */
    bool onScreen{};
    /** The camera pose was readable, so every other field means something. */
    bool valid{};
};

/**
 * Projects one world position onto the current frame.
 * @param world Position in world units.
 * @param viewportWidth Final framebuffer width in pixels.
 * @param viewportHeight Final framebuffer height in pixels.
 * @param output Receives the projection. Cleared first.
 * @return True when the camera pose was readable.
 */
[[nodiscard]] bool project(const std::array<float, 3>& world,
                           float viewportWidth,
                           float viewportHeight,
                           Projected& output) noexcept;

/** @return The vertical field of view the projection assumes, in degrees. */
[[nodiscard]] float field_of_view() noexcept;

/**
 * Replaces the vertical field of view the projection assumes.
 * @param degrees New value, clamped into the range the game offers.
 */
void set_field_of_view(float degrees) noexcept;

} // namespace sunrise::client::diagnostics::camera_projection
