/**
 * The game's localized string tables. Extracted from the entity-name walk so that entity naming and
 * the subtitle catalog share one decoder: it carries the per-part cipher shift and a dozen bounds
 * checks over untrusted blob bytes, and two copies of that drifting apart is the difference between
 * a correct name and a read past the end of a buffer.
 */

#include "string_table.h"

#include <algorithm>

#include "../blob_read.h"

namespace sunrise::client::content::strings {
namespace {

namespace package_reader = middleware::content::packages::reader;
namespace package_tables = middleware::content::packages::tables;

constexpr std::uint32_t kStringRootClass = 0x808047B7U;
constexpr std::uint32_t kStringRootRowClass = 0x808047C6U;
constexpr std::uint32_t kLocalizedStringsClass = 0x80809A88U;
constexpr std::uint32_t kLocalizedStringDataClass = 0x80809A8AU;
constexpr std::uint32_t kStringHashClass = 0x80800070U;
constexpr std::uint32_t kStringPartClass = 0x80809A90U;
constexpr std::uint32_t kStringCombinationClass = 0x80809A8EU;

constexpr std::size_t kStringRootDescriptor = 0x20;
constexpr std::size_t kStringRootRowStride = 0x08;
constexpr std::size_t kStringRootContainerTag = 0x00;
constexpr std::size_t kStringHashesDescriptor = 0x08;
constexpr std::size_t kStringPartsDescriptor = 0x08;
constexpr std::size_t kStringCombinationsDescriptor = 0x48;
/** The container names its per-language data blobs here. Only the English slot is mapped. */
constexpr std::size_t kEnglishStringDataTag = 0x18;

constexpr std::size_t kStringHashStride = 0x04;
constexpr std::size_t kStringPartStride = 0x20;
constexpr std::size_t kStringCombinationStride = 0x10;
constexpr std::size_t kCombinationPartCount = 0x08;
constexpr std::size_t kPartCharactersPointer = 0x08;
constexpr std::size_t kPartByteLength = 0x14;
constexpr std::size_t kPartCipherShift = 0x18;
/** Parts one string may be assembled from. */
constexpr std::size_t kMaximumStringParts = 64;

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

/** @return True when the hash array is well formed and wholly inside the localized blob. */
[[nodiscard]] bool hashes_valid(const Container& container) noexcept {
    return container.hashes.elementClass == kStringHashClass
           && container.hashes.dataOffset <= container.localized.size()
           && container.hashes.count
                  <= (container.localized.size() - container.hashes.dataOffset) / kStringHashStride;
}

} // namespace

/** Collects every string container tag the installed packages declare. */
bool collect_containers(const package_reader::Source& source,
                        package_reader::Scratch& scratch,
                        std::vector<std::uint32_t>& output) noexcept {
    output.clear();
    std::vector<std::uint32_t> roots{};
    if (!scan_tags(source.directory, kStringRootClass, roots)) {
        return false;
    }
    // Not named `blob`: that would shadow the namespace the reads below are called through.
    std::vector<std::byte> rootBytes{};
    std::uint32_t classId = 0;
    for (const std::uint32_t root : roots) {
        package_tables::Array rows{};
        if (!package_reader::read_tag(source, scratch, root, rootBytes, classId)
            || classId != kStringRootClass
            || !package_tables::find_array_at(rootBytes, kStringRootDescriptor, rows)
            || rows.elementClass != kStringRootRowClass || rows.dataOffset > rootBytes.size()
            || rows.count > (rootBytes.size() - rows.dataOffset) / kStringRootRowStride) {
            continue;
        }
        for (std::uint64_t index = 0; index < rows.count; ++index) {
            std::uint32_t tag = 0;
            const std::size_t row =
                rows.dataOffset + static_cast<std::size_t>(index) * kStringRootRowStride;
            if (blob::read(rootBytes, row + kStringRootContainerTag, tag)) {
                output.push_back(tag);
            }
        }
    }
    sort_unique(output);
    if (!output.empty()) {
        return true;
    }
    // A build whose roots name no container still has the containers themselves.
    return scan_tags(source.directory, kLocalizedStringsClass, output) && !output.empty();
}

/** Loads one container's localized blob and hash array. */
bool open_container(const package_reader::Source& source,
                    package_reader::Scratch& scratch,
                    std::uint32_t tag,
                    Container& output) noexcept {
    output.hashes = {};
    output.parts = {};
    output.combinations = {};
    output.open = false;
    output.decodable = false;
    std::uint32_t classId = 0;
    if (!package_reader::read_tag(source, scratch, tag, output.localized, classId)
        || classId != kLocalizedStringsClass
        || !package_tables::find_array_at(output.localized, kStringHashesDescriptor, output.hashes)
        || !hashes_valid(output)) {
        // Left closed on a bad hash array, so no later call can read through it.
        return false;
    }
    output.open = true;
    return true;
}

/** Loads the English data blob of an open container. */
bool open_data(const package_reader::Source& source,
               package_reader::Scratch& scratch,
               Container& container) noexcept {
    container.decodable = false;
    if (!container.open) {
        return false;
    }
    std::uint32_t dataTag = 0;
    std::uint32_t classId = 0;
    if (!blob::read(container.localized, kEnglishStringDataTag, dataTag)
        || !package_reader::read_tag(source, scratch, dataTag, container.data, classId)
        || classId != kLocalizedStringDataClass
        || !package_tables::find_array_at(container.data, kStringPartsDescriptor, container.parts)
        || !package_tables::find_array_at(
            container.data, kStringCombinationsDescriptor, container.combinations)) {
        return false;
    }
    container.decodable = true;
    return true;
}

/** Reports how many strings one container holds. */
std::uint64_t count(const Container& container) noexcept {
    return container.open ? container.hashes.count : 0;
}

/** Reads one string's hash by ordinal. */
bool hash_at(const Container& container, std::uint64_t index, std::uint32_t& output) noexcept {
    if (!container.open || index >= container.hashes.count) {
        return false;
    }
    return blob::read(
        container.localized,
        container.hashes.dataOffset + static_cast<std::size_t>(index) * kStringHashStride,
        output);
}

/** Finds one hash's ordinal by binary search. */
bool find_index(const Container& container, std::uint32_t hash, std::uint64_t& output) noexcept {
    if (!container.open) {
        return false;
    }
    std::uint64_t low = 0;
    std::uint64_t high = container.hashes.count;
    while (low < high) {
        const std::uint64_t middle = low + (high - low) / 2;
        std::uint32_t value = 0;
        if (!hash_at(container, middle, value)) {
            return false;
        }
        if (value < hash) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    std::uint32_t value = 0;
    if (low >= container.hashes.count || !hash_at(container, low, value) || value != hash) {
        return false;
    }
    output = low;
    return true;
}

/** Assembles one string's text, advancing each part's bytes by its cipher shift. */
bool decode(const Container& container, std::uint64_t index, std::string& output) {
    output.clear();
    const std::span<const std::byte> data{container.data};
    const package_tables::Array& parts = container.parts;
    const package_tables::Array& combinations = container.combinations;
    if (!container.decodable || parts.elementClass != kStringPartClass
        || combinations.elementClass != kStringCombinationClass || index >= combinations.count
        || parts.dataOffset > data.size()
        || parts.count > (data.size() - parts.dataOffset) / kStringPartStride
        || combinations.dataOffset > data.size()
        || combinations.count
               > (data.size() - combinations.dataOffset) / kStringCombinationStride) {
        return false;
    }
    const std::size_t combination =
        combinations.dataOffset + static_cast<std::size_t>(index) * kStringCombinationStride;
    std::int64_t partCount = 0;
    std::size_t firstPart = 0;
    if (!blob::read(data, combination + kCombinationPartCount, partCount) || partCount <= 0
        || partCount > static_cast<std::int64_t>(kMaximumStringParts)
        || !blob::relative(data, combination, firstPart) || firstPart < parts.dataOffset
        || (firstPart - parts.dataOffset) % kStringPartStride != 0) {
        return false;
    }
    const std::size_t firstIndex = (firstPart - parts.dataOffset) / kStringPartStride;
    if (firstIndex >= parts.count
        || static_cast<std::uint64_t>(partCount) > parts.count - firstIndex) {
        return false;
    }
    for (std::size_t part = 0; part < static_cast<std::size_t>(partCount); ++part) {
        const std::size_t at = parts.dataOffset + (firstIndex + part) * kStringPartStride;
        std::size_t characters = 0;
        std::uint16_t byteLength = 0;
        std::uint16_t cipherShift = 0;
        if (!blob::relative(data, at + kPartCharactersPointer, characters)
            || !blob::read(data, at + kPartByteLength, byteLength)
            || !blob::read(data, at + kPartCipherShift, cipherShift)
            || byteLength > kMaximumStringBytes - output.size() || characters > data.size()
            || byteLength > data.size() - characters) {
            return false;
        }
        const char* bytes = reinterpret_cast<const char*>(data.data() + characters);
        if (cipherShift == 0) {
            output.append(bytes, byteLength);
            continue;
        }
        for (std::size_t byte = 0; byte < byteLength; ++byte) {
            output.push_back(
                static_cast<char>(static_cast<unsigned char>(bytes[byte]) + cipherShift));
        }
    }
    // A part may carry trailing bytes after its terminator, which are not part of the text.
    output.erase(std::find(output.begin(), output.end(), '\0'), output.end());
    return !output.empty();
}

} // namespace sunrise::client::content::strings
