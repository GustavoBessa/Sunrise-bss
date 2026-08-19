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
#include <memory>
#include <string_view>

#include "../../core/logging/log.h"
#include "internal.h"

namespace sunrise::client::playbook::internal {
namespace {

/** Fields a version-1 step line carries. The label is the last one. */
constexpr std::size_t kFieldCountV1 = 11;
/** Fields a version-2 step line carries: version 1 plus the subtitle column. */
constexpr std::size_t kFieldCountV2 = 12;
/** Longest single field, which bounds the null-terminated copy a numeric parse needs. */
constexpr std::size_t kFieldCapacity = 64;

/** One step line split into its fields. Version 1 leaves the last slot unused. */
using Fields = std::array<std::string_view, kFieldCountV2>;

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
[[nodiscard]] bool split(std::string_view line, std::size_t fieldCount, Fields& output) noexcept {
    output = {};
    std::size_t count = 0;
    std::size_t begin = 0;
    while (count + 1 < fieldCount) {
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
[[nodiscard]] bool parse_step(std::string_view line, bool withSubtitle, Step& output) noexcept {
    Fields fields{};
    const std::size_t fieldCount = withSubtitle ? kFieldCountV2 : kFieldCountV1;
    if (!split(line, fieldCount, fields)) {
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
    // Version 1 has no subtitle column, so the audio and label columns shift down by one.
    const std::size_t subtitleField = 9;
    const std::size_t audioField = withSubtitle ? 10 : 9;
    const std::size_t labelField = withSubtitle ? 11 : 10;
    // The subtitle column is read past and discarded. Roteiros written while subtitles existed still
    // carry one, and refusing the line over it would cost the whole step.
    static_cast<void>(subtitleField);
    // An empty audio column is the normal state today, so it reads as "no sound" rather than
    // failing the line.
    if (!fields[audioField].empty() && !unsigned_field(fields[audioField], step.audioTag)) {
        return false;
    }
    if (step.radius < kMinimumRadius || step.radius > kMaximumRadius) {
        return false;
    }
    const std::size_t labelLength = (std::min)(fields[labelField].size(), step.label.size());
    std::copy_n(fields[labelField].begin(), labelLength, step.label.begin());
    step.labelLength = static_cast<std::uint8_t>(labelLength);
    output = step;
    return true;
}

/** Stores one metadata value, dropping bytes a single line cannot carry. */
void store_metadata(std::string_view value, Metadata& output) noexcept {
    output = {};
    for (const char byte : value) {
        if (output.length >= output.value.size()) {
            break;
        }
        if (byte >= ' ' && byte <= '~') {
            output.value[output.length++] = byte;
        }
    }
}

/**
 * Reads one `# key value` comment line into the roteiro's metadata.
 * A key this build does not know is ignored, which is what keeps the format additive.
 * @param line Comment line, `#` included.
 * @param output Roteiro receiving the value.
 */
void read_metadata(std::string_view line, Roteiro& output) noexcept {
    std::string_view rest = line.substr(1);
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    const std::size_t space = rest.find(' ');
    if (space == std::string_view::npos) {
        return;
    }
    const std::string_view key = rest.substr(0, space);
    const std::string_view value = rest.substr(space + 1);
    if (key == "author") {
        store_metadata(value, output.author);
    } else if (key == "description") {
        store_metadata(value, output.description);
    } else if (key == "game_build") {
        store_metadata(value, output.gameBuild);
    } else if (key == "order") {
        // Anything other than the one word that means ordered leaves the roteiro free, which is what
        // a file written before ordering existed says by saying nothing.
        output.sequential = value == "sequential";
    }
}

/**
 * Parses one `@` continuation line into the timed gate of the step above it.
 *
 * @param line Line without its terminator, `@` included.
 * @param output Roteiro whose most recent step receives the gate.
 * @return True when the gate was stored.
 */
[[nodiscard]] bool parse_gate(std::string_view line, Roteiro& output) noexcept {
    // A timed first step has nothing to wait on, so it would never fire and is refused here too.
    if (output.count < 2) {
        return false;
    }
    std::string_view rest = line.substr(1);
    if (rest.empty() || rest.front() != ',') {
        return false;
    }
    rest.remove_prefix(1);
    std::uint32_t delay = 0;
    if (!unsigned_field(rest, delay) || delay > kMaximumDelayMs) {
        return false;
    }
    Step& step = output.steps[output.count - 1];
    step.gate = Gate::delay;
    step.delayMs = static_cast<std::uint16_t>(delay);
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
                               Document& document,
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
    // The subtitle column stays empty: dialogue is a list now, and every line of it -- including the
    // first, with its own dwell -- goes on a `+` line below. Putting the first line here as well
    // would read back twice.
    const int written =
        std::snprintf(document.data() + used,
                      document.size() - used,
                      "%zu,%u,%u,%d,0x%08X,%.3f,%.3f,%.3f,%.1f,,%s,%.*s\r\n",
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

/**
 * Appends one step's continuation lines, which today means only its timed gate.
 * @param step Step to write.
 * @param document Whole document storage.
 * @param used Bytes already written, advanced on success.
 * @return True when every line fit.
 */
[[nodiscard]] bool append_continuations(const Step& step,
                                        Document& document,
                                        std::size_t& used) noexcept {
    if (step.gate == Gate::delay) {
        const int written = std::snprintf(document.data() + used,
                                          document.size() - used,
                                          "%c,%u\r\n",
                                          kGateMarker,
                                          static_cast<unsigned>(step.delayMs));
        if (written <= 0 || static_cast<std::size_t>(written) >= document.size() - used) {
            return false;
        }
        used += static_cast<std::size_t>(written);
    }
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
    // On the heap: a whole roteiro with all its dialogue is far past what belongs on the caller's
    // stack, and the caller here is the game's own render thread.
    auto storage = std::make_unique<Document>();
    if (!storage) {
        (void)CloseHandle(file);
        report_fail("load", "storage");
        return false;
    }
    Document& document = *storage;
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
    bool withSubtitle = false;
    std::size_t skipped = 0;
    while (!text.empty()) {
        const std::size_t breakAt = text.find('\n');
        std::string_view line = breakAt == std::string_view::npos ? text : text.substr(0, breakAt);
        text = breakAt == std::string_view::npos ? std::string_view{} : text.substr(breakAt + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        if (line.front() == '#') {
            // Metadata rides in comments, so a build that does not know a key still loads the file.
            read_metadata(line, output);
            continue;
        }
        if (!header) {
            if (line == kMagicV2 || line == kMagicV3) {
                // Both carry the column; version 3 leaves it empty and speaks through `+` lines.
                withSubtitle = true;
            } else if (line != kMagicV1) {
                report_fail("load", "magic");
                return false;
            }
            header = true;
            continue;
        }
        // Continuation lines attach to the step above them, so they are dispatched before the step
        // parse and never consume a step slot.
        if (line.front() == kLineMarker) {
            // A dialogue line from a roteiro written while subtitles existed. Skipped without being
            // counted as malformed, so an older shared file loads without reporting a fault.
            continue;
        }
        if (line.front() == kGateMarker) {
            skipped += parse_gate(line, output) ? 0U : 1U;
            continue;
        }
        if (output.count >= output.steps.size()) {
            report_fail("load", "capacity");
            break;
        }
        Step step{};
        if (!parse_step(line, withSubtitle, step)) {
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
    auto storage = std::make_unique<Document>();
    if (!storage) {
        report_fail("save", "storage");
        return false;
    }
    Document& document = *storage;
    std::size_t used = 0;
    const std::string_view author = value_of(value.author);
    const std::string_view description = value_of(value.description);
    const std::string_view gameBuild = value_of(value.gameBuild);
    const int header = std::snprintf(document.data(),
                                     document.size(),
                                     "%.*s\r\n"
                                     "# destination %.*s\r\n"
                                     "# order %s\r\n"
                                     "# author %.*s\r\n"
                                     "# description %.*s\r\n"
                                     "# game_build %.*s\r\n"
                                     "# step,bubble,slice,region,spawn_hash,x,y,z,radius,"
                                     "subtitle_hash,audio_tag,label\r\n"
                                     "# @,delay_ms   |   +,subtitle_hash,dwell_ms\r\n",
                                     static_cast<int>(kMagicV3.size()),
                                     kMagicV3.data(),
                                     static_cast<int>(destination_of(value).size()),
                                     destination_of(value).data(),
                                     value.sequential ? "sequential" : "free",
                                     static_cast<int>(author.size()),
                                     author.data(),
                                     static_cast<int>(description.size()),
                                     description.data(),
                                     static_cast<int>(gameBuild.size()),
                                     gameBuild.data());
    if (header <= 0 || static_cast<std::size_t>(header) >= document.size()) {
        report_fail("save", "header");
        return false;
    }
    used = static_cast<std::size_t>(header);
    for (std::size_t index = 0; index < value.count; ++index) {
        if (!append_step(value.steps[index], index + 1, document, used)
            || !append_continuations(value.steps[index], document, used)) {
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
