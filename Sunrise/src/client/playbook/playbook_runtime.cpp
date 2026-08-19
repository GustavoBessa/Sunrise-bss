/**
 * The mission playbook. It watches the player's location and fires the roteiro's steps as they are
 * reached, once each per run, announcing the first and the last one differently so the start and
 * the end of a roteiro are distinguishable on screen.
 *
 * Nothing plays a sound yet. A step's audio tag is carried and reported; the emit path is the one
 * piece that does not exist, and this is where it plugs in.
 */

#include "playbook.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

#include "../../core/logging/log.h"
#include "../diagnostics/activity_location.h"
#include "internal.h"

namespace sunrise::client::playbook {
namespace {

namespace location = diagnostics::activity_location;

SRWLOCK g_lock{SRWLOCK_INIT};
Roteiro g_roteiro{};
Announcement g_announcement{};
core::path::Buffer g_directory{};
bool g_directoryResolved{};
/** Set once a destination's file was looked for, so a missing file is not retried per frame. */
bool g_loaded{};

/** Writes one step's file path into caller storage. @return True when the path was built. */
[[nodiscard]] bool current_path(std::string_view destination, core::path::Buffer& output) noexcept {
    return g_directoryResolved && internal::resolve_path(g_directory, destination, output);
}

/** Saves the loaded roteiro. @return True when the whole file was written. */
[[nodiscard]] bool save_locked() noexcept {
    core::path::Buffer path{};
    if (!current_path(destination_of(g_roteiro), path)) {
        internal::report_fail("save", "path");
        return false;
    }
    return internal::save(path.chars.data(), g_roteiro);
}

/**
 * Points the loaded roteiro at one destination, loading its file the first time it is seen.
 * @param destination Package name the player is currently in.
 */
void ensure_destination_locked(std::string_view destination) noexcept {
    if (g_loaded && destination == destination_of(g_roteiro)) {
        return;
    }
    g_roteiro = {};
    const std::size_t length = (std::min)(destination.size(), g_roteiro.destination.size());
    std::copy_n(destination.begin(), length, g_roteiro.destination.begin());
    g_roteiro.destinationLength = static_cast<std::uint8_t>(length);
    g_loaded = true;
    core::path::Buffer path{};
    if (!current_path(destination, path)) {
        return;
    }
    // File I/O on the caller thread, which is the game's own pump. It runs only when the
    // destination changes, not per frame.
    (void)internal::load(path.chars.data(), g_roteiro);
}

/** @return Squared distance between two world positions, which avoids a square root. */
[[nodiscard]] float distance_squared(const location::Position& left,
                                     const location::Position& right) noexcept {
    float total = 0.0F;
    for (std::size_t lane = 0; lane < location::kPositionLanes; ++lane) {
        const float delta = left[lane] - right[lane];
        total += delta * delta;
    }
    return total;
}

/**
 * Tests one step against a sampled location.
 * @param step Step to test.
 * @param sampled Current location, already known to be in world.
 * @return True when the player is close enough, in the right bubble, by the right spawn.
 */
[[nodiscard]] bool matches(const Step& step, const location::Location& sampled) noexcept {
    return !step.reached && sampled.bubbleValid && sampled.spawnFound && sampled.positionPresent
           && step.bubble == static_cast<std::uint32_t>(sampled.bubble)
           && step.spawnHash == sampled.spawnHash
           && distance_squared(step.position, sampled.position) <= step.radius * step.radius;
}

/**
 * Words one fired step for the screen.
 * The first and last steps of a roteiro are named as its start and its end, which is what makes a
 * roteiro's boundaries visible without counting rows.
 * @param ordinal One-based step position.
 * @param count Steps in the roteiro.
 * @param label Step label, which may be empty.
 * @param now Tick the step fired on.
 * @param output Receives the announcement.
 */
void build_announcement(std::size_t ordinal,
                        std::size_t count,
                        std::string_view label,
                        std::uint64_t now,
                        Announcement& output) noexcept {
    output = {};
    const char* prefix = "Step";
    if (ordinal == 1) {
        prefix = "Start";
    } else if (ordinal == count) {
        prefix = "End";
    }
    if (label.empty()) {
        (void)std::snprintf(
            output.text.data(), output.text.size(), "%s %zu/%zu", prefix, ordinal, count);
    } else {
        (void)std::snprintf(output.text.data(),
                            output.text.size(),
                            "%s %zu/%zu - %.*s",
                            prefix,
                            ordinal,
                            count,
                            static_cast<int>(label.size()),
                            label.data());
    }
    output.firedTick = now;
    output.present = true;
}

/** Reports one fired step, including the sound it names but nothing can play yet. */
void report_fired(std::size_t ordinal, const Step& step) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=playbook stage=match step=%zu bubble=%u spawn=0x%08X "
                                      "audio=0x%08X result=fired",
                                      ordinal,
                                      static_cast<unsigned>(step.bubble),
                                      static_cast<unsigned>(step.spawnHash),
                                      static_cast<unsigned>(step.audioTag));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Drops every step's reached latch. */
void rearm_locked() noexcept {
    for (std::size_t index = 0; index < g_roteiro.count; ++index) {
        g_roteiro.steps[index].reached = false;
    }
}

/** @param value Byte from an authored label. @return True when it may be stored. */
[[nodiscard]] bool label_byte(char value) noexcept {
    // A comma would shift the file's columns and a control byte would break the line, so the label
    // keeps only printable ASCII without separators.
    return value >= ' ' && value <= '~' && value != ',';
}

} // namespace

