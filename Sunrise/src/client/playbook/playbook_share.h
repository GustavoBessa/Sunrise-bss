#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "definition.h"

namespace sunrise::client::playbook::share {

/** Shared roteiros one listing reports. */
inline constexpr std::size_t kListCapacity = 64;

/** One roteiro found in the shared folder. */
struct Entry {
    std::array<char, state::build_data::scenarios::kNameCapacity> destination{};
    std::uint8_t destinationLength{};
    Metadata author{};
    Metadata description{};
    std::size_t steps{};
    /**
     * Set when this install has that destination.
     *
     * Reported because a roteiro for a destination the install does not carry imports cleanly and
     * then never fires, which is the kind of silence that reads as a broken feature.
     */
    bool destinationKnown{};
    /** Set when a roteiro for this destination already exists locally. */
    bool collides{};
};

/** @param value Entry to read. @return Its destination name as a bounded view. */
[[nodiscard]] inline std::string_view destination_of(const Entry& value) noexcept {
    return {value.destination.data(), value.destinationLength};
}

/**
 * Resolves and creates the shared folder.
 * @param module Loaded DLL used to resolve the owned artifact directory.
 */
void initialize(void* module) noexcept;

/** Drops the resolved folders. */
void shutdown() noexcept;

/**
 * Writes the loaded roteiro into the shared folder, metadata included.
 * @return True when the file was written.
 */
[[nodiscard]] bool export_current() noexcept;

/**
 * Lists the shared folder.
 * @param output Caller-owned fixed entry storage.
 * @return Entries written, which stops at the output's size.
 */
[[nodiscard]] std::size_t list(std::span<Entry> output) noexcept;

/**
 * Installs one shared roteiro locally.
 * @param destination Destination name, as the listing reported it.
 * @param replace Allows replacing a roteiro this install already has. Without it a collision is
 * refused rather than overwritten, because a local roteiro is captured work.
 * @return True when the roteiro was installed.
 */
[[nodiscard]] bool import_entry(std::string_view destination, bool replace) noexcept;

} // namespace sunrise::client::playbook::share
