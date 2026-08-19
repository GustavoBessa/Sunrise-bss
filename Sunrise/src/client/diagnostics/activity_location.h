#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../state/build_data/hash_names/definition.h"
#include "../../state/build_data/scenarios/definition.h"

namespace sunrise::client::diagnostics::activity_location {

/** Three floats make one world position, the basis the game stores a body position in. */
inline constexpr std::size_t kPositionLanes = 3;

/** One world position, laid out as the spawn catalog and the physics hooks both expect it. */
using Position = std::array<float, kPositionLanes>;

/**
 * Where the local player is, read from published State in one pass.
 *
 * Every consumer reads the same struct so none of them can disagree about the player's location.
 * The flags say which fields carry a value: a consumer that formats one of them for display must
 * check its flag rather than the value, because an absent field keeps its zero.
 */
struct Location {
    /** Committed destination package name. Empty when none is committed. */
    std::array<char, state::build_data::scenarios::kNameCapacity> destination{};
    std::uint8_t destinationLength{};
    /** Region index the host reported. Negative when the host named none. */
    std::int32_t region{};
    /** Bubble the region belongs to. Carries a value only while `bubbleValid` is set. */
    std::size_t bubble{};
    /** That bubble's own name hash. Carries a value only while `bubbleValid` is set. */
    std::uint32_t bubbleHash{};
    /** Slice-set state inside the bubble. Carries a value only while `region` is not negative. */
    std::uint32_t sliceState{};
    /** Player world position. Carries a value only while `positionPresent` is set. */
    Position position{};
    /** Nearest spawn set's name hash. Carries a value only while `spawnFound` is set. */
    std::uint32_t spawnHash{};
    /** Distance to that spawn point in world units. Only while `spawnFound` is set. */
    float spawnDistance{};
    /** The player is in a world and a region session is live. Every field below needs this. */
    bool inWorld{};
    /** The destination's bubble layout was found, without which no bubble can be named. */
    bool layoutFound{};
    /** The region maps to a bubble the layout declares. */
    bool bubbleValid{};
    /** A player position has been observed since the last world entry. */
    bool positionPresent{};
    /** The layout named a map-package stem, which the spawn search is keyed by. */
    bool stemPresent{};
    /** The spawn search ran and found a point. */
    bool spawnFound{};
};

/** @param value Sampled location. @return Its destination name as a bounded view. */
[[nodiscard]] inline std::string_view destination_of(const Location& value) noexcept {
    return {value.destination.data(), value.destinationLength};
}

/**
 * Reads the player's current location from published State.
 *
 * The nearest-spawn search walks a bank of up to 65,536 points, so it is rate limited and its last
 * result is reused between calls. That budget is shared: this is why every consumer samples here
 * instead of searching on its own.
 *
 * Leaving the world drops the published player position, because the next destination has its own
 * map and a position from this one must not carry over.
 *
 * @param output Receives the location. Cleared first, so a partial read leaves no stale field.
 * @return True while the player is in a world. False leaves only `inWorld` meaningful.
 */
[[nodiscard]] bool sample(Location& output) noexcept;

/** Bytes of one formatted status line, including its null. */
inline constexpr std::size_t kLineCapacity = 96;

/** One formatted status line. */
using Line = std::array<char, kLineCapacity>;

/** The four status lines, worded exactly as the HUD status overlay shows them. */
struct Lines {
    Line activity{};
    Line bubble{};
    Line sliceSet{};
    Line spawn{};
};

/**
 * Formats the four status lines from one sampled location.
 *
 * Every surface that shows the player's location formats it here, so the HUD overlay and the
 * mission playbook cannot word the same value differently.
 *
 * @param sampled Location from `sample()`. Only meaningful while it reports being in world.
 * @param output Receives the four lines, each null terminated.
 */
void format(const Location& sampled, Lines& output) noexcept;

/**
 * Names one bubble hash.
 * @param hash Bubble name hash from a destination layout.
 * @param storage Receives the found row, which owns the returned bytes.
 * @return The internal name, or an empty view when no installed package names the hash.
 */
[[nodiscard]] std::string_view bubble_name(std::uint32_t hash,
                                           state::build_data::hash_names::Name& storage) noexcept;

/**
 * Names one spawn-set hash. The packages name few, so the two the client defines are named here.
 * @param hash Spawn-set name hash.
 * @param storage Receives the found row, which owns the returned bytes.
 * @return The name, or an empty view when no installed package names the hash.
 */
[[nodiscard]] std::string_view
spawn_set_name(std::uint32_t hash, state::build_data::hash_names::Name& storage) noexcept;

} // namespace sunrise::client::diagnostics::activity_location
