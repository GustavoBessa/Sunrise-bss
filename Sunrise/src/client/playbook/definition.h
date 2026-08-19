#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../diagnostics/activity_location.h"

namespace sunrise::client::playbook {

/** Steps one roteiro holds. A mission longer than this is split across destinations. */
inline constexpr std::size_t kStepCapacity = 64;
/** Bytes of one step's free-text label, without a null. */
inline constexpr std::size_t kLabelCapacity = 64;
/** How close the player must get for a step to fire, in world units. */
inline constexpr float kDefaultRadius = 8.0F;
/** Below this a step is unreachable in practice, because the position is sampled per frame. */
inline constexpr float kMinimumRadius = 1.0F;
/** Above this one step would swallow its neighbours. */
inline constexpr float kMaximumRadius = 200.0F;
/** A step whose sound is not identified yet. Nothing plays for it. */
inline constexpr std::uint32_t kNoAudioTag = 0;
/**
 * Spoken lines one step carries.
 *
 * A step is one beat of the mission, and a beat is a short exchange rather than a whole scene: a
 * conversation longer than this is authored as the next beat, which also gives it its own anchor.
 */
inline constexpr std::size_t kDialogueCapacity = 8;
/** How long one line stays on screen by default, in milliseconds. */
inline constexpr std::uint16_t kDefaultDwellMs = 3500;
/** Below this a line is gone before it can be read. */
inline constexpr std::uint16_t kMinimumDwellMs = 500;
/** Above this one line would outlast the beat that carries it. */
inline constexpr std::uint16_t kMaximumDwellMs = 20000;
/** Longest wait a timed step may carry, in milliseconds. */
inline constexpr std::uint16_t kMaximumDelayMs = 60000;
/** What a step waits by default once it is made timed, in milliseconds. */
inline constexpr std::uint16_t kDefaultDelayMs = 2000;

/** What has to happen for a step to fire. */
enum class Gate : std::uint8_t {
    /**
     * The player has to reach the captured position. This is the only gate a captured step gets,
     * because a capture is by definition a place the author stood in.
     */
    place = 0,
    /**
     * The previous step has to have fired `delayMs` ago.
     *
     * This is what lets dialogue run while the player walks: a conversation cannot be pinned to
     * coordinates, because nobody covers the same ground at the same pace twice. It only means
     * anything in a sequential roteiro, where "the previous step" is well defined.
     */
    delay = 1,
};

/**
 * One spoken line of a step's dialogue.
 *
 * The hash names a localized string in the installed game and the words are resolved from the
 * subtitle catalog at playback, so a shared roteiro carries references and never the game's text.
 */
struct Line {
    std::uint32_t subtitleHash{};
    /** How long this line holds the screen before the next one takes it. */
    std::uint16_t dwellMs{kDefaultDwellMs};
};

/**
 * One beat of a mission roteiro: where it happens, and what is said there.
 *
 * A `place` step is matched by destination, bubble, and distance to `position` -- and, in a
 * sequential roteiro, by the previous step having fired. Nothing else is a match term.
 * Every other field is recorded for reference:
 *  - the slice set moves with activity progress, so keying on it would stop a step from firing on
 *    a replay;
 *  - the nearest-spawn hash is a pure function of position within the map, so it adds no
 *    discrimination over the distance test, while the boundary between two spawn points crossing a
 *    step's radius would stop it firing where it was captured.
 */
struct Step {
    /** Player position captured here. The match measures distance from this. */
    diagnostics::activity_location::Position position{};
    /** Bubble the step is in. Part of the match. */
    std::uint32_t bubble{};
    /** That bubble's name hash, kept so a step stays readable before the layout loads. */
    std::uint32_t bubbleHash{};
    /** Nearest spawn set's name hash, or zero when none was known. The step's readable anchor. */
    std::uint32_t spawnHash{};
    /** Slice-set state observed at capture. Reference only. */
    std::uint32_t sliceState{};
    /** Region index observed at capture. Reference only. */
    std::int32_t region{};
    /** How close the player must get, in world units. Only read for a `place` gate. */
    float radius{kDefaultRadius};
    /** Sound to play here, or `kNoAudioTag` while unknown. Nothing plays yet either way. */
    std::uint32_t audioTag{};
    /** The dialogue spoken at this beat, in the order it is heard. */
    std::array<Line, kDialogueCapacity> lines{};
    std::uint8_t lineCount{};
    /** Wait after the previous step fired. Only read for a `delay` gate. */
    std::uint16_t delayMs{};
    /** What has to happen for this step to fire. */
    Gate gate{Gate::place};
    /** Free text the author wrote for this step. */
    std::array<char, kLabelCapacity> label{};
    std::uint8_t labelLength{};
    /** Set once this step has fired in the current run. Never written to the file. */
    bool reached{};
};

/** @param value Step to read. @return Its dialogue as a bounded view. */
[[nodiscard]] inline std::span<const Line> lines_of(const Step& value) noexcept {
    return {value.lines.data(), value.lineCount};
}

/** Bytes of one metadata value, without a null. */
inline constexpr std::size_t kMetadataCapacity = 96;

/** One free-text metadata value carried by a shared roteiro. */
struct Metadata {
    std::array<char, kMetadataCapacity> value{};
    std::uint8_t length{};
};

/** @param value Metadata to read. @return Its text as a bounded view. */
[[nodiscard]] inline std::string_view value_of(const Metadata& value) noexcept {
    return {value.value.data(), value.length};
}

/** One destination's roteiro, in the order its steps were captured. */
struct Roteiro {
    std::array<Step, kStepCapacity> steps{};
    std::size_t count{};
    /**
     * Steps fire in order, each waiting on the one before it.
     *
     * This is what separates a mission from a set of points, and it is what a `delay` gate needs to
     * mean anything. It is off by default so that a roteiro captured before ordering existed keeps
     * behaving exactly as it was tested; the file says `# order sequential` when it is on.
     */
    bool sequential{};
    /** Destination the roteiro belongs to, which is also its file name. */
    std::array<char, state::build_data::scenarios::kNameCapacity> destination{};
    std::uint8_t destinationLength{};
    /** Who authored it. Carried so a shared roteiro keeps its credit. */
    Metadata author{};
    /** What it covers, in the author's words. */
    Metadata description{};
    /** Game build it was captured against, which is what makes a mismatch explainable. */
    Metadata gameBuild{};
};

/** Bytes of one announcement line, including its null. */
inline constexpr std::size_t kAnnouncementCapacity = 128;
/** Bytes of one shown subtitle, without a null. Long enough for a line of dialogue. */
inline constexpr std::size_t kSubtitleCapacity = 200;

/**
 * The most recently fired step, worded for the screen, with whichever line is currently spoken.
 *
 * The wording lives here rather than in the overlay because the roteiro owns what its first and
 * last step mean. The line is chosen here too, by the runtime's own clock, so the text is fixed at
 * the moment it came up and cannot change under a catalog rebuild while it is on screen. The
 * overlay only decides how long the step line stays up.
 */
struct Announcement {
    std::array<char, kAnnouncementCapacity> text{};
    /** The line being spoken now, resolved when the catalog holds it. Empty otherwise. */
    std::array<char, kSubtitleCapacity> subtitle{};
    std::uint8_t subtitleLength{};
    /** One-based position of that line in its beat, or zero when no line is up. */
    std::uint8_t lineOrdinal{};
    /** Lines the beat carries, so the screen can say how far into it the player is. */
    std::uint8_t lineCount{};
    /** Tick the step fired on, which is what the overlay measures its hold against. */
    std::uint64_t firedTick{};
    bool present{};
};

/** @param value Announcement to read. @return Its subtitle as a bounded view. */
[[nodiscard]] inline std::string_view subtitle_of(const Announcement& value) noexcept {
    return {value.subtitle.data(), value.subtitleLength};
}

/** @param value Step to read. @return Its label as a bounded view. */
[[nodiscard]] inline std::string_view label_of(const Step& value) noexcept {
    return {value.label.data(), value.labelLength};
}

/** @param value Roteiro to read. @return Its destination name as a bounded view. */
[[nodiscard]] inline std::string_view destination_of(const Roteiro& value) noexcept {
    return {value.destination.data(), value.destinationLength};
}

} // namespace sunrise::client::playbook
