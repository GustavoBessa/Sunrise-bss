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
 * Appends one spoken line to a step and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @param subtitleHash Localized string to speak. Zero is refused, because a line with no words
 * would hold the screen saying nothing.
 * @return True when the ordinal existed, the line fit, and the file was written.
 */
[[nodiscard]] bool append_line(std::size_t index, std::uint32_t subtitleHash) noexcept;

/**
 * Removes one spoken line from a step and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @param line Line ordinal, below that step's line count. Later lines close the gap.
 * @return True when both ordinals existed and the file was written.
 */
[[nodiscard]] bool remove_line(std::size_t index, std::size_t line) noexcept;

/**
 * Replaces one spoken line's time on screen and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @param line Line ordinal, below that step's line count.
 * @param dwellMs Milliseconds, between `kMinimumDwellMs` and `kMaximumDwellMs`.
 * @return True when the ordinals and the dwell were valid and the file was written.
 */
[[nodiscard]] bool set_line_dwell(std::size_t index, std::size_t line, std::uint16_t dwellMs) noexcept;

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
 * Speaks one step's dialogue now, without the player having to reach it.
 *
 * This is the authoring loop: judging a conversation's pacing means hearing it, and hearing it must
 * not cost a walk across the destination for every adjustment. It fires no step and moves no latch.
 *
 * @param index Step ordinal, below the current count.
 * @param now Monotonic tick count in milliseconds.
 * @return True when the ordinal existed and the step had something to say.
 */
[[nodiscard]] bool preview(std::size_t index, std::uint64_t now) noexcept;

/** Stops a preview, or whatever else is being spoken. */
void stop_preview() noexcept;

/**
 * Replaces one roteiro's metadata and saves it.
 * @param author Who authored it. @param description What it covers.
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
 * @return The most recently fired step's announcement, carrying whichever of its lines is currently
 * spoken, or one reporting nothing has fired.
 *
 * The HUD overlay reads this every frame and decides how long to keep the step line on screen, which
 * is why the announcement carries the tick it fired on instead of a countdown. Which line is spoken
 * is decided here rather than there, so the words are fixed the moment they come up.
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

} // namespace sunrise::client::playbook
