#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::server {

/** Starts the in-process server surface. */
[[nodiscard]] bool initialize() noexcept;

/** Runs one bounded server service slice. @param now Monotonic tick count. */
void service(std::uint64_t now) noexcept;

/** Stops the in-process server surface. */
void shutdown() noexcept;

/**
 * @return Total number of live actors across all Sunrise-hosted physics worlds.
 *
 * Reflects Sunrise-simulated entities only — not game enemies. Reads as zero until actors are
 * spawned by the Sunrise host and incremented via `gameplay::adjust_actor_count()`.
 */
[[nodiscard]] std::size_t live_actor_count() noexcept;

} // namespace sunrise::server
