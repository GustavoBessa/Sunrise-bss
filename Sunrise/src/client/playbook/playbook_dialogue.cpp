/**
 * The clock that walks a step's spoken lines.
 *
 * Its whole job is to answer "which line is on screen right now", and to answer it the same way for
 * a step reached in the world and for a step being previewed from the interface. That is why the
 * preview needs no code of its own: previewing a beat is starting one without having walked to it.
 */

#include "playbook_dialogue.h"

#include <Windows.h>

#include <algorithm>

#include "../content/strings/subtitle_catalog.h"

namespace sunrise::client::playbook::dialogue {
namespace {

namespace catalog = content::strings::catalog;

SRWLOCK g_lock{SRWLOCK_INIT};
/** Copied from the step, so an edit cannot rewrite what is already being spoken. */
std::array<Line, kDialogueCapacity> g_lines{};
std::uint8_t g_lineCount{};
/** Line on screen, or `g_lineCount` once the beat has finished speaking. */
std::uint8_t g_index{};
/** Tick the line on screen came up on. */
std::uint64_t g_lineTick{};
/** The resolved text of the line on screen, held so it is looked up once and not per frame. */
std::array<char, kSubtitleCapacity> g_text{};
std::uint8_t g_textLength{};
bool g_speaking{};

/** Clears the spoken state. Runs under the lock. */
void silence_locked() noexcept {
    g_lines = {};
    g_lineCount = 0;
    g_index = 0;
    g_lineTick = 0;
    g_text = {};
    g_textLength = 0;
    g_speaking = false;
}

/** Resolves the line at `g_index` into `g_text`. Runs under the lock. */
void resolve_locked() noexcept {
    g_text = {};
    g_textLength = 0;
    if (g_index >= g_lineCount) {
        return;
    }
    catalog::Match match{};
    if (!catalog::text_for(g_lines[g_index].subtitleHash, match)) {
        // A line whose string this install does not carry still holds its slot: the beat's pacing is
        // the author's, and skipping the wait would run the rest of the conversation early.
        return;
    }
    const std::size_t length = (std::min)(static_cast<std::size_t>(match.length), g_text.size());
    std::copy_n(match.text.begin(), length, g_text.begin());
    g_textLength = static_cast<std::uint8_t>(length);
}

/** @param line Line to read. @return Its dwell, clamped into the authorable range. */
[[nodiscard]] std::uint64_t dwell_of(const Line& line) noexcept {
    const std::uint16_t clamped =
        (std::min)((std::max)(line.dwellMs, kMinimumDwellMs), kMaximumDwellMs);
    return static_cast<std::uint64_t>(clamped);
}

} // namespace

/** Starts speaking one step's lines. */
void start(const Step& step, std::uint64_t now) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    silence_locked();
    const std::size_t count =
        (std::min)(static_cast<std::size_t>(step.lineCount), kDialogueCapacity);
    if (count == 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    std::copy_n(step.lines.begin(), count, g_lines.begin());
    g_lineCount = static_cast<std::uint8_t>(count);
    g_index = 0;
    g_lineTick = now;
    g_speaking = true;
    resolve_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

/** Stops speaking. */
void stop() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    silence_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

/** Advances to the line the clock has reached. */
void service(std::uint64_t now) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_speaking) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    // A tick captured before this read reports as no time passed rather than as a very long wait,
    // which is what an unsigned subtraction would turn a backwards clock into.
    while (g_speaking && now >= g_lineTick && now - g_lineTick >= dwell_of(g_lines[g_index])) {
        g_lineTick += dwell_of(g_lines[g_index]);
        ++g_index;
        if (g_index >= g_lineCount) {
            silence_locked();
            break;
        }
        resolve_locked();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Writes the line being spoken into an announcement. */
void fill(Announcement& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    output.subtitle = g_text;
    output.subtitleLength = g_textLength;
    output.lineCount = g_lineCount;
    // One-based, and zero when nothing is up, so the screen can tell "line 1 of 3" from "silent".
    output.lineOrdinal =
        g_speaking && g_index < g_lineCount ? static_cast<std::uint8_t>(g_index + 1) : 0U;
    ReleaseSRWLockShared(&g_lock);
}

/** Reports whether a line is on screen. */
bool speaking() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool value = g_speaking;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

} // namespace sunrise::client::playbook::dialogue
