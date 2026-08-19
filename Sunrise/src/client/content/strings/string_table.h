#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"

namespace sunrise::client::content::strings {

/**
 * The game's localized string tables, read out of the installed packages.
 *
 * A string is named by a 32-bit hash. Resolving one means finding its ordinal in a container's hash
 * array, then assembling its text from that container's `parts` and `combinations` arrays. Each part
 * carries a cipher shift its bytes must be advanced by, so the text is obfuscated on disk.
 *
 * Only the English data blob is reached: `kEnglishStringDataTag` below is a fixed offset, and the
 * other languages would need their slot mapped first.
 */

/** Longest single string the decoder assembles. Anything longer is refused, not truncated. */
inline constexpr std::size_t kMaximumStringBytes = 4096;

/**
 * One opened string container.
 *
 * Reuse one instance across containers: the two blobs are large, and reusing keeps the walk from
 * reallocating for every container in the packages.
 */
struct Container {
    /** Localized-strings blob, which owns the hash array. */
    std::vector<std::byte> localized{};
    /** English string-data blob, which owns the parts and combinations. */
    std::vector<std::byte> data{};
    middleware::content::packages::tables::Array hashes{};
    middleware::content::packages::tables::Array parts{};
    middleware::content::packages::tables::Array combinations{};
    /** Set once the localized blob and its hash array are loaded. */
    bool open{};
    /** Set once the English data blob and its two arrays are loaded. */
    bool decodable{};
};

/**
 * Collects every string container tag the installed packages declare.
 *
 * Containers are normally reached through the string roots. A build whose roots name none falls back
 * to scanning the container class directly.
 *
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param output Receives the container tags, sorted and deduplicated.
 * @return True when at least one container was found.
 */
[[nodiscard]] bool collect_containers(const middleware::content::packages::reader::Source& source,
                                      middleware::content::packages::reader::Scratch& scratch,
                                      std::vector<std::uint32_t>& output) noexcept;

/**
 * Loads one container's localized blob and hash array.
 *
 * The data blob is left alone, because a caller looking for specific hashes should skip a container
 * that holds none of them before paying for the larger read. Call `open_data` once it is wanted.
 *
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param tag Container tag from `collect_containers`.
 * @param output Receives the container. Cleared first.
 * @return True when the container is open.
 */
[[nodiscard]] bool open_container(const middleware::content::packages::reader::Source& source,
                                  middleware::content::packages::reader::Scratch& scratch,
                                  std::uint32_t tag,
                                  Container& output) noexcept;

/**
 * Loads the English data blob of an open container, so its strings can be decoded.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param container Container already opened by `open_container`.
 * @return True when the container is decodable.
 */
[[nodiscard]] bool open_data(const middleware::content::packages::reader::Source& source,
                             middleware::content::packages::reader::Scratch& scratch,
                             Container& container) noexcept;

/** @param container Open container. @return Strings it holds. */
[[nodiscard]] std::uint64_t count(const Container& container) noexcept;

/**
 * Reads one string's hash by ordinal, which is what makes a whole container enumerable.
 * @param container Open container.
 * @param index Ordinal below `count`.
 * @param output Receives the hash.
 * @return True when the ordinal is inside the hash array.
 */
[[nodiscard]] bool hash_at(const Container& container,
                           std::uint64_t index,
                           std::uint32_t& output) noexcept;

/**
 * Finds one hash's ordinal. The hash array is sorted, so this is a binary search.
 * @param container Open container.
 * @param hash String hash to find.
 * @param output Receives the ordinal.
 * @return True when this container holds that exact hash.
 */
[[nodiscard]] bool find_index(const Container& container,
                              std::uint32_t hash,
                              std::uint64_t& output) noexcept;

/**
 * Assembles one string's text, advancing each part's bytes by its cipher shift.
 * @param container Container made decodable by `open_data`.
 * @param index Ordinal of the string, from `find_index` or an enumeration.
 * @param output Receives the text. Cleared first.
 * @return True when every part read back and the result is not empty.
 */
[[nodiscard]] bool decode(const Container& container, std::uint64_t index, std::string& output);

} // namespace sunrise::client::content::strings
