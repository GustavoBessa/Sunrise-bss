/**
 * The mission playbook page. The top half lists the same four location values the HUD status
 * overlay shows, read and worded through the same shared sampler. The bottom half is the roteiro
 * built from them: a linear sequence of beats, each announcing itself when reached.
 *
 * The run block and the step table are two views of one thing. Which beat comes next is answered by
 * the playbook itself rather than recomputed here, so the page and the HUD can never disagree about
 * where the mission stands.
 */

#include "playbook_panel.h"

#include <Windows.h>

#include <array>
#include <cfloat>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <string_view>

#include "../../../client/diagnostics/activity_location.h"
#include "../../../client/diagnostics/camera_projection.h"
#include "../../../client/playbook/playbook.h"
#include "../../../client/playbook/playbook_share.h"
#include "../../../core/ui/components/filter/ui_filter_component.h"
#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../../core/ui/components/toggle/ui_toggle_component.h"

namespace sunrise::server::ui::playbook {
namespace {

namespace book = client::playbook;
namespace location = client::diagnostics::activity_location;
namespace components = core::ui::components;
namespace share = book::share;
namespace projection = client::diagnostics::camera_projection;

/** No step is selected. It lies immediately outside any roteiro. */
constexpr std::size_t kNoSelection = book::kStepCapacity;
/** Room for one authored label plus its null. */
constexpr std::size_t kLabelInputCapacity = book::kLabelCapacity + 1;
/** Room for `0x` and eight hex digits plus its null. */
constexpr std::size_t kTagInputCapacity = 16;
/** Widest location label, which sets the value column for the four rows. */
constexpr char kWidestLabel[] = "Closest spawn";
/** Room for one authored metadata value plus its null. */
constexpr std::size_t kMetadataInputCapacity = book::kMetadataCapacity + 1;

std::array<char, kLabelInputCapacity> g_label{};
std::array<char, kTagInputCapacity> g_tag{};
std::size_t g_selected{kNoSelection};
/** Input buffers for the selected step's objective and completion text editors. */
std::array<char, book::kStepTextCapacity + 1> g_objectiveText{};
std::array<char, book::kStepTextCapacity + 1> g_completionText{};

/** Manual-step section input state. */
float g_manualX{};
float g_manualY{};
float g_manualZ{};
int g_manualBubble{};
std::array<char, kLabelInputCapacity> g_manualLabel{};


std::array<char, kMetadataInputCapacity> g_author{};
std::array<char, kMetadataInputCapacity> g_description{};
std::array<share::Entry, share::kListCapacity> g_shared{};
std::size_t g_sharedCount{};
bool g_sharedListed{};
/**
 * Shared entry whose replace is armed, or `kNoSelection`.
 *
 * Overwriting a local roteiro throws away captured work with no copy kept, so it takes two presses.
 * The button's label was the only warning before, and a label is not a confirmation.
 */
std::size_t g_replacing{kNoSelection};

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

/**
 * Draws the manual-step authoring section.
 *
 * Lets the author add a step at arbitrary coordinates without physically walking there. Useful for
 * designing roteiros offline or patching a captured path with a waypoint in a bubble not visited.
 *
 * @param sampled Current location, used to pre-fill coordinates on first open.
 */
void draw_manual_step(const location::Location& sampled) noexcept {
    if (!ImGui::TreeNodeEx("Add step manually", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }
    // Pre-fill with the player's current position the first time the section is opened while in
    // world, so adding a step "here" is a one-click affair that still allows editing.
    static bool g_prefilled{};
    if (!g_prefilled && sampled.positionPresent && sampled.bubbleValid) {
        g_manualX = sampled.position[0];
        g_manualY = sampled.position[1];
        g_manualZ = sampled.position[2];
        g_manualBubble = static_cast<int>(sampled.bubble);
        g_prefilled = true;
    }
    if (!sampled.positionPresent) {
        g_prefilled = false;
    }
    const float half = ImGui::GetContentRegionAvail().x * 0.49F;
    ImGui::SetNextItemWidth(half);
    ImGui::DragFloat("X##man", &g_manualX, 0.5F, -FLT_MAX, FLT_MAX, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("Y##man", &g_manualY, 0.5F, -FLT_MAX, FLT_MAX, "%.2f");
    ImGui::SetNextItemWidth(half);
    ImGui::DragFloat("Z##man", &g_manualZ, 0.5F, -FLT_MAX, FLT_MAX, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt("Bubble##man", &g_manualBubble, 0);
    (void)components::filter::input(
        "playbook_manual_label", "Label (optional)", g_manualLabel.data(), g_manualLabel.size());
    ImGui::Spacing();
    if (ImGui::Button("Add step", ImVec2(-FLT_MIN, 0.0F))) {
        location::Position pos{g_manualX, g_manualY, g_manualZ};
        if (book::add_step(pos,
                           static_cast<std::uint32_t>((std::max)(0, g_manualBubble)),
                           std::string_view(g_manualLabel.data()))) {
            g_manualLabel = {};
            g_prefilled = false;
            g_selected = kNoSelection;
        }
    }
    ImGui::TreePop();
}

/**
 * Words one step's gate for the table.
 * @param step Step to describe.
 * @param output Receives a short null-terminated description.
 */
void format_gate(const book::Step& step, std::array<char, 24>& output) noexcept {
    switch (step.gate) {
        case book::Gate::delay:
            (void)std::snprintf(output.data(),
                                output.size(),
                                "+%.1fs",
                                static_cast<double>(step.delayMs) / 1000.0);
            break;
        case book::Gate::interaction:
            (void)std::snprintf(output.data(), output.size(), "[E]%.0fu",
                                static_cast<double>(step.radius));
            break;
        case book::Gate::clearArea:
            (void)std::snprintf(output.data(), output.size(), "clr≤%u",
                                static_cast<unsigned>(step.targetActorCount));
            break;
        case book::Gate::place:
        default:
            (void)std::snprintf(output.data(), output.size(), "%.0fu",
                                static_cast<double>(step.radius));
            break;
    }
}

/**
 * Draws the run block: how far the roteiro has got and what it is waiting for.
 *
 * The same numbers the HUD tracker shows, because following a mission and authoring one are the same
 * act done at different moments, and a second source of truth for "where am I in the run" would
 * eventually disagree with the first.
 *
 * @param roteiro Loaded roteiro.
 */
void draw_run(const book::Roteiro& roteiro) noexcept {
    const book::Run run = book::run_state(GetTickCount64());
    if (roteiro.count == 0) {
        return;
    }
    ImGui::ProgressBar(static_cast<float>(run.reached) / static_cast<float>(run.stepCount),
                       ImVec2(-FLT_MIN, 0.0F));
    if (!run.active) {
        ImGui::TextDisabled("not in world; the run starts on arrival");
    } else if (run.nextOrdinal == 0) {
        ImGui::TextDisabled("roteiro complete");
    } else {
        const std::string_view label{run.nextLabel.data(), run.nextLabelLength};
        const std::string_view shown =
            label.empty() ? std::string_view("unlabelled beat") : label;
        ImGui::Text("Next: %zu. %.*s", run.nextOrdinal, static_cast<int>(shown.size()), shown.data());
        ImGui::SameLine();
        if (run.nextIsTimed) {
            ImGui::TextDisabled("in %.1fs", static_cast<double>(run.nextWaitMs) / 1000.0);
        } else if (run.nextDistanceKnown) {
            ImGui::TextDisabled("%.0f units away", static_cast<double>(run.nextDistance));
        } else {
            ImGui::TextDisabled("in another bubble");
        }
        if (!run.sequential) {
            // Said plainly, because the tracker's "next" reads as binding and here it is not.
            ImGui::TextDisabled("the roteiro is free, so any beat can fire first");
        }
        if (g_selected != run.nextOrdinal - 1 && ImGui::SmallButton("Select this beat")) {
            g_selected = run.nextOrdinal - 1;
        }
    }

    const book::Route route = book::route_ahead();
    if (route.active) {
        ImGui::TextDisabled("marker follows waypoint %zu; %zu ahead drawn",
                            route.nearestOrdinal,
                            route.count);
    } else if (run.active) {
        ImGui::TextDisabled("no waypoint of this roteiro is in the bubble you are in");
    }
    // The game hands over no field of view, so the projection has to be told. A mismatch leaves the
    // marker right at the centre of the screen and increasingly wrong towards its edges, which is
    // exactly what it looks like when this is set wrong.
    float fov = projection::field_of_view();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5F);
    if (ImGui::DragFloat("Marker FOV",
                         &fov,
                         0.5F,
                         projection::kMinimumFieldOfView,
                         projection::kMaximumFieldOfView,
                         "%.0f deg")) {
        projection::set_field_of_view(fov);
    }
    ImGui::TextDisabled("match the game's own field of view, or the marker drifts off centre");
}

/** Draws the step table and keeps the selection inside it. @param roteiro Loaded roteiro. */
void draw_steps(const book::Roteiro& roteiro) noexcept {
    if (roteiro.count == 0) {
        ImGui::TextDisabled("no steps captured for this destination yet");
        return;
    }
    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("playbook_steps", 4, flags, ImVec2(0.0F, 220.0F))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0F);
    ImGui::TableSetupColumn("Bubble", ImGuiTableColumnFlags_WidthFixed, 52.0F);
    ImGui::TableSetupColumn("Gate", ImGuiTableColumnFlags_WidthFixed, 56.0F);
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
            g_objectiveText = {};
            g_completionText = {};
            if (step.audioTag != book::kNoAudioTag) {
                (void)std::snprintf(g_tag.data(),
                                    g_tag.size(),
                                    "0x%08X",
                                    static_cast<unsigned>(step.audioTag));
            }
            // Pre-fill text editors from the step's stored text.
            const std::size_t objLen =
                (std::min)(static_cast<std::size_t>(step.objectiveTextLength),
                           g_objectiveText.size() - 1);
            std::copy_n(step.objectiveText.data(), objLen, g_objectiveText.data());
            const std::size_t compLen =
                (std::min)(static_cast<std::size_t>(step.completionTextLength),
                           g_completionText.size() - 1);
            std::copy_n(step.completionText.data(), compLen, g_completionText.data());
        }
        ImGui::TableNextColumn();
        ImGui::Text("%u", static_cast<unsigned>(step.bubble));
        ImGui::TableNextColumn();
        std::array<char, 24> gate{};
        format_gate(step, gate);
        // A timed step is dimmed, so a glance separates the beats that are places from the beats
        // that are pauses inside a conversation.
        if (step.gate == book::Gate::delay) {
            ImGui::TextDisabled("%s", gate.data());
        } else {
            ImGui::TextUnformatted(gate.data());
        }
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

/** @return Short colour-coded label for each gate type. */
const char* gate_label(book::Gate gate) noexcept {
    switch (gate) {
        case book::Gate::place:       return "Reach";
        case book::Gate::delay:       return "Wait";
        case book::Gate::interaction: return "Interact";
        case book::Gate::clearArea:   return "Clear";
        default:                      return "?";
    }
}

/**
 * Draws the gate editor for one step.
 * @param index Step ordinal. @param step Step being edited.
 */
void draw_gate(std::size_t index, const book::Step& step) noexcept {
    // All four gate types are offered. Delay is disabled on the first step (nothing to follow).
    const book::Gate g = step.gate;
    const float quarter = ImGui::GetContentRegionAvail().x * 0.24F;
    if (ImGui::RadioButton("Reach##gate", g == book::Gate::place) && g != book::Gate::place) {
        (void)book::set_gate(index, book::Gate::place, 0U);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(index == 0);
    if (ImGui::RadioButton("Wait##gate", g == book::Gate::delay) && g != book::Gate::delay) {
        (void)book::set_gate(index, book::Gate::delay, book::kDefaultDelayMs);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::RadioButton("Interact##gate", g == book::Gate::interaction)
        && g != book::Gate::interaction) {
        (void)book::set_gate(index, book::Gate::interaction, 0U);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Clear area##gate", g == book::Gate::clearArea)
        && g != book::Gate::clearArea) {
        (void)book::set_gate(index, book::Gate::clearArea, 0U);
    }
    (void)quarter;

    switch (g) {
        case book::Gate::place: {
            float radius = step.radius;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5F);
            if (ImGui::DragFloat("Radius##gate",
                                 &radius,
                                 0.5F,
                                 book::kMinimumRadius,
                                 book::kMaximumRadius,
                                 "%.1f units")) {
                (void)book::set_radius(index, radius);
            }
            break;
        }
        case book::Gate::delay: {
            if (index == 0) {
                ImGui::TextDisabled("the first step has nothing to follow; change to another type");
                break;
            }
            int delay = static_cast<int>(step.delayMs);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5F);
            if (ImGui::DragInt("Wait##gate",
                               &delay,
                               50.0F,
                               0,
                               static_cast<int>(book::kMaximumDelayMs),
                               "%d ms")) {
                (void)book::set_gate(index, book::Gate::delay, static_cast<std::uint16_t>(delay));
            }
            ImGui::TextDisabled("measured from the moment the previous step fired");
            break;
        }
        case book::Gate::interaction: {
            float radius = step.radius;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5F);
            if (ImGui::DragFloat("Radius##gate",
                                 &radius,
                                 0.5F,
                                 book::kMinimumRadius,
                                 book::kMaximumRadius,
                                 "%.1f units")) {
                (void)book::set_radius(index, radius);
            }
            ImGui::TextDisabled("player must be in radius and press E (interact)");
            break;
        }
        case book::Gate::clearArea: {
            int target = static_cast<int>(step.targetActorCount);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5F);
            if (ImGui::DragInt("Target actors##gate", &target, 1.0F, 0, 9999, "%d remaining")) {
                (void)book::set_target_actors(index, static_cast<std::uint16_t>(target));
            }
            ImGui::TextDisabled("fires when live actor count falls to this value or below");
            break;
        }
    }
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
    if (step.spawnHash == 0) {
        // Captured before the spawn catalog was ready, so this step has no readable anchor.
        ImGui::TextDisabled("no nearest-spawn anchor recorded");
    } else {
        state::build_data::hash_names::Name storage{};
        const std::string_view named = location::spawn_set_name(step.spawnHash, storage);
        if (named.empty()) {
            ImGui::TextDisabled("near spawn 0x%08X", static_cast<unsigned>(step.spawnHash));
        } else {
            ImGui::TextDisabled("near %.*s", static_cast<int>(named.size()), named.data());
        }
    }
    ImGui::TextDisabled("captured at %.1f, %.1f, %.1f  |  slice state %u  |  region %d",
                        static_cast<double>(step.position[0]),
                        static_cast<double>(step.position[1]),
                        static_cast<double>(step.position[2]),
                        static_cast<unsigned>(step.sliceState),
                        static_cast<int>(step.region));
    ImGui::Spacing();
    draw_gate(g_selected, step);

