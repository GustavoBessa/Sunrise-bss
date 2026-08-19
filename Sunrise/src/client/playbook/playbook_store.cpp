/**
 * The roteiro file format. One line per step, comma separated, so a roteiro stays readable and
 * hand editable without a JSON reader: the project's own JSON primitives are private members of
 * the Core settings parser, which gates the boot and must not grow a second caller.
 *
 * A malformed line is skipped and reported rather than failing the load, so one bad hand edit
 * cannot cost the whole roteiro.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "../../core/logging/log.h"
#include "internal.h"

namespace sunrise::client::playbook::internal {
namespace {

/** Fields every step line carries, in order. */
constexpr std::size_t kFieldCount = 11;
/** Longest single field, which bounds the null-terminated copy a numeric parse needs. */
constexpr std::size_t kFieldCapacity = 64;

/** One step line split into its fields. */
using Fields = std::array<std::string_view, kFieldCount>;

/** @param value Candidate byte. @return True for a byte a package name may hold. */
[[nodiscard]] bool name_byte(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_';
}

/**
 * Copies one field into null-terminated storage so the C numeric parsers can read it.
 * @param field Field text.
 * @param output Receives the bytes and a null.
 * @return True when the field is non-empty and fits.
 */
[[nodiscard]] bool terminated(std::string_view field,
                              std::array<char, kFieldCapacity>& output) noexcept {
    if (field.empty() || field.size() >= output.size()) {
        return false;
    }
    output = {};
    std::copy_n(field.begin(), field.size(), output.begin());
    return true;
}

/**
 * Reads one unsigned field, accepting the `0x` form the hash columns are written in.
 * @param field Field text.
 * @param output Receives the value.
 * @return True when the whole field parsed.
 */
[[nodiscard]] bool unsigned_field(std::string_view field, std::uint32_t& output) noexcept {
    std::string_view digits = field;
    int base = 10;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits = digits.substr(2);
        base = 16;
    }
    if (digits.empty()) {
        return false;
    }
    const char* const begin = digits.data();
    const char* const end = digits.data() + digits.size();
    const auto parsed = std::from_chars(begin, end, output, base);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

