#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "definition.h"

namespace sunrise::client::playbook::internal {

/** Directory the per-destination roteiro files live in, below the owned artifact directory. */
inline constexpr std::wstring_view kDirectorySuffix = L"\\playbooks";
/** Extension every roteiro file carries. */
inline constexpr std::wstring_view kFileExtension = L".csv";
/**
 * First line of every roteiro file, which also carries the layout version.
 *
 * All three are read, because a roteiro captured against an older build must keep working:
 *  - version 1 has no subtitle column;
 *  - version 2 has one subtitle column, which loads as the step's first spoken line;
 *  - version 3 carries its dialogue on `+` continuation lines and leaves that column empty.
 *
 * Everything written from here on is version 3. A version-3 file read by a version-2 build loses its
 * dialogue, which is the one direction that degrades: `+` lines are not steps, and the reader skips
 * a line it cannot parse rather than failing the file.
 */
inline constexpr std::string_view kMagicV1 = "sunrise_playbook 1";
inline constexpr std::string_view kMagicV2 = "sunrise_playbook 2";
inline constexpr std::string_view kMagicV3 = "sunrise_playbook 3";
/**
 * First byte of a line that adds one spoken line to the step above it.
 * The form is `+,<subtitle_hash>,<dwell_ms>`.
 */
inline constexpr char kLineMarker = '+';
/**
 * First byte of a line that makes the step above it wait on time instead of place.
 * The form is `@,<delay_ms>`. A step with no such line is gated on its captured position, which is
 * what every step written before timed gates existed means.
 */
inline constexpr char kGateMarker = '@';
/** Longest line one step occupies, including its terminator. */
inline constexpr std::size_t kLineCapacity = 256;
/**
 * Largest roteiro file accepted.
 *
 * Every step may carry its full dialogue, and each spoken line is a line of the file. That makes the
 * document too large for the caller's stack, so `load` and `save` hold it on the heap -- the callers
 * here run on the game's own render thread, whose stack this module does not own.
 */
inline constexpr std::size_t kFileCapacity =
    kLineCapacity * (kStepCapacity * (1 + kDialogueCapacity) + 8);
/** One whole roteiro document. */
using Document = std::array<char, kFileCapacity>;

/**
 * Reports one store outcome on the Client channel.
 * @param stage Short key naming the step that failed.
 * @param reason Short key naming why.
 */
void report_fail(const char* stage, const char* reason) noexcept;

/**
 * Builds one destination's file path.
 * @param directory Resolved playbook directory.
 * @param destination Lowercase package name. Any other byte makes this fail rather than write
 * outside the directory.
 * @param output Receives the whole path.
 * @return True when the name is safe and the path fits fixed storage.
 */
[[nodiscard]] bool resolve_path(const core::path::Buffer& directory,
                                std::string_view destination,
                                core::path::Buffer& output) noexcept;

/**
 * Reads one roteiro from disk.
 * @param path Null-terminated file path.
 * @param output Receives the parsed steps. Its destination is left to the caller.
 * @return True when the file is absent, or present with a valid header. A malformed step line is
 * skipped and reported, not fatal, so one bad hand edit cannot cost the whole roteiro.
 */
[[nodiscard]] bool load(const wchar_t* path, Roteiro& output) noexcept;

/**
 * Writes one roteiro to disk, replacing the file.
 * @param path Null-terminated file path.
 * @param value Roteiro to write. The reached latch is runtime state and is not written.
 * @return True when the whole document reached the file.
 */
[[nodiscard]] bool save(const wchar_t* path, const Roteiro& value) noexcept;

} // namespace sunrise::client::playbook::internal
