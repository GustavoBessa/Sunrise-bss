/**
 * The playbook overlay. It shows the roteiro step the player has just reached, so the start and the
 * end of a roteiro announce themselves without the menu being open.
 *
 * The wording comes from the playbook, which owns what a roteiro's first and last step mean. All
 * this overlay decides is how long the line stays up.
 */

#include "ui_hud_playbook_overlay.h"

#include <Windows.h>

#include <imgui.h>

#include "../../../../client/playbook/playbook.h"

namespace sunrise::core::ui::hud::overlays::playbook {
namespace {

namespace book = client::playbook;

/** How long a reached step stays on screen. Long enough to read, short enough to stop nagging. */
constexpr std::uint64_t kHoldMs = 6'000;
/** Shown while no roteiro is loaded, so the enabled overlay is never an empty box. */
constexpr char kIdle[] = "no roteiro for this destination";
/**
 * Draws the tracker line: which beat the roteiro is waiting for, and what it is waiting on.
 *
 * This is what makes a roteiro followable rather than only recordable. Without it the overlay can
 * say where the player has been and never where the mission goes next.
 */
void draw_tracker() noexcept {
    const book::Run run = book::run_state(GetTickCount64());
    if (!run.active) {
        ImGui::TextDisabled("%s", kIdle);
        return;
    }
    if (run.nextOrdinal == 0) {
        ImGui::TextDisabled("roteiro complete  |  %zu/%zu", run.reached, run.stepCount);
        return;
    }
    const std::string_view label{run.nextLabel.data(), run.nextLabelLength};
    // The ordinal always shows; the label is the author's and may be empty.
    if (label.empty()) {
        ImGui::TextDisabled("next  %zu/%zu", run.nextOrdinal, run.stepCount);
    } else {
        ImGui::TextDisabled("next  %zu/%zu  %.*s",
                            run.nextOrdinal,
                            run.stepCount,
                            static_cast<int>(label.size()),
                            label.data());
    }
    if (run.nextIsTimed) {
        ImGui::TextDisabled("in %.1fs", static_cast<double>(run.nextWaitMs) / 1000.0);
        return;
    }
    if (run.nextDistanceKnown) {
        ImGui::TextDisabled("%.0f units away", static_cast<double>(run.nextDistance));
        return;
    }
    // Another bubble, so a straight line would point through walls and send the player wrong.
    ImGui::TextDisabled("in another bubble");
}

} // namespace

/** Draws the most recently reached roteiro step, or the way to the next one. */
void draw() noexcept {
    const book::Announcement announcement = book::last_announcement();
    const std::uint64_t now = GetTickCount64();
    // Unsigned arithmetic, so a tick captured after this read reports as elapsed rather than
    // wrapping into a very long hold.
    const bool holding = announcement.present && now >= announcement.firedTick
                         && now - announcement.firedTick < kHoldMs;
    if (!holding) {
        // Nothing just happened, so the overlay says where the mission goes next instead.
        draw_tracker();
        return;
    }
    ImGui::TextUnformatted(announcement.text.data());
}

} // namespace sunrise::core::ui::hud::overlays::playbook
