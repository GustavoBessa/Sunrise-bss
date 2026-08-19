/**
 * The objective marker: a diamond over the world where the roteiro's next beat is, with its distance
 * under it, and an arrow at the edge of the screen when the beat is behind the player.
 *
 * The game's own objective marker system is not reached by anything in this project, so this is drawn
 * by Sunrise over the frame. It reads the same run state the tracker line does, which is what keeps
 * the two from ever disagreeing about which beat comes next.
 */

#include "ui_hud_objective_overlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "../../../../client/diagnostics/camera_projection.h"
#include "../../../../client/playbook/playbook.h"

namespace sunrise::core::ui::hud::overlays::objective {
namespace {

namespace book = client::playbook;
namespace projection = client::diagnostics::camera_projection;

/** Half-width of the diamond at the near end of the scale, in pixels. */
constexpr float kNearRadius = 14.0F;
/** Half-width at the far end. A marker far off stays visible without covering the world. */
constexpr float kFarRadius = 6.0F;
/** Distance at which the diamond reaches `kFarRadius`, in world units. */
constexpr float kFarRange = 120.0F;
/** Thickness of the diamond's outline. */
constexpr float kOutline = 2.0F;
/** How far inside the viewport the off-screen arrow sits, in pixels. */
constexpr float kEdgeInset = 40.0F;
/** Side of the off-screen arrow, in pixels. */
constexpr float kArrowRadius = 12.0F;
/** Gap between the diamond and the distance under it, in pixels. */
constexpr float kLabelGap = 6.0F;

/** The marker's fill and its outline. Amber, so it reads against the game's own blues and whites. */
constexpr ImU32 kFill = IM_COL32(255, 196, 64, 210);
constexpr ImU32 kEdge = IM_COL32(24, 18, 8, 230);
constexpr ImU32 kText = IM_COL32(255, 226, 170, 240);
constexpr ImU32 kTextShadow = IM_COL32(0, 0, 0, 200);

/** @param range Distance to the beat. @return Half-width the diamond is drawn at. */
[[nodiscard]] float radius_for(float range) noexcept {
    const float weight = (std::clamp)(range / kFarRange, 0.0F, 1.0F);
    return kNearRadius + ((kFarRadius - kNearRadius) * weight);
}

/** Draws the diamond and its outline centred on one point. */
void draw_diamond(ImDrawList& list, const ImVec2& at, float radius) noexcept {
    const std::array<ImVec2, 4> points{ImVec2{at.x, at.y - radius},
                                       ImVec2{at.x + radius, at.y},
                                       ImVec2{at.x, at.y + radius},
                                       ImVec2{at.x - radius, at.y}};
    list.AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), kFill);
    list.AddPolyline(
        points.data(), static_cast<int>(points.size()), kEdge, ImDrawFlags_Closed, kOutline);
}

/** Draws one line of text centred on a point, over a shadow so it reads on any background. */
void draw_centered(ImDrawList& list, const ImVec2& at, const char* text) noexcept {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 origin{at.x - (size.x * 0.5F), at.y};
    list.AddText(ImVec2{origin.x + 1.0F, origin.y + 1.0F}, kTextShadow, text);
    list.AddText(origin, kText, text);
}

/**
 * Draws an arrow at the edge of the viewport pointing the way to a beat that does not project.
 * @param list Foreground draw list.
 * @param viewport Viewport being drawn over.
 * @param bearing Horizontal turn towards the beat, in degrees.
 */
void draw_offscreen(ImDrawList& list, const ImGuiViewport& viewport, float bearing) noexcept {
    const ImVec2 centre{viewport.Pos.x + (viewport.Size.x * 0.5F),
                        viewport.Pos.y + (viewport.Size.y * 0.5F)};
    // Left or right only. The bearing is a horizontal turn, and an arrow that also moved vertically
    // would claim a precision this projection does not have.
    const float direction = bearing < 0.0F ? -1.0F : 1.0F;
    const ImVec2 at{centre.x + (direction * ((viewport.Size.x * 0.5F) - kEdgeInset)), centre.y};
    const std::array<ImVec2, 3> points{
        ImVec2{at.x + (direction * kArrowRadius), at.y},
        ImVec2{at.x - (direction * kArrowRadius), at.y - kArrowRadius},
        ImVec2{at.x - (direction * kArrowRadius), at.y + kArrowRadius}};
    list.AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), kFill);
    list.AddPolyline(
        points.data(), static_cast<int>(points.size()), kEdge, ImDrawFlags_Closed, kOutline);
}

} // namespace

/**
 * Draws the route ahead of the player.
 *
 * The waypoints come from where the player is standing rather than from what has fired, so turning
 * around brings the markers back down the path instead of leaving them at the far end of the run.
 * The first waypoint is the one to head for and is drawn solid; the ones after it trace the line the
 * roteiro takes, drawn smaller so the path reads as a direction and not as four equal objectives.
 */
void draw() noexcept {
    const book::Route route = book::route_ahead();
    if (!route.active || route.count == 0) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* list = ImGui::GetForegroundDrawList();
    if (viewport == nullptr || list == nullptr) {
        return;
    }

    // Projected first, then drawn, so the line joining them can be laid under every diamond.
    std::array<projection::Projected, book::kRouteAheadCapacity> points{};
    for (std::size_t index = 0; index < route.count; ++index) {
        if (!projection::project(route.ahead[index].position,
                                 viewport->Size.x,
                                 viewport->Size.y,
                                 points[index])) {
            return;
        }
    }
    if (!points[0].valid) {
        return;
    }
    if (!points[0].onScreen) {
        // The way to go is behind or beside the player, so the edge arrow is the whole marker: the
        // trail would be pointing off the same edge and would say nothing more.
        draw_offscreen(*list, *viewport, points[0].bearing);
        return;
    }

    const ImVec2 origin{viewport->Pos.x, viewport->Pos.y};
    // The trail, under the diamonds, between consecutive on-screen waypoints.
    for (std::size_t index = 1; index < route.count; ++index) {
        if (!points[index - 1].onScreen || !points[index].onScreen) {
            continue;
        }
        list->AddLine(ImVec2{origin.x + points[index - 1].x, origin.y + points[index - 1].y},
                      ImVec2{origin.x + points[index].x, origin.y + points[index].y},
                      kTrail,
                      kTrailThickness);
    }
    // Drawn back to front, so the waypoint the player is heading for sits over the trail behind it.
    for (std::size_t step = route.count; step > 0; --step) {
        const std::size_t index = step - 1;
        if (!points[index].onScreen) {
            continue;
        }
        const ImVec2 at{origin.x + points[index].x, origin.y + points[index].y};
        const bool primary = index == 0;
        const float radius = radius_for(points[index].range) * (primary ? 1.0F : kTrailScale);
        draw_diamond(*list, at, radius, primary);
        if (!primary) {
            continue;
        }
        std::array<char, 48> label{};
        (void)std::snprintf(label.data(),
                            label.size(),
                            "%zu/%zu  %.0fu",
                            route.ahead[index].ordinal,
                            route.stepCount,
                            static_cast<double>(points[index].range));
        draw_centered(*list, ImVec2{at.x, at.y + radius + kLabelGap}, label.data());
    }
}

} // namespace sunrise::core::ui::hud::overlays::objective
