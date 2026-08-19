#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::server::gameplay {

/**
 * Binds the gameplay endpoint for the configured topology.
 * A disabled topology succeeds without binding, so the rest of the Server is unaffected.
 * @return True when the channel is ready to be advertised, or is deliberately off.
 */
[[nodiscard]] bool initialize() noexcept;

/**
 * Runs one bounded gameplay slice.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/** Stops the endpoint and clears every association and peer. */
void shutdown() noexcept;

/**
 * @return Total number of live actors across all Sunrise-hosted physics worlds.
 *
 * This reflects the Sunrise simulation, not game enemies. The counter is maintained by the
 * world runner on spawn and removal; it reads as zero when no worlds are hosting actors.
 */
[[nodiscard]] std::size_t live_actor_count() noexcept;

/**
 * Called by the world runner each time an actor is spawned or removed.
 * @param delta +1 on spawn, -1 on removal. Saturates at zero on underflow.
 */
void adjust_actor_count(int delta) noexcept;

} // namespace sunrise::server::gameplay
