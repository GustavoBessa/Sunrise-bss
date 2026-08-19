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
/** Shown while the roteiro has fired nothing yet, so the enabled overlay is never an empty box. */
constexpr char kIdle[] = "no step reached yet";
/** Subtitle wrap width, in font sizes, so it scales with the interface instead of the viewport. */
constexpr float kSubtitleWrapEms = 24.0F;

} // namespace

/** Draws the most recently reached roteiro step. */
void draw() noexcept {
    const book::Announcement announcement = book::last_announcement();
    if (!announcement.present) {
        ImGui::TextDisabled("%s", kIdle);
        return;
    }
    const std::uint64_t now = GetTickCount64();
    // Unsigned arithmetic, so a tick captured after this read reports as elapsed rather than
    // wrapping into a very long hold.
    if (now < announcement.firedTick || now - announcement.firedTick >= kHoldMs) {
        ImGui::TextDisabled("%s", kIdle);
        return;
    }
    ImGui::TextUnformatted(announcement.text.data());
    const std::string_view subtitle = book::subtitle_of(announcement);
    if (subtitle.empty()) {
        return;
    }
    // Wrapped, because a line of dialogue is longer than the step line above it.
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * kSubtitleWrapEms);
    ImGui::TextUnformatted(subtitle.data(), subtitle.data() + subtitle.size());
    ImGui::PopTextWrapPos();
}

} // namespace sunrise::core::ui::hud::overlays::playbook
