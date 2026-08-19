/**
 * Names entities by walking the activity wrappers for their name resources, then resolving those
 * name hashes against the game's localized string tables.
 *
 * The string decoding itself lives in `content/strings/string_table.h`, shared with the subtitle
 * catalog. Only the activity walk is here.
 */

#include "localized_aliases.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../blob_read.h"
#include "../items/packages/internal.h"
#include "../strings/string_table.h"

namespace sunrise::client::content::entity_names::localized_aliases {
namespace {

namespace package_reader = middleware::content::packages::reader;
namespace package_tables = middleware::content::packages::tables;
namespace strings = content::strings;

constexpr std::uint32_t kActivityWrapperClass = 0x80809B14U;
constexpr std::uint32_t kEntityComponentClass = 0x80809C36U;
constexpr std::uint32_t kActivityComponentKind = 0x80809A3BU;
constexpr std::uint32_t kActivityComponentBody = 0x8080948FU;
constexpr std::uint32_t kActivityGroupClass = 0x80808356U;
constexpr std::uint32_t kActivityTableRowClass = 0x80808358U;
constexpr std::uint32_t kEntityNameResourceClass = 0x80807EB6U;
constexpr std::uint32_t kEntityClass = 0x80809C0FU;

constexpr std::size_t kWrapperComponentTag = 0x0C;
constexpr std::size_t kComponentKindPointer = 0x10;
constexpr std::size_t kComponentBodyPointer = 0x18;
constexpr std::size_t kActivityGroupsDescriptor = 0xA8;
constexpr std::size_t kActivityGroupStride = 0x68;
constexpr std::size_t kActivityTableFirstDescriptor = 0x08;
constexpr std::size_t kActivityTableCount = 6;
/** The six table descriptors of one group sit next to each other at this stride. */
constexpr std::size_t kActivityTableDescriptorStride = 0x10;
constexpr std::size_t kActivityTableRowStride = 0x18;
constexpr std::size_t kMapEntrySize = 0x90;
constexpr std::size_t kMapEntityTag = 0x00;
constexpr std::size_t kMapDataResourcePointer = 0x78;
constexpr std::size_t kEntityNameHash = 0x20;

/** One entity and the string hash naming it, before the string is resolved. */
struct Candidate {
    std::uint32_t entity{};
    std::uint32_t stringHash{};
};

/** One resolved string, trimmed to what a name may hold. */
struct StringValue {
    std::array<char, kNameCapacity> text{};
    std::uint8_t length{};
};

[[nodiscard]] bool collect_tag(void* context, std::uint32_t tag) noexcept {
    static_cast<std::vector<std::uint32_t>*>(context)->push_back(tag);
    return true;
}

void sort_unique(std::vector<std::uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

[[nodiscard]] bool scan_tags(std::wstring_view directory,
                             std::uint32_t classId,
                             std::vector<std::uint32_t>& output) noexcept {
    output.clear();
    package_reader::ScanResult scan{};
    const bool ok = package_reader::scan_class(directory, classId, &collect_tag, &output, scan);
    if (ok) {
        sort_unique(output);
    }
    return ok;
}

void append_table_candidates(std::span<const std::byte> data,
                             const package_tables::Array& table,
                             const std::vector<std::uint32_t>& entityTags,
                             std::vector<Candidate>& output) noexcept {
    if (table.elementClass != kActivityTableRowClass || table.dataOffset > data.size()
        || table.count > (data.size() - table.dataOffset) / kActivityTableRowStride) {
        return;
    }
    for (std::uint64_t index = 0; index < table.count; ++index) {
        const std::size_t row =
            table.dataOffset + static_cast<std::size_t>(index) * kActivityTableRowStride;
        std::size_t mapEntry = 0;
        if (!blob::relative(data, row, mapEntry) || mapEntry > data.size()
            || kMapEntrySize > data.size() - mapEntry) {
            continue;
        }
        std::uint32_t entity = 0;
        if (!blob::read(data, mapEntry + kMapEntityTag, entity)
            || !std::binary_search(entityTags.begin(), entityTags.end(), entity)) {
            continue;
        }
        std::size_t nameResource = 0;
        std::uint32_t resourceClass = 0;
        std::uint32_t stringHash = 0;
        if (!blob::resource(data, mapEntry + kMapDataResourcePointer, nameResource, resourceClass)
            || resourceClass != kEntityNameResourceClass
            || !blob::read(data, nameResource + kEntityNameHash, stringHash) || stringHash == 0
            || stringHash == 0xFFFFFFFFU) {
            continue;
        }
        output.push_back(Candidate{entity, stringHash});
    }
}

void append_wrapper_candidates(const package_reader::Source& source,
                               package_reader::Scratch& scratch,
                               std::uint32_t wrapperTag,
                               const std::vector<std::uint32_t>& entityTags,
                               std::vector<Candidate>& output,
                               std::vector<std::byte>& data) noexcept {
    std::uint32_t classId = 0;
    if (!package_reader::read_tag(source, scratch, wrapperTag, data, classId)
        || classId != kActivityWrapperClass) {
        return;
    }
    std::uint32_t componentTag = 0;
    if (!blob::read(data, kWrapperComponentTag, componentTag)) {
        return;
    }
    if (!package_reader::read_tag(source, scratch, componentTag, data, classId)
        || classId != kEntityComponentClass) {
        return;
    }
    std::size_t kind = 0;
    std::size_t body = 0;
    std::uint32_t kindClass = 0;
    std::uint32_t bodyClass = 0;
    if (!blob::resource(data, kComponentKindPointer, kind, kindClass)
        || kindClass != kActivityComponentKind
        || !blob::resource(data, kComponentBodyPointer, body, bodyClass)
        || bodyClass != kActivityComponentBody) {
        return;
    }

    package_tables::Array groups{};
    if (!package_tables::find_array_at(data, body + kActivityGroupsDescriptor, groups)
        || groups.elementClass != kActivityGroupClass || groups.dataOffset > data.size()
        || groups.count > (data.size() - groups.dataOffset) / kActivityGroupStride) {
        return;
    }
    for (std::uint64_t groupIndex = 0; groupIndex < groups.count; ++groupIndex) {
        const std::size_t group =
            groups.dataOffset + static_cast<std::size_t>(groupIndex) * kActivityGroupStride;
        for (std::size_t tableIndex = 0; tableIndex < kActivityTableCount; ++tableIndex) {
            package_tables::Array table{};
            const std::size_t descriptor = group + kActivityTableFirstDescriptor
                                           + tableIndex * kActivityTableDescriptorStride;
            if (package_tables::find_array_at(data, descriptor, table)) {
                append_table_candidates(data, table, entityTags, output);
            }
        }
    }
}

[[nodiscard]] bool collect_candidates(const package_reader::Source& source,
                                      package_reader::Scratch& scratch,
                                      std::vector<Candidate>& output,
                                      Result& result) noexcept {
    std::vector<std::uint32_t> wrappers{};
    std::vector<std::uint32_t> entities{};
    if (!scan_tags(source.directory, kActivityWrapperClass, wrappers)
        || !scan_tags(source.directory, kEntityClass, entities) || wrappers.empty()
        || entities.empty()) {
        return false;
    }
    result.wrappers = wrappers.size();
    std::vector<std::byte> data{};
    for (const std::uint32_t wrapper : wrappers) {
        append_wrapper_candidates(source, scratch, wrapper, entities, output, data);
    }
    std::sort(output.begin(), output.end(), [](const Candidate& left, const Candidate& right) {
        return left.entity < right.entity
               || (left.entity == right.entity && left.stringHash < right.stringHash);
    });
    output.erase(std::unique(output.begin(),
                             output.end(),
                             [](const Candidate& left, const Candidate& right) {
                                 return left.entity == right.entity
                                        && left.stringHash == right.stringHash;
                             }),
                 output.end());
    result.placements = output.size();
    return !output.empty();
}

void store_string(std::string_view source, StringValue& output) noexcept {
    if (source.empty() || source.size() >= output.text.size()) {
        return;
    }
    std::size_t length = 0;
    for (const unsigned char value : source) {
        if (value >= 0x20) {
            output.text[length++] = static_cast<char>(value);
        }
    }
    if (length != 0) {
        output.length = static_cast<std::uint8_t>(length);
    }
}

/**
 * Resolves whichever wanted hashes one container holds.
 * @param container Reused across containers, because its two blobs are large.
 */
void resolve_container(const package_reader::Source& source,
                       package_reader::Scratch& scratch,
                       std::uint32_t containerTag,
                       const std::vector<std::uint32_t>& wanted,
                       std::vector<StringValue>& values,
                       strings::Container& container) {
    if (!strings::open_container(source, scratch, containerTag, container)) {
        return;
    }
    std::vector<std::pair<std::size_t, std::uint64_t>> matches{};
    for (std::size_t wantedIndex = 0; wantedIndex < wanted.size(); ++wantedIndex) {
        if (values[wantedIndex].length != 0) {
            continue;
        }
        std::uint64_t stringIndex = 0;
        if (strings::find_index(container, wanted[wantedIndex], stringIndex)) {
            matches.emplace_back(wantedIndex, stringIndex);
        }
    }
    // The data blob is the larger of the two reads, so a container holding none of the wanted
    // hashes is skipped before paying for it.
    if (matches.empty() || !strings::open_data(source, scratch, container)) {
        return;
    }
    std::string decoded{};
    for (const auto [wantedIndex, stringIndex] : matches) {
        if (strings::decode(container, stringIndex, decoded)) {
            store_string(decoded, values[wantedIndex]);
        }
    }
}

[[nodiscard]] bool resolve_candidates(const package_reader::Source& source,
                                      package_reader::Scratch& scratch,
                                      const std::vector<Candidate>& candidates,
                                      std::vector<Entry>& output,
                                      Result& result) {
    std::vector<std::uint32_t> containers{};
    if (!strings::collect_containers(source, scratch, containers)) {
        return false;
    }
    std::vector<std::uint32_t> wanted{};
    wanted.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        wanted.push_back(candidate.stringHash);
    }
    sort_unique(wanted);
    std::vector<StringValue> values(wanted.size());
    strings::Container container{};
    for (const std::uint32_t tag : containers) {
        resolve_container(source, scratch, tag, wanted, values, container);
    }

    const std::size_t before = output.size();
    for (const Candidate& candidate : candidates) {
        const auto found = std::lower_bound(wanted.begin(), wanted.end(), candidate.stringHash);
        if (found == wanted.end() || *found != candidate.stringHash) {
            continue;
        }
        const StringValue& value = values[static_cast<std::size_t>(found - wanted.begin())];
        if (value.length == 0) {
            continue;
        }
        Entry entry{};
        entry.tag = candidate.entity;
        entry.length = value.length;
        std::memcpy(entry.text.data(), value.text.data(), value.length);
        output.push_back(entry);
    }
    result.resolved = output.size() - before;
    return result.resolved != 0;
}

} // namespace

bool append(std::wstring_view packageDirectory,
            std::vector<Entry>& output,
            Result& result) noexcept {
    result = {};
    package_reader::BlockKeys keys{};
    if (!client::content::items::packages::collect_keys(keys)) {
        return false;
    }
    auto scratch = std::make_unique<package_reader::Scratch>();
    if (!scratch) {
        return false;
    }
    const package_reader::Source source{packageDirectory, &keys};
    std::vector<Candidate> candidates{};
    const bool collected = collect_candidates(source, *scratch, candidates, result);
    const bool resolved = collected
                          && resolve_candidates(source, *scratch, candidates, output, result);
    package_reader::close_files(*scratch);
    return resolved;
}

} // namespace sunrise::client::content::entity_names::localized_aliases
