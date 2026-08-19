#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
 * One located step of a mission roteiro.
 *
 * A step is matched by destination, bubble, nearest-spawn hash, and distance to `position`. The
 * region and slice-set fields are recorded for reference and take no part in the match: the slice
 * set moves with activity progress, so keying on it would stop a step from firing on a replay.
 */
struct Step {
    /** Player position captured here. The match measures distance from this. */
    diagnostics::activity_location::Position position{};
    /** Bubble the step is in. Part of the match. */
    std::uint32_t bubble{};
    /** That bubble's name hash, kept so a step stays readable before the layout loads. */
    std::uint32_t bubbleHash{};
    /** Nearest spawn set's name hash. Part of the match. */
    std::uint32_t spawnHash{};
    /** Slice-set state observed at capture. Reference only. */
    std::uint32_t sliceState{};
    /** Region index observed at capture. Reference only. */
    std::int32_t region{};
    /** How close the player must get, in world units. */
    float radius{kDefaultRadius};
    /** Sound to play here, or `kNoAudioTag` while unknown. Nothing plays yet either way. */
    std::uint32_t audioTag{};
    /** Free text the author wrote for this step. */
    std::array<char, kLabelCapacity> label{};
    std::uint8_t labelLength{};
    /** Set once this step has fired in the current run. Never written to the file. */
    bool reached{};
};

/** One destination's roteiro, in the order its steps were captured. */
struct Roteiro {
    std::array<Step, kStepCapacity> steps{};
    std::size_t count{};
    /** Destination the roteiro belongs to, which is also its file name. */
    std::array<char, state::build_data::scenarios::kNameCapacity> destination{};
    std::uint8_t destinationLength{};
};

/** Bytes of one announcement line, including its null. */
inline constexpr std::size_t kAnnouncementCapacity = 128;

/**
 * The most recently fired step, worded for the screen.
 *
 * The wording lives here rather than in the overlay because the roteiro owns what its first and
 * last step mean. The overlay only decides how long to keep it up.
 */
struct Announcement {
    std::array<char, kAnnouncementCapacity> text{};
    /** Tick the step fired on, which is what the overlay measures its hold against. */
    std::uint64_t firedTick{};
    bool present{};
};

/** @param value Step to read. @return Its label as a bounded view. */
[[nodiscard]] inline std::string_view label_of(const Step& value) noexcept {
    return {value.label.data(), value.labelLength};
}

/** @param value Roteiro to read. @return Its destination name as a bounded view. */
[[nodiscard]] inline std::string_view destination_of(const Roteiro& value) noexcept {
    return {value.destination.data(), value.destinationLength};
}

} // namespace sunrise::client::playbook
