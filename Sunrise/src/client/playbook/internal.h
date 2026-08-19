#pragma once

#include <cstddef>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "definition.h"

namespace sunrise::client::playbook::internal {

/** Directory the per-destination roteiro files live in, below the owned artifact directory. */
inline constexpr std::wstring_view kDirectorySuffix = L"\\playbooks";
/** Extension every roteiro file carries. */
inline constexpr std::wstring_view kFileExtension = L".csv";
/** First line of every roteiro file, which also carries the layout version. */
inline constexpr std::string_view kMagic = "sunrise_playbook 1";
/** Longest line one step occupies, including its terminator. */
inline constexpr std::size_t kLineCapacity = 256;
/** Largest roteiro file accepted into fixed storage. The extra lines cover the header. */
inline constexpr std::size_t kFileCapacity = kLineCapacity * (kStepCapacity + 4);

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
