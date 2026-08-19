/**
 * The current-status overlay. Each line says where the player is, and each has its own switch.
 *
 * Both the location and its wording come from the shared sampler, so this overlay and the mission
 * playbook cannot disagree about where the player is or word the same value differently, and the
 * costly nearest-spawn search runs once for both instead of once each.
 */

#include "ui_hud_status_overlay.h"

#include <cstdio>
#include <imgui.h>

#include "../../../../client/diagnostics/activity_location.h"
#include "../../../../server/runtime/server_runtime.h"
#include "../overlay.h"

namespace sunrise::core::ui::hud::overlays::status {
namespace {

namespace location = client::diagnostics::activity_location;

/** Widest label, which sets the value column for every row. */
constexpr char kWidestLabel[] = "Closest spawn";
/** Shown while no destination is loaded. */
constexpr char kOutOfWorld[] = "not in world";
/** Shown when every line of this overlay is switched off. */
constexpr char kNoLines[] = "every status line is off";

/** Draws one line as a label and a value. @param valueColumn Left edge of the value column. */
void draw_line(StatusLine line, const location::Line& value, float valueColumn) noexcept {
    if (!enabled(line)) {
        return;
    }
    ImGui::TextDisabled("%s", display_name(line));
    ImGui::SameLine(valueColumn);
    ImGui::TextUnformatted(value.data());
}

} // namespace

/** Draws the current-status lines inside the overlay window the stack has already started. */
void draw() noexcept {
    const bool anyLocation = enabled(StatusLine::activity) || enabled(StatusLine::bubble)
                             || enabled(StatusLine::sliceSet) || enabled(StatusLine::closestSpawn);
    const bool showActors = enabled(StatusLine::actorCount);
    if (!anyLocation && !showActors) {
        // With every line off the overlay would draw an empty box and say nothing.
        ImGui::TextDisabled("%s", kNoLines);
        return;
    }
    location::Location sampled{};
    const bool inWorld = anyLocation && location::sample(sampled);
    if (anyLocation) {
        if (!inWorld) {
            ImGui::TextDisabled("%s", kOutOfWorld);
        } else {
            location::Lines lines{};
            location::format(sampled, lines);
            const float valueColumn =
                ImGui::CalcTextSize(kWidestLabel).x + (ImGui::GetStyle().ItemSpacing.x * 2.0F);
            draw_line(StatusLine::activity, lines.activity, valueColumn);
            draw_line(StatusLine::bubble, lines.bubble, valueColumn);
            draw_line(StatusLine::sliceSet, lines.sliceSet, valueColumn);
            draw_line(StatusLine::closestSpawn, lines.spawn, valueColumn);
        }
    }
    if (showActors) {
        std::array<char, 32> count{};
        (void)std::snprintf(count.data(),
                            count.size(),
                            "%zu",
                            server::live_actor_count());
        const float valueColumn =
            ImGui::CalcTextSize(kWidestLabel).x + (ImGui::GetStyle().ItemSpacing.x * 2.0F);
        ImGui::TextDisabled("%s", display_name(StatusLine::actorCount));
        ImGui::SameLine(valueColumn);
        ImGui::TextUnformatted(count.data());
    }
}

} // namespace sunrise::core::ui::hud::overlays::status
