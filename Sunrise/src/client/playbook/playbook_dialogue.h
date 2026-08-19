#pragma once

#include <cstdint>

#include "definition.h"

namespace sunrise::client::playbook::dialogue {

/**
 * The clock that walks a step's spoken lines.
 *
 * A beat fires once and then talks for a while, so something has to decide which of its lines is on
 * screen right now. That belongs here rather than in the overlay for two reasons: the overlay is
 * redrawn per frame and must stay free of state, and resolving each line's text once when it comes
 * up is what keeps the words from changing under a catalog rebuild mid-sentence.
 *
 * The lines are copied in on `start`, so editing a step in the interface cannot rewrite what is
 * already being spoken.
 *
 * One beat is spoken at a time. A step that fires while another is talking takes the screen, because
 * the newer beat is where the player now is.
 */

/**
 * Starts speaking one step's lines.
 * A step with no lines stops whatever was speaking and leaves the screen quiet.
 * @param step Step whose dialogue to speak. Its lines are copied.
 * @param now Monotonic tick count in milliseconds.
 */
void start(const Step& step, std::uint64_t now) noexcept;

/** Stops speaking, clearing the screen of dialogue. */
void stop() noexcept;

/**
 * Advances to the line the clock has reached.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/**
 * Writes the line being spoken into an announcement.
 * @param output Announcement receiving the line, its ordinal, and the beat's line count. Its
 * dialogue fields are cleared when nothing is being spoken, so a stale line cannot outlive its beat.
 */
void fill(Announcement& output) noexcept;

/** @return True while a line is on screen. */
[[nodiscard]] bool speaking() noexcept;

} // namespace sunrise::client::playbook::dialogue