/** @param field Field text. @param output Receives the value. @return True when it parsed. */
[[nodiscard]] bool signed_field(std::string_view field, std::int32_t& output) noexcept {
    const char* const begin = field.data();
    const char* const end = field.data() + field.size();
    const auto parsed = std::from_chars(begin, end, output, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

/**
 * Reads one float field.
 * `std::from_chars` for floating point is uneven across the toolchains this project builds with,
 * so the field is null terminated and handed to `strtof`, whose behaviour is fixed.
 * @param field Field text.
 * @param output Receives the value.
 * @return True when the whole field parsed into a finite value.
 */
[[nodiscard]] bool float_field(std::string_view field, float& output) noexcept {
    std::array<char, kFieldCapacity> text{};
    if (!terminated(field, text)) {
        return false;
    }
    char* end = nullptr;
    const float value = std::strtof(text.data(), &end);
    if (end == text.data() || *end != '\0') {
        return false;
    }
    // A non-finite coordinate would make every distance comparison against it false.
    if (value != value || value > 3.0e38F || value < -3.0e38F) {
        return false;
    }
    output = value;
    return true;
}

/**
 * Splits one line on commas.
 * The label is the last field and keeps whatever it holds, so a comma inside it would shift the
 * columns. Capture strips commas for exactly that reason.
 * @param line Line without its terminator.
 * @param output Receives the fields.
 * @return True when the line holds exactly the expected field count.
 */
[[nodiscard]] bool split(std::string_view line, Fields& output) noexcept {
    std::size_t count = 0;
    std::size_t begin = 0;
    while (count + 1 < kFieldCount) {
        const std::size_t comma = line.find(',', begin);
        if (comma == std::string_view::npos) {
            return false;
        }
        output[count++] = line.substr(begin, comma - begin);
        begin = comma + 1;
    }
    // Everything after the last separator is the label, commas included.
    output[count] = line.substr(begin);
    return true;
}

/**
 * Parses one step line.
 * @param line Line without its terminator.
 * @param output Receives the step only when every field is valid.
 * @return True when the line is one complete step.
 */
[[nodiscard]] bool parse_step(std::string_view line, Step& output) noexcept {
    Fields fields{};
    if (!split(line, fields)) {
        return false;
    }
    Step step{};
    // The first column is read only to check the line is well formed. A step's position in the
    // roteiro is its position in the file, so a hand edited ordinal cannot reorder anything.
    std::uint32_t ordinal = 0;
    if (!unsigned_field(fields[0], ordinal) || !unsigned_field(fields[1], step.bubble)
        || !unsigned_field(fields[2], step.sliceState) || !signed_field(fields[3], step.region)
        || !unsigned_field(fields[4], step.spawnHash) || !float_field(fields[5], step.position[0])
        || !float_field(fields[6], step.position[1]) || !float_field(fields[7], step.position[2])
        || !float_field(fields[8], step.radius)) {
        return false;
    }
    // An empty audio column is the normal state today, so it reads as "no sound" rather than
    // failing the line.
    if (!fields[9].empty() && !unsigned_field(fields[9], step.audioTag)) {
        return false;
    }
    if (step.radius < kMinimumRadius || step.radius > kMaximumRadius) {
        return false;
    }
    const std::size_t labelLength = (std::min)(fields[10].size(), step.label.size());
    std::copy_n(fields[10].begin(), labelLength, step.label.begin());
    step.labelLength = static_cast<std::uint8_t>(labelLength);
    output = step;
    return true;
}

/**
 * Appends one step line to the document.
 * @param step Step to write.
 * @param ordinal One-based position in the roteiro.
 * @param document Whole document storage.
 * @param used Bytes already written, advanced on success.
 * @return True when the whole line fit.
 */
[[nodiscard]] bool append_step(const Step& step,
                               std::size_t ordinal,
                               std::array<char, kFileCapacity>& document,
                               std::size_t& used) noexcept {
    const std::string_view label = label_of(step);
    // The column is written in the same `0x` form the reader accepts, or left empty. A decimal
    // value behind a `0x` prefix would read back as a different number.
    std::array<char, 16> audio{};
    if (step.audioTag != kNoAudioTag
        && std::snprintf(
               audio.data(), audio.size(), "0x%08X", static_cast<unsigned>(step.audioTag))
               <= 0) {
        return false;
    }
    const int written =
        std::snprintf(document.data() + used,
                      document.size() - used,
                      "%zu,%u,%u,%d,0x%08X,%.3f,%.3f,%.3f,%.1f,%s,%.*s\r\n",
                      ordinal,
                      static_cast<unsigned>(step.bubble),
                      static_cast<unsigned>(step.sliceState),
                      static_cast<int>(step.region),
                      static_cast<unsigned>(step.spawnHash),
                      static_cast<double>(step.position[0]),
                      static_cast<double>(step.position[1]),
                      static_cast<double>(step.position[2]),
                      static_cast<double>(step.radius),
                      audio.data(),
                      static_cast<int>(label.size()),
                      label.data());
    if (written <= 0 || static_cast<std::size_t>(written) >= document.size() - used) {
        return false;
    }
    used += static_cast<std::size_t>(written);
    return true;
}

} // namespace

/** Reports one store outcome on the Client channel. */
void report_fail(const char* stage, const char* reason) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=playbook stage=%s result=fail reason=%s",
                                      stage,
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Builds one destination's file path under the playbook directory. */
bool resolve_path(const core::path::Buffer& directory,
                  std::string_view destination,
                  core::path::Buffer& output) noexcept {
    if (destination.empty()) {
        return false;
    }
    output = directory;
    if (!core::path::append(output, L"\\")) {
        return false;
    }
    // Widened one byte at a time, and only for the bytes a package name may hold, so a crafted
    // name cannot escape the directory or reach a device path.
    std::array<wchar_t, state::build_data::scenarios::kNameCapacity + 1> wide{};
    if (destination.size() >= wide.size()) {
        return false;
    }
    for (std::size_t index = 0; index < destination.size(); ++index) {
        if (!name_byte(destination[index])) {
            return false;
        }
        wide[index] = static_cast<wchar_t>(destination[index]);
    }
    return core::path::append(output, {wide.data(), destination.size()})
           && core::path::append(output, kFileExtension);
}

/** Reads one roteiro from disk. */
bool load(const wchar_t* path, Roteiro& output) noexcept {
    const HANDLE file = CreateFileW(path,
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // A destination with no roteiro yet is the ordinary case, not a failure.
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    auto document = std::array<char, kFileCapacity>{};
    DWORD read = 0;
    const bool readOk =
        ReadFile(file, document.data(), static_cast<DWORD>(document.size() - 1), &read, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    if (!readOk) {
        report_fail("load", "read");
        return false;
    }

    std::string_view text(document.data(), read);
    bool header = false;
    std::size_t skipped = 0;
    while (!text.empty()) {
        const std::size_t breakAt = text.find('\n');
        std::string_view line = breakAt == std::string_view::npos ? text : text.substr(0, breakAt);
        text = breakAt == std::string_view::npos ? std::string_view{} : text.substr(breakAt + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (!header) {
            if (line != kMagic) {
                report_fail("load", "magic");
                return false;
            }
            header = true;
            continue;
        }
        if (output.count >= output.steps.size()) {
            report_fail("load", "capacity");
            break;
        }
        Step step{};
        if (!parse_step(line, step)) {
            ++skipped;
            continue;
        }
        output.steps[output.count++] = step;
    }
    if (!header) {
        report_fail("load", "magic");
        return false;
    }
    if (skipped != 0) {
        report_fail("load", "line");
    }
    return true;
}

/** Writes one roteiro to disk, replacing the file. */
bool save(const wchar_t* path, const Roteiro& value) noexcept {
    auto document = std::array<char, kFileCapacity>{};
    std::size_t used = 0;
    const int header = std::snprintf(document.data(),
                                     document.size(),
                                     "%.*s\r\n"
                                     "# destination %.*s\r\n"
                                     "# step,bubble,slice,region,spawn_hash,x,y,z,radius,"
                                     "audio_tag,label\r\n",
                                     static_cast<int>(kMagic.size()),
                                     kMagic.data(),
                                     static_cast<int>(destination_of(value).size()),
                                     destination_of(value).data());
    if (header <= 0 || static_cast<std::size_t>(header) >= document.size()) {
        report_fail("save", "header");
        return false;
    }
    used = static_cast<std::size_t>(header);
    for (std::size_t index = 0; index < value.count; ++index) {
        if (!append_step(value.steps[index], index + 1, document, used)) {
            report_fail("save", "capacity");
            return false;
        }
    }

    const HANDLE file = CreateFileW(
        path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report_fail("save", "open");
        return false;
    }
    DWORD written = 0;
    bool complete =
        WriteFile(file, document.data(), static_cast<DWORD>(used), &written, nullptr) != FALSE
        && written == static_cast<DWORD>(used);
    complete = CloseHandle(file) != FALSE && complete;
    if (!complete) {
        report_fail("save", "write");
    }
    return complete;
}

} // namespace sunrise::client::playbook::internal
