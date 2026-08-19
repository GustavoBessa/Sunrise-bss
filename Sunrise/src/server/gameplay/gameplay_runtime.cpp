#include "gameplay_runtime.h"

#include <atomic>
#include <cstddef>

#include "association/association_host.h"
#include "dtls/dtls_host.h"
#include "endpoint/gameplay_endpoint.h"
#include "group/group_host.h"
#include "peer/peer_transport.h"
#include "physics/host/physics_session.h"
#include "physics/host/runtime.h"

namespace sunrise::server::gameplay {
namespace {

std::atomic<std::size_t> g_actorCount{0};

} // namespace

/** Binds the gameplay endpoint for the configured topology. */
bool initialize() noexcept {
    association::reset();
    dtls::reset();
    peer::reset();
    group::reset();
    if (!endpoint::initialize()) {
        return false;
    }
    // The endpoint binds first. No transport path reaches the host yet, so a host that cannot
    // allocate must not stop the channel from carrying everything that does not need one.
    static_cast<void>(physics::host::runtime::initialize());
    return true;
}

/** Runs one bounded gameplay slice. */
void service(std::uint64_t now) noexcept {
    endpoint::service(now);
    // The retries run before the send so anything they queue leaves in this slice.
    group::service(now);
    peer::service(now);
    // Last, because it reads the admitted set the two calls above have already settled. It emits
    // nothing on the wire, so its position cannot delay a queued send.
    physics::host::session::service(now);
}

/** @return Total live actor count across all hosted worlds. */
std::size_t live_actor_count() noexcept {
    return g_actorCount.load(std::memory_order_relaxed);
}

/** Adjusts the live actor counter on spawn (+1) or removal (-1). */
void adjust_actor_count(int delta) noexcept {
    if (delta > 0) {
        g_actorCount.fetch_add(static_cast<std::size_t>(delta), std::memory_order_relaxed);
    } else if (delta < 0) {
        const std::size_t sub = static_cast<std::size_t>(-delta);
        std::size_t current = g_actorCount.load(std::memory_order_relaxed);
        // Saturate at zero rather than wrapping.
        while (current > 0) {
            const std::size_t next = current >= sub ? current - sub : 0;
            if (g_actorCount.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
                break;
            }
        }
    }
}

/** Stops the endpoint and clears every association and peer. */
void shutdown() noexcept {
    endpoint::shutdown();
    // The worlds close before the host does, or their State contexts are stranded.
    physics::host::session::reset();
    physics::host::runtime::shutdown();
    peer::reset();
    group::reset();
    dtls::reset();
    association::reset();
}

} // namespace sunrise::server::gameplay
