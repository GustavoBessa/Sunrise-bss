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
 * Replaces one step's fire radius and saves the roteiro.
 * @param index Step ordinal, below the current count.
 * @param radius World units, between `kMinimumRadius` and `kMaximumRadius`.
 * @return True when the ordinal and the radius were valid and the file was written.
 */
[[nodiscard]] bool set_radius(std::size_t index, float radius) noexcept;

/** Clears every step's reached latch so the roteiro can be walked again from the start. */
void rearm() noexcept;

/**
 * @return The most recently fired step's announcement, or one reporting nothing has fired.
 *
 * The HUD overlay reads this every frame and decides how long to keep it on screen, which is why
 * the announcement carries the tick it fired on instead of a countdown.
 */
[[nodiscard]] Announcement last_announcement() noexcept;

/** @return Steps already reached in the current run. */
[[nodiscard]] std::size_t reached_count() noexcept;

} // namespace sunrise::client::playbook
