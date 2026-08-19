#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::client::content::strings::catalog {

/** Bytes of one match's text handed to a caller, without a null. Longer text is cut. */
inline constexpr std::size_t kTextCapacity = 200;

/** One catalogued string, copied out for display. */
struct Match {
    std::uint32_t hash{};
    std::array<char, kTextCapacity> text{};
    std::uint8_t length{};
};

/** How far a build has got, and whether a catalog can be searched. */
struct Progress {
    /** Containers the build has to walk. Zero before the container sweep finishes. */
    std::size_t containers{};
    std::size_t containersDone{};
    /** Strings catalogued so far. */
    std::size_t rows{};
    /** A build is running and consuming slices. */
    bool building{};
    /** A catalog is loaded and searchable. */
    bool ready{};
    /** The last build or load failed. `reason` says which step. */
    bool failed{};
};

/**
 * Resolves the cache path.
 * @param module Loaded DLL used to resolve the owned artifact directory.
 */
void initialize(void* module) noexcept;

/** Drops the catalog and the resolved path. */
void shutdown() noexcept;

/**
 * Runs one bounded slice of work on the caller thread.
 *
 * On the first call it loads the cache from disk when one is present. While a build is running it
 * walks one string container per slice, because the walk decompresses package blocks and a whole
 * sweep in one call would stall the frame it runs on.
 *
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/**
 * Starts a build, discarding the current catalog.
 *
 * It needs the game's package block keys, so it only works in a running game. The build writes the
 * cache when it finishes.
 */
void rebuild() noexcept;

/** @return How far the build has got, and whether the catalog can be searched. */
[[nodiscard]] Progress progress() noexcept;

/** @return Short key naming the step that failed, or an empty view. */
[[nodiscard]] std::string_view failure() noexcept;

/**
 * Copies the catalogued strings whose text contains one term, case insensitively.
 * @param needle Search term. An empty term matches nothing, because every string would.
 * @param output Caller-owned fixed match storage.
 * @return Matches written, which stops at the output's size.
 */
[[nodiscard]] std::size_t search(std::string_view needle, std::span<Match> output) noexcept;

/**
 * Resolves one string hash to its catalogued text.
 * @param hash String hash, as a playbook step stores it.
 * @param output Receives the text and its length.
 * @return True when the catalog is ready and holds that hash.
 */
[[nodiscard]] bool text_for(std::uint32_t hash, Match& output) noexcept;

} // namespace sunrise::client::content::strings::catalog
