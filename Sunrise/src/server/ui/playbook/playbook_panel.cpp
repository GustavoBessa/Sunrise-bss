/**
 * The mission playbook page. The top half lists the same four location values the HUD status
 * overlay shows, read and worded through the same shared sampler. The bottom half is the roteiro
 * built from them: captured steps, in order, each of which announces itself when reached.
 */

#include "playbook_panel.h"

#include <array>
#include <cfloat>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <string_view>

#include "../../../client/diagnostics/activity_location.h"
#include "../../../client/playbook/playbook.h"
#include "../../../core/ui/components/filter/ui_filter_component.h"
#include "../../../core/ui/components/section/ui_section_component.h"

namespace sunrise::server::ui::playbook {
namespace {

namespace book = client::playbook;
namespace location = client::diagnostics::activity_location;
namespace components = core::ui::components;

/** No step is selected. It lies immediately outside any roteiro. */
constexpr std::size_t kNoSelection = book::kStepCapacity;
/** Room for one authored label plus its null. */
constexpr std::size_t kLabelInputCapacity = book::kLabelCapacity + 1;
/** Room for `0x` and eight hex digits plus its null. */
constexpr std::size_t kTagInputCapacity = 16;
/** Widest location label, which sets the value column for the four rows. */
constexpr char kWidestLabel[] = "Closest spawn";

std::array<char, kLabelInputCapacity> g_label{};
std::array<char, kTagInputCapacity> g_tag{};
std::size_t g_selected{kNoSelection};

/** Draws one location row as a muted label and its value. */
void draw_location_row(const char* name, const location::Line& value, float valueColumn) noexcept {
    ImGui::TextDisabled("%s", name);
    ImGui::SameLine(valueColumn);
    ImGui::TextUnformatted(value.data());
}

/**
 * Draws the live location block.
 * @param sampled Receives the sample, so the capture control can reuse it.
 * @return True while the player is in a world.
 */
[[nodiscard]] bool draw_location(location::Location& sampled) noexcept {
    components::section::header("Current location",
                               "The same values the HUD status overlay shows.");
    ImGui::Spacing();
    if (!location::sample(sampled)) {
        ImGui::TextDisabled("not in world");
        return false;
    }
    location::Lines lines{};
    location::format(sampled, lines);
    const float valueColumn =
        ImGui::CalcTextSize(kWidestLabel).x + (ImGui::GetStyle().ItemSpacing.x * 2.0F);
    draw_location_row("Activity", lines.activity, valueColumn);
    draw_location_row("Bubble", lines.bubble, valueColumn);
    draw_location_row("Slice set", lines.sliceSet, valueColumn);
    draw_location_row("Closest spawn", lines.spawn, valueColumn);
    return true;
}

/**
 * Draws the capture and rearm controls.
 * @param sampled Current location.
 * @param inWorld Whether a capture is possible at all.
 */
void draw_controls(const location::Location& sampled, bool inWorld) noexcept {
    // Every one of these is part of the match, so a step captured without them could never fire.
    const bool capturable =
        inWorld && sampled.bubbleValid && sampled.spawnFound && sampled.positionPresent;
    (void)components::filter::input(
        "playbook_label", "Label for the next step", g_label.data(), g_label.size());
    ImGui::Spacing();
    ImGui::BeginDisabled(!capturable);
    if (ImGui::Button("Capture point", ImVec2(ImGui::GetContentRegionAvail().x * 0.49F, 0.0F))) {
        if (book::capture(std::string_view(g_label.data()))) {
            g_label = {};
            g_selected = kNoSelection;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Rearm", ImVec2(-FLT_MIN, 0.0F))) {
        book::rearm();
    }
    if (!capturable) {
        ImGui::TextDisabled(
            inWorld ? "waiting for a bubble, a position and a closest spawn" : "not in world");
    }
}

/** Draws the step table and keeps the selection inside it. @param roteiro Loaded roteiro. */
void draw_steps(const book::Roteiro& roteiro) noexcept {
    if (roteiro.count == 0) {
        ImGui::TextDisabled("no steps captured for this destination yet");
        return;
    }
    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("playbook_steps", 5, flags, ImVec2(0.0F, 220.0F))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0F);
    ImGui::TableSetupColumn("Bubble", ImGuiTableColumnFlags_WidthFixed, 52.0F);
    ImGui::TableSetupColumn("Spawn");
    ImGui::TableSetupColumn("Radius", ImGuiTableColumnFlags_WidthFixed, 56.0F);
    ImGui::TableSetupColumn("Label");
    ImGui::TableHeadersRow();

    for (std::size_t index = 0; index < roteiro.count; ++index) {
        const book::Step& step = roteiro.steps[index];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        std::array<char, 32> ordinal{};
        (void)std::snprintf(ordinal.data(), ordinal.size(), "%zu##step%zu", index + 1, index);
        // The whole row selects, so a step can be edited without hunting for a control.
        if (ImGui::Selectable(ordinal.data(),
                              g_selected == index,
                              ImGuiSelectableFlags_SpanAllColumns)) {
            g_selected = index;
            g_tag = {};
            if (step.audioTag != book::kNoAudioTag) {
                (void)std::snprintf(g_tag.data(),
                                    g_tag.size(),
                                    "0x%08X",
                                    static_cast<unsigned>(step.audioTag));
            }
        }
        ImGui::TableNextColumn();
        ImGui::Text("%u", static_cast<unsigned>(step.bubble));
        ImGui::TableNextColumn();
        state::build_data::hash_names::Name storage{};
        const std::string_view named = location::spawn_set_name(step.spawnHash, storage);
        if (named.empty()) {
            ImGui::Text("0x%08X", static_cast<unsigned>(step.spawnHash));
        } else {
            ImGui::Text("%.*s", static_cast<int>(named.size()), named.data());
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", static_cast<double>(step.radius));
        ImGui::TableNextColumn();
        // A reached step is marked so the run's progress is readable at a glance.
        const std::string_view label = book::label_of(step);
        if (step.reached) {
            ImGui::TextDisabled("* %.*s", static_cast<int>(label.size()), label.data());
        } else {
            ImGui::Text("%.*s", static_cast<int>(label.size()), label.data());
        }
    }
    ImGui::EndTable();
}

/** Draws the editor for the selected step. @param roteiro Loaded roteiro. */
void draw_selected(const book::Roteiro& roteiro) noexcept {
    if (g_selected >= roteiro.count) {
        g_selected = kNoSelection;
        ImGui::TextDisabled("select a step to edit it");
        return;
    }
    const book::Step& step = roteiro.steps[g_selected];
    ImGui::Text("Step %zu of %zu", g_selected + 1, roteiro.count);
    ImGui::TextDisabled("captured at %.1f, %.1f, %.1f  |  slice state %u  |  region %d",
                        static_cast<double>(step.position[0]),
                        static_cast<double>(step.position[1]),
                        static_cast<double>(step.position[2]),
                        static_cast<unsigned>(step.sliceState),
                        static_cast<int>(step.region));
    ImGui::Spacing();

    float radius = step.radius;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5F);
    if (ImGui::DragFloat("Radius",
                         &radius,
                         0.5F,
                         book::kMinimumRadius,
                         book::kMaximumRadius,
                         "%.1f units")) {
        (void)book::set_radius(g_selected, radius);
    }

    (void)components::filter::input(
        "playbook_tag", "Audio tag, hex", g_tag.data(), g_tag.size());
    ImGui::TextDisabled("Nothing plays yet. The tag is stored for when a sound can be emitted.");
    ImGui::Spacing();
    if (ImGui::Button("Apply tag", ImVec2(ImGui::GetContentRegionAvail().x * 0.49F, 0.0F))) {
        const std::string_view text{g_tag.data()};
        std::uint32_t parsed = book::kNoAudioTag;
        if (text.empty()) {
            (void)book::set_audio_tag(g_selected, book::kNoAudioTag);
        } else {
            const std::string_view digits =
                text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')
                    ? text.substr(2)
                    : text;
            const char* const end = digits.data() + digits.size();
            const auto result = std::from_chars(digits.data(), end, parsed, 16);
            if (result.ec == std::errc{} && result.ptr == end) {
                (void)book::set_audio_tag(g_selected, parsed);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove step", ImVec2(-FLT_MIN, 0.0F))) {
        if (book::remove_step(g_selected)) {
            g_selected = kNoSelection;
            g_tag = {};
        }
    }
}

} // namespace

/** Draws the mission playbook page inside the active Core UI frame. */
void draw() noexcept {
    location::Location sampled{};
    const bool inWorld = draw_location(sampled);

    ImGui::Spacing();
    ImGui::Spacing();
    components::section::header("Capture",
                                "Records this location as the next step of the roteiro.");
    ImGui::Spacing();
    draw_controls(sampled, inWorld);

    // Read after the controls, so a step captured this frame is already in the table below.
    ImGui::Spacing();
    ImGui::Spacing();
    const book::Roteiro roteiro = book::get();
    // An empty name cannot go through the precision form: a zero precision would print nothing and
    // swallow the stand-in with it.
    const std::string_view destination = book::destination_of(roteiro);
    const std::string_view shown =
        destination.empty() ? std::string_view("no destination") : destination;
    std::array<char, 128> summary{};
    (void)std::snprintf(summary.data(),
                        summary.size(),
                        "%.*s  |  %zu of %zu steps reached",
                        static_cast<int>(shown.size()),
                        shown.data(),
                        book::reached_count(),
                        roteiro.count);
    components::section::header("Roteiro", summary.data());
    ImGui::Spacing();
    draw_steps(roteiro);
    ImGui::Spacing();
    draw_selected(roteiro);
}

} // namespace sunrise::server::ui::playbook