    ImGui::Spacing();
    ImGui::SeparatorText("Sound");
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

    ImGui::Spacing();
    ImGui::SeparatorText("Presentation");
    (void)components::filter::input("playbook_obj_text",
                                    "Objective (shown while waiting for this step)",
                                    g_objectiveText.data(),
                                    g_objectiveText.size());
    (void)components::filter::input("playbook_comp_text",
                                    "Completion (shown when step fires)",
                                    g_completionText.data(),
                                    g_completionText.size());
    if (ImGui::Button("Apply text", ImVec2(-FLT_MIN, 0.0F))) {
        (void)book::set_objective_text(g_selected, std::string_view(g_objectiveText.data()));
        (void)book::set_completion_text(g_selected, std::string_view(g_completionText.data()));
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Order");
    // Move-up is swapping this step with the one before it.
    ImGui::BeginDisabled(g_selected == 0);
    if (ImGui::Button("Move up", ImVec2(ImGui::GetContentRegionAvail().x * 0.49F, 0.0F))) {
        if (book::move_step_down(g_selected - 1)) {
            --g_selected;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_selected + 1 >= roteiro.count);
    if (ImGui::Button("Move down", ImVec2(-FLT_MIN, 0.0F))) {
        if (book::move_step_down(g_selected)) {
            ++g_selected;
        }
    }
    ImGui::EndDisabled();
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
        if (!entry.collides) {
            if (ImGui::Button("Import")) {
                (void)share::import_entry(name, false);
                g_sharedListed = false;
            }
        } else if (g_replacing == index) {
            // Armed. The second press is the one that overwrites, so the destructive click is never
            // the first click: the local roteiro is captured work and there is no copy of it.
            if (ImGui::Button("Overwrite my roteiro")) {
                (void)share::import_entry(name, true);
                g_replacing = kNoSelection;
                g_sharedListed = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                g_replacing = kNoSelection;
            }
            ImGui::TextDisabled("this discards the roteiro you captured for this destination");
        } else {
            if (ImGui::Button("Replace")) {
                g_replacing = index;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("you already have one for this destination");
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
    ImGui::Spacing();
    draw_manual_step(sampled);

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
                        "%.*s  |  %zu of %zu steps reached  |  %llus in world",
                        static_cast<int>(shown.size()),
                        shown.data(),
                        book::reached_count(),
                        roteiro.count,
                        static_cast<unsigned long long>(book::run_age(GetTickCount64()) / 1000ULL));
    components::section::header("Roteiro", summary.data());
    ImGui::Spacing();
    bool sequential = roteiro.sequential;
    if (components::toggle::control("Steps fire in order", sequential)
        && roteiro.destinationLength != 0) {
        (void)book::set_sequential(sequential);
    }
    ImGui::TextDisabled(roteiro.sequential
                            ? "a step waits on the one before it, which is what a wait needs"
                            : "any step fires as soon as it is reached, in any order");
    ImGui::Spacing();
    draw_run(roteiro);
    ImGui::Spacing();
    draw_steps(roteiro);
    ImGui::Spacing();
    draw_selected(roteiro);

    ImGui::Spacing();
    ImGui::Spacing();
    draw_share(roteiro);
}

} // namespace sunrise::server::ui::playbook
