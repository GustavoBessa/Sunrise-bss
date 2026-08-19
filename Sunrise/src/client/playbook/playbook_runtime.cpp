/**
 * The mission playbook. It watches the player's location and fires the roteiro's steps as they are
 * reached, once each per run, announcing the first and the last one differently so the start and
 * the end of a roteiro are distinguishable on screen.
 *
 * A sequential roteiro is a script rather than a set of points: a step waits on the one before it,
 * which is also what lets a step be gated on time instead of place. The run clock those waits are
 * measured against is stamped here, on entering the world, because the game exposes none that serves.
 *
 * Nothing plays a sound yet. A step's audio tag is carried and reported; the emit path is the one
 * piece that does not exist, and this is where it plugs in.
 */

#include "playbook.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../server/runtime/server_runtime.h"
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
/** Tick the player entered the world on, which is the run clock's zero. */
std::uint64_t g_runStart{};
/** Whether the player was in a world on the previous slice, so the entry edge can be seen. */
bool g_wasInWorld{};
/** Tick the most recent step fired on, which is what a `delay` gate waits from. */
std::uint64_t g_lastFiredTick{};
/** Set once any step has fired this run, so a `delay` gate has something to follow. */
bool g_anyFired{};
/** Whether the interact key (VK_E) was held on the previous slice, used for leading-edge detect. */
bool g_interactWasDown{};
/**
 * The location sampled on the most recent slice.
 *
 * Kept so the run tracker can measure the distance to the next step without sampling again. The HUD
 * reads the tracker every frame and the slice already sampled this tick, so a second sample would
 * buy nothing but the cost of the nearest-spawn search behind it.
 */
location::Location g_lastSample{};

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
 * Tests whether the player has reached one step's captured position.
 *
 * The nearest-spawn hash is deliberately NOT a term. It is a pure function of position within the
 * map, so given the bubble and the distance test it adds no discrimination at all -- while the
 * boundary between two spawn points can cross a step's radius, which would stop the step from
 * firing with the player standing exactly where it was captured. It is recorded as the step's
 * readable anchor, not used to match.
 *
 * @param step Step to test.
 * @param sampled Current location, already known to be in world.
 * @return True when the player is in the right bubble and close enough to the captured position.
 */
[[nodiscard]] bool at_place(const Step& step, const location::Location& sampled) noexcept {
    return sampled.bubbleValid && sampled.positionPresent
           && step.bubble == static_cast<std::uint32_t>(sampled.bubble)
           && distance_squared(step.position, sampled.position) <= step.radius * step.radius;
}

/**
 * Tests whether one step's wait has elapsed since the previous step fired.
 *
 * A timed step with nothing before it never fires: without a previous step there is no moment to
 * measure from, and firing it on world entry would be a different behaviour from the one authored.
 *
 * @param step Step to test.
 * @param now Monotonic tick count in milliseconds.
 * @return True when a step has fired this run and the wait has passed.
 */
[[nodiscard]] bool after_wait(const Step& step, std::uint64_t now) noexcept {
    const std::uint64_t wait =
        static_cast<std::uint64_t>((std::min)(step.delayMs, kMaximumDelayMs));
    // Unsigned arithmetic, so a tick captured before this read reports as no time passed rather
    // than as a wait long enough to fire everything at once.
    return g_anyFired && now >= g_lastFiredTick && now - g_lastFiredTick >= wait;
}

/**
 * @return True when the interact key (VK_E) has a leading edge this slice.
 *
 * The leading edge is sampled once per service call and cached in `g_interactWasDown`, so multiple
 * steps cannot each consume the same key press. The game's default interact key is E (0x45).
 * The player's actual binding could differ; wiring it through `key_bindings` is a future step.
 */
[[nodiscard]] bool interact_pressed_locked() noexcept {
    constexpr int kInteractVk = 'E';
    const bool down = (GetAsyncKeyState(kInteractVk) & 0x8000) != 0;
    const bool edge = down && !g_interactWasDown;
    g_interactWasDown = down;
    return edge;
}

