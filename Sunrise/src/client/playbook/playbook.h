#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "definition.h"

namespace sunrise::client::playbook {

/**
 * Resolves the playbook directory and creates it.
 * @param module Loaded DLL used to resolve the owned artifact directory.
 */
void initialize(void* module) noexcept;

/** Drops the loaded roteiro and the resolved directory. */
void shutdown() noexcept;

/**
 * Runs one bounded matching slice on the caller thread.
 *
 * It samples the player's location, loads the roteiro of a destination it has not seen yet, and
 * fires the steps the player has reached. A step fires once per run.
 *
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/** @return A copy of the loaded roteiro, taken under the lock. */
[[nodiscard]] Roteiro get() noexcept;

/**
 * Appends one step at the player's current location and saves the roteiro.
 * @param label Free text for the step. Commas and control bytes are dropped, because the file is
 * comma separated and one step is one line.
 * @return True when the location was usable, the step fit, and the file was written.
 */
[[nodiscard]] bool capture(std::string_view label) noexcept;

/**
 * Appends one step with manually supplied position data and saves the roteiro.
 *
 * This is the authoring path for destinations the author has not physically visited in the current
 * session. The step gets `Gate::place` and the default radius; the caller supplies the rest.
 *
 * @param position World-space XYZ.
 * @param bubble Bubble index the position is in.
 * @param label Free text. Commas and control bytes are dropped.
 * @return True when the step fit the roteiro and the file was written.
 */
[[nodiscard]] bool add_step(diagnostics::activity_location::Position position,
                            std::uint32_t bubble,
                            std::string_view label) noexcept;

/**
 * Swaps two adjacent steps and saves the roteiro.
 * @param index Lower of the two ordinals. Must be below `count - 1`.
 * @return True when both ordinals were valid and the file was written.
 */
[[nodiscard]] bool move_step_down(std::size_t index) noexcept;

/**
 * Removes one step and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @return True when the ordinal existed and the file was written.
 */
[[nodiscard]] bool remove_step(std::size_t index) noexcept;

/**
 * Replaces one step's sound reference and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @param audioTag Sound to play here, or `kNoAudioTag` to clear it.
 * @return True when the ordinal existed and the file was written.
 */
[[nodiscard]] bool set_audio_tag(std::size_t index, std::uint32_t audioTag) noexcept;

/**
 * Replaces what has to happen for one step to fire, and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @param gate `Gate::place` for the captured position, `Gate::delay` for a wait after the previous
 * step. A `delay` gate on the first step of a roteiro is refused: there is nothing for it to follow.
 * @param delayMs Wait for a `delay` gate, up to `kMaximumDelayMs`. Ignored for `place`.
 * @return True when the ordinal and the gate were valid and the file was written.
 */
[[nodiscard]] bool set_gate(std::size_t index, Gate gate, std::uint16_t delayMs) noexcept;

/**
 * Replaces whether the roteiro's steps fire in order, and saves it.
 * @param sequential True to make each step wait on the one before it.
 * @return True when the file was written.
 */
[[nodiscard]] bool set_sequential(bool sequential) noexcept;

/**
 * Replaces the roteiro's author and description metadata and saves it.
 * @param author Author name. Commas and control bytes are dropped.
 * @param description What the roteiro covers. Commas and control bytes are dropped.
 * @return True when the file was written.
 */
[[nodiscard]] bool set_metadata(std::string_view author, std::string_view description) noexcept;

/**
 * Replaces one step's fire radius and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @param radius World units, between `kMinimumRadius` and `kMaximumRadius`.
 * @return True when the ordinal and the radius were valid and the file was written.
 */
[[nodiscard]] bool set_radius(std::size_t index, float radius) noexcept;

/**
 * Replaces one step's actor-count threshold for a `clearArea` gate and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @param target How many actors may remain for the step to fire. Zero means "all cleared".
 * @return True when the ordinal existed and the file was written.
 */
[[nodiscard]] bool set_target_actors(std::size_t index, std::uint16_t target) noexcept;

/**
 * Replaces one step's objective text and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @param text Text shown while this step is next. Trimmed to `kStepTextCapacity`.
 * @return True when the ordinal existed and the file was written.
 */
[[nodiscard]] bool set_objective_text(std::size_t index, std::string_view text) noexcept;

/**
 * Replaces one step's completion text and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @param text Text shown when this step fires. Trimmed to `kStepTextCapacity`.
 * @return True when the ordinal existed and the file was written.
 */
[[nodiscard]] bool set_completion_text(std::size_t index, std::string_view text) noexcept;

/** Clears every step's reached latch so the roteiro can be walked again from the start. */
void rearm() noexcept;

/**
 * Forces the loaded roteiro to be read from disk on the next slice.
 *
 * The runtime only reloads when the destination changes, so anything that rewrites the current
 * destination's file behind its back has to say so.
 */
void reload() noexcept;

/**
 * @return The most recently fired step's announcement, or one reporting nothing has fired.
 *
 * The HUD overlay reads this every frame and decides how long to keep it on screen, which is why the
 * announcement carries the tick it fired on instead of a countdown.
 */
[[nodiscard]] Announcement last_announcement() noexcept;

/** @return Steps already reached in the current run. */
[[nodiscard]] std::size_t reached_count() noexcept;

/**
 * @param now Monotonic tick count in milliseconds.
 * @return Milliseconds since the player entered the world, or zero when they are not in one.
 *
 * The clock is stamped by this module on entering the world, because the game has none that serves:
 * `world_transition_age()` measures the loading screen, and `world_phase()` latches at `arrived` and
 * stays there in orbit.
 */
[[nodiscard]] std::uint64_t run_age(std::uint64_t now) noexcept;

/**
 * Where the run currently stands, and what the roteiro is waiting for next.
 *
 * A roteiro that only announces beats after the fact can be authored but not followed: to walk a
 * mission you have to know which beat comes next and where it is. This is what the HUD tracker and
 * the page's run block are drawn from.
 */
struct Run {
    /** Steps the roteiro holds. */
    std::size_t stepCount{};
    /** Steps already fired this run. */
    std::size_t reached{};
    /** One-based position of the next unfired step, or zero when there is none left. */
    std::size_t nextOrdinal{};
    /** That step's label, which is the only human-readable name a beat has. */
    std::array<char, kLabelCapacity> nextLabel{};
    std::uint8_t nextLabelLength{};
    /** How far the player is from the next step, in world units. */
    float nextDistance{};
    /**
     * The next step is a place and the player is in its bubble, so `nextDistance` means something.
     * A step in another bubble has no useful straight-line distance to report.
     */
    bool nextDistanceKnown{};
    /** The next step waits on time rather than place. */
    bool nextIsTimed{};
    /** Milliseconds still to wait for a timed next step. */
    std::uint64_t nextWaitMs{};
    /** Milliseconds since the player entered the world. */
    std::uint64_t ageMs{};
    /** The roteiro's steps fire in order, so `nextOrdinal` is binding rather than advisory. */
    bool sequential{};
    /** The player is in a world with a roteiro loaded for it. */
    bool active{};
};

/**
 * @param now Monotonic tick count in milliseconds.
 * @return The run's standing. Its distance is measured against the location sampled on the most
 * recent slice rather than a fresh one, so reading this every frame costs no extra sampling.
 */
[[nodiscard]] Run run_state(std::uint64_t now) noexcept;

/** Waypoints of the route the marker draws ahead of the player. */
inline constexpr std::size_t kRouteAheadCapacity = 4;

/** One waypoint of the route. */
struct Waypoint {
    diagnostics::activity_location::Position position{};
    /** One-based position in the roteiro. */
    std::size_t ordinal{};
    /** Straight-line distance from the player, in world units. */
    float distance{};
    /** The step has already fired in this run. */
    bool reached{};
};

/**
 * The stretch of the route around where the player is standing.
 *
 * A roteiro is a linear path, and guidance along it is driven by **where the player is**, not by what
 * has already fired. Those differ the moment the player turns around: with a latch-driven marker,
 * walking back down the route leaves the marker pinned to the far end, which is precisely when a
 * marker is least useful. Anchoring on the nearest waypoint instead makes the markers walk back with
 * the player, so the path stays readable in both directions.
 *
 * Firing is untouched by this: a step still fires once per run, and a sequential roteiro still holds
 * its order. This decides only what the player is *shown*.
 */
struct Route {
    /** Waypoints from the player's place on the route forwards, nearest first. */
    std::array<Waypoint, kRouteAheadCapacity> ahead{};
    std::size_t count{};
    /** One-based waypoint the player is standing nearest to, or zero when none is in this bubble. */
    std::size_t nearestOrdinal{};
    /** Steps the roteiro holds. */
    std::size_t stepCount{};
    /** The player is in a world with a roteiro whose route reaches this bubble. */
    bool active{};
};

/**
 * @return The route around the player. Only waypoints in the bubble the player is in are reported:
 * a straight line across a bubble boundary points through the map rather than along it.
 *
 * Measured against the location sampled on the most recent slice, so drawing this every frame costs
 * no extra sampling.
 */
[[nodiscard]] Route route_ahead() noexcept;

} // namespace sunrise::client::playbook
