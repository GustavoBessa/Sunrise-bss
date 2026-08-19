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
#include <cstring>
#include <imgui.h>
#include <string_view>

#include "../../../client/content/strings/subtitle_catalog.h"
#include "../../../client/diagnostics/activity_location.h"
#include "../../../client/playbook/playbook.h"
#include "../../../client/playbook/playbook_share.h"
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

namespace catalog = client::content::strings::catalog;
namespace share = book::share;

/** Subtitle matches one search shows. */
constexpr std::size_t kMatchCapacity = 40;
/** Room for one search term plus its null. */
constexpr std::size_t kSearchCapacity = 64;
/** Room for one authored metadata value plus its null. */
constexpr std::size_t kMetadataInputCapacity = book::kMetadataCapacity + 1;

std::array<char, kLabelInputCapacity> g_label{};
std::array<char, kTagInputCapacity> g_tag{};
std::size_t g_selected{kNoSelection};

std::array<char, kSearchCapacity> g_search{};
/** The term the results below were produced for, so the scan runs on a change and not per frame. */
std::array<char, kSearchCapacity> g_searched{};
std::array<catalog::Match, kMatchCapacity> g_matches{};
std::size_t g_matchCount{};
std::size_t g_matchSelected{kNoSelection};

std::array<char, kMetadataInputCapacity> g_author{};
std::array<char, kMetadataInputCapacity> g_description{};
std::array<share::Entry, share::kListCapacity> g_shared{};
std::size_t g_sharedCount{};
bool g_sharedListed{};

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
    // Only the match terms gate a capture. The nearest spawn is the step's readable anchor, so a
    // catalog that is not ready yet costs the label, not the capture.
    const bool capturable = inWorld && sampled.bubbleValid && sampled.positionPresent;
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
        ImGui::TextDisabled(inWorld ? "waiting for a bubble and a position" : "not in world");
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
        if (step.spawnHash == 0) {
            // Captured before the spawn catalog was ready, so this step has no readable anchor.
            ImGui::TextDisabled("-");
        } else {
            state::build_data::hash_names::Name storage{};
            const std::string_view named = location::spawn_set_name(step.spawnHash, storage);
            if (named.empty()) {
                ImGui::Text("0x%08X", static_cast<unsigned>(step.spawnHash));
            } else {
                ImGui::Text("%.*s", static_cast<int>(named.size()), named.data());
            }
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

    if (step.subtitleHash != 0) {
        catalog::Match match{};
        if (catalog::text_for(step.subtitleHash, match)) {
            ImGui::TextWrapped("Subtitle: %.*s", static_cast<int>(match.length), match.text.data());
        } else {
            ImGui::TextDisabled("Subtitle 0x%08X, not in the catalog",
                                static_cast<unsigned>(step.subtitleHash));
        }
        if (ImGui::SmallButton("Clear subtitle")) {
            (void)book::set_subtitle(g_selected, 0);
        }
        ImGui::Spacing();
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

/** Draws the subtitle catalog: its build state, a search, and the attach action. */
void draw_subtitles(const book::Roteiro& roteiro) noexcept {
    if (!ImGui::TreeNodeEx("Subtitles", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }
    const catalog::Progress progress = catalog::progress();
    if (progress.building) {
        ImGui::TextDisabled("building  %zu / %zu containers  |  %zu strings",
                            progress.containersDone,
                            progress.containers,
                            progress.rows);
    } else if (progress.ready) {
        ImGui::TextDisabled("%zu strings catalogued", progress.rows);
    } else if (progress.failed) {
        const std::string_view reason = catalog::failure();
        ImGui::TextDisabled("no catalog  (%.*s)", static_cast<int>(reason.size()), reason.data());
    } else {
        ImGui::TextDisabled("no catalog yet");
    }
    ImGui::BeginDisabled(progress.building);
    if (ImGui::Button("Build catalog")) {
        catalog::rebuild();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("reads the game's own strings; English only");

    ImGui::Spacing();
    (void)components::filter::input(
        "playbook_search", "Search subtitle text", g_search.data(), g_search.size());
    // Scanned on a change, not per frame: the walk touches every catalogued string.
    if (std::strcmp(g_search.data(), g_searched.data()) != 0) {
        g_searched = g_search;
        g_matchSelected = kNoSelection;
        g_matchCount = catalog::search(std::string_view(g_search.data()), g_matches);
    }

    if (g_matchCount == 0) {
        ImGui::TextDisabled("no matches");
    } else if (ImGui::BeginListBox("##playbook_matches", ImVec2(-FLT_MIN, 160.0F))) {
        for (std::size_t index = 0; index < g_matchCount; ++index) {
            const catalog::Match& match = g_matches[index];
            std::array<char, catalog::kTextCapacity + 32> row{};
            (void)std::snprintf(row.data(),
                                row.size(),
                                "%.*s##match%zu",
                                static_cast<int>(match.length),
                                match.text.data(),
                                index);
            if (ImGui::Selectable(row.data(), g_matchSelected == index)) {
                g_matchSelected = index;
            }
        }
        ImGui::EndListBox();
    }

    const bool attachable = g_matchSelected < g_matchCount && g_selected < roteiro.count;
    ImGui::BeginDisabled(!attachable);
    if (ImGui::Button("Attach to selected step", ImVec2(-FLT_MIN, 0.0F))) {
        (void)book::set_subtitle(g_selected, g_matches[g_matchSelected].hash);
    }
    ImGui::EndDisabled();
    if (!attachable) {
        ImGui::TextDisabled("select a step above and a subtitle here");
    }
    ImGui::TreePop();
}

/** Draws the sharing section: metadata, export, and the shared folder listing. */
void draw_share(const book::Roteiro& roteiro) noexcept {
    if (!ImGui::TreeNodeEx("Share", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }
    (void)components::filter::input("playbook_author", "Author", g_author.data(), g_author.size());
    (void)components::filter::input(
        "playbook_description", "Description", g_description.data(), g_description.size());
    ImGui::BeginDisabled(roteiro.count == 0);
    if (ImGui::Button("Save details", ImVec2(ImGui::GetContentRegionAvail().x * 0.49F, 0.0F))) {
        (void)book::set_metadata(std::string_view(g_author.data()),
                                 std::string_view(g_description.data()));
    }
    ImGui::SameLine();
    if (ImGui::Button("Export", ImVec2(-FLT_MIN, 0.0F))) {
        if (share::export_current()) {
            g_sharedListed = false;
        }
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Export writes to Sunrise\\playbooks\\shared");

    ImGui::Spacing();
    if (!g_sharedListed) {
        g_sharedListed = true;
        g_sharedCount = share::list(g_shared);
    }
    if (ImGui::Button("Refresh shared")) {
        g_sharedListed = false;
    }
    if (g_sharedCount == 0) {
        ImGui::TextDisabled("nothing in the shared folder");
        ImGui::TreePop();
        return;
    }
    for (std::size_t index = 0; index < g_sharedCount; ++index) {
        const share::Entry& entry = g_shared[index];
        const std::string_view name = share::destination_of(entry);
        const std::string_view author = book::value_of(entry.author);
        ImGui::PushID(static_cast<int>(index));
        ImGui::Text("%.*s  |  %zu steps", static_cast<int>(name.size()), name.data(), entry.steps);
        if (!author.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("by %.*s", static_cast<int>(author.size()), author.data());
        }
        if (!entry.destinationKnown) {
            // Said now, because an import that cannot fire otherwise looks like a broken feature.
            ImGui::TextDisabled("this install has no such destination; steps would never fire");
        }
        if (ImGui::Button(entry.collides ? "Replace" : "Import")) {
            (void)share::import_entry(name, entry.collides);
            g_sharedListed = false;
        }
        if (entry.collides) {
            ImGui::SameLine();
            ImGui::TextDisabled("you already have this one");
        }
        ImGui::PopID();
        ImGui::Separator();
    }
    ImGui::TreePop();
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

    ImGui::Spacing();
    ImGui::Spacing();
    draw_subtitles(roteiro);
    draw_share(roteiro);
}

} // namespace sunrise::server::ui::playbook