/** Resolves the playbook directory and creates it. */
void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_roteiro = {};
    g_announcement = {};
    g_loaded = false;
    g_directory = {};
    g_directoryResolved = core::path::artifact_directory(module, g_directory)
                          && core::path::append(g_directory, internal::kDirectorySuffix);
    if (!g_directoryResolved) {
        internal::report_fail("initialize", "path");
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (CreateDirectoryW(g_directory.chars.data(), nullptr) == FALSE
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_directoryResolved = false;
        internal::report_fail("initialize", "directory");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Drops the loaded roteiro and the resolved directory. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_roteiro = {};
    g_announcement = {};
    g_loaded = false;
    g_directory = {};
    g_directoryResolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Samples the player's location and fires the steps they have reached. */
void service(std::uint64_t now) noexcept {
    location::Location sampled{};
    // Sampled before the lock: it reads published State and never touches playbook storage.
    const bool inWorld = location::sample(sampled);

    AcquireSRWLockExclusive(&g_lock);
    if (!inWorld) {
        // Leaving the world ends the run, so the roteiro can be walked again on the next entry.
        rearm_locked();
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    const std::string_view destination = location::destination_of(sampled);
    if (destination.empty()) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    ensure_destination_locked(destination);
    for (std::size_t index = 0; index < g_roteiro.count; ++index) {
        Step& step = g_roteiro.steps[index];
        if (!matches(step, sampled)) {
            continue;
        }
        step.reached = true;
        report_fired(index + 1, step);
        // Overlapping radii can fire more than one step in a tick. The last one wins the screen,
        // and the log carries every one of them.
        build_announcement(index + 1, g_roteiro.count, label_of(step), now, g_announcement);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies the loaded roteiro. */
Roteiro get() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Roteiro snapshot = g_roteiro;
    ReleaseSRWLockShared(&g_lock);
    return snapshot;
}

/** Appends one step at the player's current location and saves the roteiro. */
bool capture(std::string_view label) noexcept {
    location::Location sampled{};
    if (!location::sample(sampled)) {
        return false;
    }
    // Every one of these is part of the match, so a step missing any of them could never fire.
    if (!sampled.bubbleValid || !sampled.spawnFound || !sampled.positionPresent) {
        internal::report_fail("capture", "location");
        return false;
    }
    const std::string_view destination = location::destination_of(sampled);
    if (destination.empty()) {
        internal::report_fail("capture", "destination");
        return false;
    }

    Step step{};
    step.position = sampled.position;
    step.bubble = static_cast<std::uint32_t>(sampled.bubble);
    step.bubbleHash = sampled.bubbleHash;
    step.spawnHash = sampled.spawnHash;
    step.sliceState = sampled.sliceState;
    step.region = sampled.region;
    step.radius = kDefaultRadius;
    step.audioTag = kNoAudioTag;
    // A captured step counts as reached: the player is standing on it, and announcing it now would
    // fire the roteiro's own start the moment it is authored.
    step.reached = true;
    for (const char byte : label) {
        if (step.labelLength >= step.label.size()) {
            break;
        }
        if (label_byte(byte)) {
            step.label[step.labelLength++] = byte;
        }
    }

    AcquireSRWLockExclusive(&g_lock);
    ensure_destination_locked(destination);
    if (g_roteiro.count >= g_roteiro.steps.size()) {
        ReleaseSRWLockExclusive(&g_lock);
        internal::report_fail("capture", "capacity");
        return false;
    }
    g_roteiro.steps[g_roteiro.count++] = step;
    const bool saved = save_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return saved;
}

/** Removes one step and saves the roteiro. */
bool remove_step(std::size_t index) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (index >= g_roteiro.count) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    for (std::size_t at = index + 1; at < g_roteiro.count; ++at) {
        g_roteiro.steps[at - 1] = g_roteiro.steps[at];
    }
    g_roteiro.steps[--g_roteiro.count] = {};
    const bool saved = save_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return saved;
}

/** Replaces one step's sound reference and saves the roteiro. */
bool set_audio_tag(std::size_t index, std::uint32_t audioTag) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (index >= g_roteiro.count) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_roteiro.steps[index].audioTag = audioTag;
    const bool saved = save_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return saved;
}

/** Replaces one step's fire radius and saves the roteiro. */
bool set_radius(std::size_t index, float radius) noexcept {
    if (radius < kMinimumRadius || radius > kMaximumRadius) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (index >= g_roteiro.count) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_roteiro.steps[index].radius = radius;
    const bool saved = save_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return saved;
}

/** Clears every step's reached latch. */
void rearm() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    rearm_locked();
    // The run starts over, so the step still on screen no longer describes it.
    g_announcement = {};
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies the most recently fired step's announcement. */
Announcement last_announcement() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Announcement snapshot = g_announcement;
    ReleaseSRWLockShared(&g_lock);
    return snapshot;
}

/** Counts the steps already reached in the current run. */
std::size_t reached_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    std::size_t reached = 0;
    for (std::size_t index = 0; index < g_roteiro.count; ++index) {
        reached += g_roteiro.steps[index].reached ? 1U : 0U;
    }
    ReleaseSRWLockShared(&g_lock);
    return reached;
}

} // namespace sunrise::client::playbook
