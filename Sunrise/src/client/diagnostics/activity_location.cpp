/**
 * The one place the player's location is derived. The HUD status overlay and the mission playbook
 * both read it here, so they cannot disagree about where the player is, and the expensive
 * nearest-spawn search runs once for both instead of once each.
 */

#include "activity_location.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>

#include "../../middleware/content/packages/tables/region_reader.h"
#include "../../middleware/content/packages/tables/spawn_reader.h"
#include "../../state/activity/definition.h"
#include "../../state/activity/destination/activity_destination_snapshot.h"
#include "../../state/activity/membership/activity_membership_query.h"
#include "../../state/build_data/runtime.h"
#include "../hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../player/player_position.h"

namespace sunrise::client::diagnostics::activity_location {
namespace {

namespace activity = state::activity;
namespace layouts = state::build_data::scenarios;
namespace tables = middleware::content::packages::tables;

// The selection's package name is copied straight into the location's storage, so a divergence
// between the two capacities must be a compile error rather than a silent truncation.
static_assert(activity::destination::kPackageNameCapacity == layouts::kNameCapacity);

/** How long a nearest-spawn result is held before the bank is searched again. */
constexpr std::uint64_t kSpawnSearchIntervalMs = 250;
/** Shown for a value the published State does not name. */
constexpr char kUnknown[] = "unknown";
/** Shown while the player's position has not been read yet. */
constexpr char kNoPosition[] = "waiting for a position";

/** The last successful nearest-spawn search, kept between calls because the bank is large. */
struct SpawnCache {
    std::uint32_t hash{};
    float distance{};
    std::uint64_t searchedTick{};
    bool valid{};
};

/**
 * Guards the cache below. The HUD overlay and the playbook both sample, and while both run on the
 * game's own pump today, a cache shared by more than one caller should not depend on that.
 */
SRWLOCK g_spawnLock{SRWLOCK_INIT};
SpawnCache g_spawn{};

/** @param value Text to store. @param output Receives it with a null. */
void assign(std::string_view value, Line& output) noexcept {
    output = {};
    (void)std::snprintf(
        output.data(), output.size(), "%.*s", static_cast<int>(value.size()), value.data());
}

/**
 * Copies the committed package name into the location.
 * @param selection Committed destination.
 * @param output Receives the name bytes and their length.
 */
void copy_destination(const activity::destination::DestinationSelection& selection,
                      Location& output) noexcept {
    const auto reported = static_cast<std::size_t>(selection.packageNameLength);
    const std::size_t length = (std::min)(reported, output.destination.size());
    for (std::size_t index = 0; index < length; ++index) {
        output.destination[index] = static_cast<char>(selection.packageName[index]);
    }
    output.destinationLength = static_cast<std::uint8_t>(length);
}

/**
 * Fills the bubble and slice-set fields from the region the host reported.
 * @param layout Destination layout the region belongs to.
 * @param output Location whose `region` is already set.
 */
void derive_region(const layouts::Definition& layout, Location& output) noexcept {
    if (output.region < 0) {
        return;
    }
    const auto index = static_cast<std::uint32_t>(output.region);
    output.sliceState = index % tables::kSliceSetIndexFactor;
    const auto bubble = static_cast<std::size_t>(index / tables::kSliceSetIndexFactor);
    if (bubble >= layout.bubbleCount) {
        return;
    }
    output.bubble = bubble;
    output.bubbleHash = layout.bubbleHashes[bubble];
    output.bubbleValid = true;
}

/**
 * Finds the spawn point nearest the player, at most once per interval.
 * @param stem Map-package stem of the loaded destination.
 * @param output Location whose position fields are already set.
 */
void search_spawn(std::string_view stem, Location& output) noexcept {
    output.stemPresent = !stem.empty();
    if (!output.positionPresent || !output.stemPresent) {
        // No search is possible, so the held result is left alone rather than reported for a
        // position it was not measured from.
        return;
    }
    const std::uint64_t now = GetTickCount64();
    AcquireSRWLockExclusive(&g_spawnLock);
    if (g_spawn.valid && now - g_spawn.searchedTick < kSpawnSearchIntervalMs) {
        output.spawnHash = g_spawn.hash;
        output.spawnDistance = g_spawn.distance;
        output.spawnFound = true;
        ReleaseSRWLockExclusive(&g_spawnLock);
        return;
    }
    state::build_data::spawn_sets::Point point{};
    float distance = 0.0F;
    g_spawn = {};
    g_spawn.searchedTick = now;
    if (!state::build_data::find_nearest_spawn_point(stem, output.position, point, distance)) {
        ReleaseSRWLockExclusive(&g_spawnLock);
        return;
    }
    g_spawn.hash = point.nameHash;
    g_spawn.distance = distance;
    g_spawn.valid = true;
    ReleaseSRWLockExclusive(&g_spawnLock);
    output.spawnHash = point.nameHash;
    output.spawnDistance = distance;
    output.spawnFound = true;
}

/** Drops the held nearest-spawn result. */
void clear_spawn_cache() noexcept {
    AcquireSRWLockExclusive(&g_spawnLock);
    g_spawn = {};
    ReleaseSRWLockExclusive(&g_spawnLock);
}

} // namespace

/** Reads the player's current location from published State in one pass. */
bool sample(Location& output) noexcept {
    output = {};
    const std::uint64_t sessionId =
        activity::membership::live_region_session(activity::kAbsentSessionId);
    // The client's own step, published every frame. The world phase only moves on the spawn gate,
    // which stops being polled once the player is in, so it stays `arrived` in orbit.
    output.inWorld =
        client::hooks::bootflow::in_world() && sessionId != activity::kAbsentSessionId;
    if (!output.inWorld) {
        clear_spawn_cache();
        // The next destination has its own map, so a position from this one must not carry over.
        client::player::position::reset();
        return false;
    }

    activity::destination::DestinationSelection selection{};
    (void)activity::destination::snapshot(sessionId, selection);
    copy_destination(selection, output);
    output.region = activity::membership::reported_region(sessionId);

    const client::player::position::Snapshot player = client::player::position::snapshot();
    output.positionPresent = player.present;
    if (player.present) {
        output.position = player.position;
    }

    layouts::Definition layout{};
    if (!state::build_data::find_scenario_layout(destination_of(output), layout)) {
        // Without a layout no bubble can be named and no stem is known, but the slice-set state is
        // still readable from the region alone.
        if (output.region >= 0) {
            output.sliceState =
                static_cast<std::uint32_t>(output.region) % tables::kSliceSetIndexFactor;
        }
        return true;
    }
    output.layoutFound = true;
    derive_region(layout, output);
    search_spawn({layout.spawnStem.data(), layout.spawnStemLength}, output);
    return true;
}

/** Names one bubble hash out of the published hash-name table. */
std::string_view bubble_name(std::uint32_t hash,
                             state::build_data::hash_names::Name& storage) noexcept {
    if (!state::build_data::find_hash_name(hash, storage)) {
        return {};
    }
    return {storage.name.data(), storage.nameLength};
}

/** Names one spawn-set hash, including the two the client itself defines. */
std::string_view spawn_set_name(std::uint32_t hash,
                                state::build_data::hash_names::Name& storage) noexcept {
    if (hash == tables::kDefaultSpawnNameHash) {
        return "default";
    }
    if (hash == tables::kUnnamedSpawnNameHash) {
        return "unnamed";
    }
    return bubble_name(hash, storage);
}

/** Formats the four status lines from one sampled location. */
void format(const Location& sampled, Lines& output) noexcept {
    output = {};
    const std::string_view destination = destination_of(sampled);
    assign(destination.empty() ? std::string_view(kUnknown) : destination, output.activity);

    if (!sampled.layoutFound || !sampled.bubbleValid) {
        assign(kUnknown, output.bubble);
    } else {
        state::build_data::hash_names::Name storage{};
        const std::string_view named = bubble_name(sampled.bubbleHash, storage);
        (void)std::snprintf(output.bubble.data(),
                            output.bubble.size(),
                            "%zu  0x%08X%s%.*s",
                            sampled.bubble,
                            sampled.bubbleHash,
                            named.empty() ? "" : "  ",
                            static_cast<int>(named.size()),
                            named.data());
    }

    if (sampled.region < 0) {
        assign(kUnknown, output.sliceSet);
    } else {
        (void)std::snprintf(output.sliceSet.data(),
                            output.sliceSet.size(),
                            "%u  state %u",
                            static_cast<std::uint32_t>(sampled.region),
                            sampled.sliceState);
    }

    if (!sampled.layoutFound) {
        assign(kUnknown, output.spawn);
    } else if (!sampled.positionPresent || !sampled.stemPresent) {
        assign(kNoPosition, output.spawn);
    } else if (!sampled.spawnFound) {
        assign(kUnknown, output.spawn);
    } else {
        state::build_data::hash_names::Name storage{};
        const std::string_view named = spawn_set_name(sampled.spawnHash, storage);
        (void)std::snprintf(output.spawn.data(),
                            output.spawn.size(),
                            "0x%08X%s%.*s  %.1f units",
                            sampled.spawnHash,
                            named.empty() ? "" : "  ",
                            static_cast<int>(named.size()),
                            named.data(),
                            static_cast<double>(sampled.spawnDistance));
    }
}

} // namespace sunrise::client::diagnostics::activity_location