/**
 * Tests one step for firing. Runs under the lock.
 *
 * In a sequential roteiro only the next unfired step is eligible, which is what makes the roteiro a
 * script: reaching the end of a mission early cannot skip its middle.
 *
 * @param index Step ordinal.
 * @param sampled Current location, already known to be in world.
 * @param now Monotonic tick count in milliseconds.
 * @param interactEdge True when the interact key had a leading edge this slice.
 * @return True when the step should fire on this slice.
 */
[[nodiscard]] bool matches_locked(std::size_t index,
                                 const location::Location& sampled,
                                 std::uint64_t now,
                                 bool interactEdge) noexcept {
    const Step& step = g_roteiro.steps[index];
    if (step.reached) {
        return false;
    }
    if (g_roteiro.sequential && index != 0 && !g_roteiro.steps[index - 1].reached) {
        return false;
    }
    switch (step.gate) {
        case Gate::delay:
            return after_wait(step, now);
        case Gate::interaction:
            return at_place(step, sampled) && interactEdge;
        case Gate::clearArea:
            return server::live_actor_count() <= static_cast<std::size_t>(step.targetActorCount);
        case Gate::place:
        default:
            return at_place(step, sampled);
    }
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
                        const Step& step,
                        std::uint64_t now,
                        Announcement& output) noexcept {
    output = {};
    const std::string_view label = label_of(step);
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

/** Drops every step's reached latch and the run's fire history. */
void rearm_locked() noexcept {
    for (std::size_t index = 0; index < g_roteiro.count; ++index) {
        g_roteiro.steps[index].reached = false;
    }
    // A `delay` gate measures from the previous step of *this* run, so the history goes with it.
    g_lastFiredTick = 0;
    g_anyFired = false;
}

/** Copies authored text into fixed metadata storage, dropping bytes a line cannot carry. */
void store_text(std::string_view source, Metadata& output) noexcept {
    output = {};
    for (const char byte : source) {
        if (output.length >= output.value.size()) {
            break;
        }
        if (byte >= ' ' && byte <= '~' && byte != ',') {
            output.value[output.length++] = byte;
        }
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
    g_runStart = 0;
    g_wasInWorld = false;
    g_lastFiredTick = 0;
    g_anyFired = false;
    g_lastSample = {};
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
    g_runStart = 0;
    g_wasInWorld = false;
    g_lastFiredTick = 0;
    g_anyFired = false;
    g_lastSample = {};
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
    g_lastSample = sampled;
    if (!inWorld) {
        // Leaving the world ends the run, so the roteiro can be walked again on the next entry.
        rearm_locked();
        g_wasInWorld = false;
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (!g_wasInWorld) {
        // The run clock's zero. The game has none that serves, so this edge is where it is stamped.
        g_wasInWorld = true;
        g_runStart = now;
    }
    const std::string_view destination = location::destination_of(sampled);
    if (destination.empty()) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    ensure_destination_locked(destination);
    for (std::size_t index = 0; index < g_roteiro.count; ++index) {
        if (!matches_locked(index, sampled, now)) {
            continue;
        }
        Step& step = g_roteiro.steps[index];
        step.reached = true;
        g_lastFiredTick = now;
        g_anyFired = true;
        report_fired(index + 1, step);
        // Overlapping radii can fire more than one step in a tick. The last one wins the screen,
        // and the log carries every one of them.
        build_announcement(index + 1, g_roteiro.count, step, now, g_announcement);
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
    // These two are the match terms, so a step missing either could never fire. The nearest spawn
    // is only the step's readable anchor, so it is recorded when known and skipped when not.
    if (!sampled.bubbleValid || !sampled.positionPresent) {
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
    step.spawnHash = sampled.spawnFound ? sampled.spawnHash : 0U;
    step.sliceState = sampled.sliceState;
    step.region = sampled.region;
    step.radius = kDefaultRadius;
    step.audioTag = kNoAudioTag;
    // A capture is by definition a place the author stood in, so the gate follows from the act.
    step.gate = Gate::place;
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
    if (g_roteiro.count == 0) {
        // A roteiro being authored from nothing is a mission, so it starts out as a script. One that
        // already had steps keeps whatever its file said, so nothing you already tested changes.
        g_roteiro.sequential = true;
    }
    g_roteiro.steps[g_roteiro.count++] = step;
    const bool saved = save_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return saved;
}

/** Appends one manually authored step and saves the roteiro. */
bool add_step(location::Position position, std::uint32_t bubble, std::string_view label) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_roteiro.count >= kStepCapacity) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    Step step{};
    step.position = position;
    step.bubble = bubble;
    step.gate = Gate::place;
    step.radius = kDefaultRadius;
    const std::size_t labelLength =
        (std::min)(label.size(), kLabelCapacity);
    for (std::size_t i = 0; i < labelLength; ++i) {
        const char ch = label[i];
        if (ch >= 0x20 && ch != ',') {
            step.label[step.labelLength++] = ch;
        }
    }
    if (g_roteiro.count == 0) {
        g_roteiro.sequential = true;
    }
    g_roteiro.steps[g_roteiro.count++] = step;
    const bool saved = save_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return saved;
}

/** Swaps steps at `index` and `index + 1` and saves the roteiro. */
bool move_step_down(std::size_t index) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (index + 1 >= g_roteiro.count) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    std::swap(g_roteiro.steps[index], g_roteiro.steps[index + 1]);
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

/** Replaces what has to happen for one step to fire, and saves the roteiro. */
bool set_gate(std::size_t index, Gate gate, std::uint16_t delayMs) noexcept {
    if (gate == Gate::delay && delayMs > kMaximumDelayMs) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (index >= g_roteiro.count || (gate == Gate::delay && index == 0)) {
        // A timed first step has nothing to wait on, so it would never fire.
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    Step& step = g_roteiro.steps[index];
    step.gate = gate;
    step.delayMs = gate == Gate::delay ? delayMs : 0U;
    const bool saved = save_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return saved;
}

/** Replaces whether the roteiro's steps fire in order, and saves it. */
bool set_sequential(bool sequential) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_roteiro.destinationLength == 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_roteiro.sequential = sequential;
    const bool saved = save_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return saved;
}

/** Copies a metadata string, dropping commas and control bytes. */
static void copy_metadata(std::string_view source, Metadata& target) noexcept {
    target = {};
    for (char ch : source) {
        if (target.length >= kMetadataCapacity) {
            break;
        }
        if (static_cast<unsigned char>(ch) >= 0x20 && ch != ',') {
            target.value[target.length++] = ch;
        }
    }
}

/** Replaces the roteiro's author and description metadata and saves it. */
bool set_metadata(std::string_view author, std::string_view description) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_roteiro.destinationLength == 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    copy_metadata(author, g_roteiro.author);
    copy_metadata(description, g_roteiro.description);
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
    // The clock restarts here too, so a rearm mid-run is a fresh run and not a resumed one.
    g_runStart = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
}

/** Forces the roteiro to be read from disk on the next slice. */
void reload() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_loaded = false;
    g_announcement = {};
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies the most recently fired step's announcement, with the line currently spoken. */
Announcement last_announcement() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Announcement snapshot = g_announcement;
    ReleaseSRWLockShared(&g_lock);
    return snapshot;
}

/** Reports where the run stands and what the roteiro is waiting for next. */
Run run_state(std::uint64_t now) noexcept {
    Run value{};
    AcquireSRWLockShared(&g_lock);
    value.stepCount = g_roteiro.count;
    value.sequential = g_roteiro.sequential;
    value.active = g_wasInWorld && g_roteiro.count != 0;
    value.ageMs = g_wasInWorld && now >= g_runStart ? now - g_runStart : std::uint64_t{0};
    for (std::size_t index = 0; index < g_roteiro.count; ++index) {
        const Step& step = g_roteiro.steps[index];
        if (step.reached) {
            ++value.reached;
            continue;
        }
        if (value.nextOrdinal != 0) {
            continue;
        }
        // The first unfired step. In a sequential roteiro it is the only one that can fire; in a
        // free one it is still the best guess at where the author meant the player to go next.
        value.nextOrdinal = index + 1;
        value.nextLabel = step.label;
        value.nextLabelLength = step.labelLength;
        value.nextIsTimed = step.gate == Gate::delay;
        if (value.nextIsTimed) {
            const std::uint64_t wait =
                static_cast<std::uint64_t>((std::min)(step.delayMs, kMaximumDelayMs));
            const std::uint64_t waited =
                g_anyFired && now >= g_lastFiredTick ? now - g_lastFiredTick : std::uint64_t{0};
            value.nextWaitMs = waited >= wait ? std::uint64_t{0} : wait - waited;
            continue;
        }
        if (g_lastSample.bubbleValid && g_lastSample.positionPresent
            && step.bubble == static_cast<std::uint32_t>(g_lastSample.bubble)) {
            // Only within one bubble: a straight line across a bubble boundary is not a distance the
            // player can walk, and reporting it would send them the wrong way.
            value.nextDistance =
                std::sqrt(distance_squared(step.position, g_lastSample.position));
            value.nextDistanceKnown = true;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return value;
}

/** Reports the route around the player. */
Route route_ahead() noexcept {
    Route value{};
    AcquireSRWLockShared(&g_lock);
    value.stepCount = g_roteiro.count;
    const bool located = g_wasInWorld && g_lastSample.bubbleValid && g_lastSample.positionPresent;
    if (!located || g_roteiro.count == 0) {
        ReleaseSRWLockShared(&g_lock);
        return value;
    }
    const auto bubble = static_cast<std::uint32_t>(g_lastSample.bubble);

    // The waypoint the player is standing nearest to is the route's anchor. It is what makes the
    // markers follow the player back down the path instead of staying at the furthest step reached.
    std::size_t nearest = 0;
    float best = 0.0F;
    for (std::size_t index = 0; index < g_roteiro.count; ++index) {
        const Step& step = g_roteiro.steps[index];
        // A timed beat has no place of its own, and a beat in another bubble is not on this stretch.
        if (step.gate == Gate::delay || step.bubble != bubble) {
            continue;
        }
        const float distance = distance_squared(step.position, g_lastSample.position);
        if (nearest == 0 || distance < best) {
            nearest = index + 1;
            best = distance;
        }
    }
    if (nearest == 0) {
        ReleaseSRWLockShared(&g_lock);
        return value;
    }
    value.nearestOrdinal = nearest;
    value.active = true;

    // Standing on a waypoint that has not fired yet, that waypoint is still the one to reach; having
    // already fired it, the route continues from the next one along.
    const std::size_t anchor =
        g_roteiro.steps[nearest - 1].reached ? nearest : nearest - 1;
    for (std::size_t index = anchor; index < g_roteiro.count && value.count < value.ahead.size();
         ++index) {
        const Step& step = g_roteiro.steps[index];
        if (step.gate == Gate::delay || step.bubble != bubble) {
            continue;
        }
        Waypoint& waypoint = value.ahead[value.count++];
        waypoint.position = step.position;
        waypoint.ordinal = index + 1;
        waypoint.distance = std::sqrt(distance_squared(step.position, g_lastSample.position));
        waypoint.reached = step.reached;
    }
    ReleaseSRWLockShared(&g_lock);
    return value;
}

/** Reports how long the player has been in the world. */
std::uint64_t run_age(std::uint64_t now) noexcept {
    AcquireSRWLockShared(&g_lock);
    // Unsigned arithmetic, so a tick captured before this read reports zero rather than an age of
    // several hundred million years.
    const std::uint64_t age =
        g_wasInWorld && now >= g_runStart ? now - g_runStart : std::uint64_t{0};
    ReleaseSRWLockShared(&g_lock);
    return age;
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
