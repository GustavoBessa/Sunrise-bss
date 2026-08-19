#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace sunrise::client::content::blob {

/**
 * Reads one whole trivially copyable value out of a package blob.
 *
 * Package blobs are untrusted input, so every read is bounds checked against the blob rather than
 * against the offset the blob itself declares.
 *
 * @param data Whole blob bytes.
 * @param offset Byte offset of the value.
 * @param value Receives the value only when the whole of it is inside the blob.
 * @return True when the value was read.
 */
template <typename Value>
[[nodiscard]] bool read(std::span<const std::byte> data,
                        std::size_t offset,
                        Value& value) noexcept {
    if (offset > data.size() || sizeof(Value) > data.size() - offset) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, sizeof value);
    return true;
}

/**
 * Follows one self-relative pointer field.
 *
 * The blobs store offsets as a signed delta from the field's own position, so resolving one is an
 * addition that has to stay inside the blob in both directions.
 *
 * @param data Whole blob bytes.
 * @param field Byte offset of the delta field.
 * @param output Receives the absolute offset the field points at.
 * @return True when the field holds a nonzero delta landing inside the blob.
 */
[[nodiscard]] inline bool relative(std::span<const std::byte> data,
                                   std::size_t field,
                                   std::size_t& output) noexcept {
    std::int64_t delta = 0;
    if (!read(data, field, delta) || delta == 0) {
        return false;
    }
    const std::int64_t absolute = static_cast<std::int64_t>(field) + delta;
    if (absolute < 0 || static_cast<std::uint64_t>(absolute) >= data.size()) {
        return false;
    }
    output = static_cast<std::size_t>(absolute);
    return true;
}

/**
 * Follows one self-relative resource pointer and reports the class recorded before it.
 *
 * A resource blob stores its class id in the four bytes preceding the payload, which is what lets a
 * walk that hops between blob kinds pick the right parser.
 *
 * @param data Whole blob bytes.
 * @param field Byte offset of the delta field.
 * @param output Receives the absolute offset of the payload.
 * @param classId Receives the class recorded before the payload.
 * @return True when the pointer resolves and the class could be read.
 */
[[nodiscard]] inline bool resource(std::span<const std::byte> data,
                                   std::size_t field,
                                   std::size_t& output,
                                   std::uint32_t& classId) noexcept {
    return relative(data, field, output) && output >= sizeof(classId)
           && read(data, output - sizeof(classId), classId);
}

} // namespace sunrise::client::content::blob
