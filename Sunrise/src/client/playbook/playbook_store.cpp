/**
 * The roteiro file format. Version 4 onwards is a JSON object; versions 1-3 were comma-separated
 * text and are still parsed as a read-only backward-compat path.
 *
 * The project's own JSON primitives are private members of the Core settings parser, which gates
 * the boot and must not grow a second caller. This module therefore carries a minimal hand-rolled
 * JSON serialiser and scanner for the one schema it owns.
 *
 * A malformed CSV step line is skipped and reported rather than failing the load, so one bad hand
 * edit cannot cost the whole roteiro.
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
 * Parses one `@` continuation line into the gate of the step above it.
 *
 * Forms:
 *  `@,<delay_ms>`       → Gate::delay
 *  `@,interaction`      → Gate::interaction
 *  `@,clear,<target>`   → Gate::clearArea
 *
 * @param line Line without its terminator, `@` included.
 * @param output Roteiro whose most recent step receives the gate.
 * @return True when the gate was stored.
 */
[[nodiscard]] bool parse_gate(std::string_view line, Roteiro& output) noexcept {
    if (output.count == 0) {
        return false;
    }
    std::string_view rest = line.substr(1);
    if (rest.empty() || rest.front() != ',') {
        return false;
    }
    rest.remove_prefix(1);
    Step& step = output.steps[output.count - 1];
    if (rest == "interaction") {
        step.gate = Gate::interaction;
        return true;
    }
    if (rest.size() > 6 && rest.substr(0, 6) == "clear,") {
        std::uint32_t target = 0;
        if (!unsigned_field(rest.substr(6), target)) {
            return false;
        }
        step.gate = Gate::clearArea;
        step.targetActorCount = static_cast<std::uint16_t>(target);
        return true;
    }
    // Legacy and current: numeric delay. Refused on the first step (nothing to follow).
    if (output.count < 2) {
        return false;
    }
    std::uint32_t delay = 0;
    if (!unsigned_field(rest, delay) || delay > kMaximumDelayMs) {
        return false;
    }
    step.gate = Gate::delay;
    step.delayMs = static_cast<std::uint16_t>(delay);
    return true;
}

/** Stores one text continuation line into a step text field. */
void store_step_text(std::string_view text,
                     std::array<char, kStepTextCapacity>& field,
                     std::uint8_t& length) noexcept {
    field = {};
    length = 0;
    for (char ch : text) {
        if (length >= kStepTextCapacity) {
            break;
        }
        if (static_cast<unsigned char>(ch) >= 0x20) {
            field[length++] = ch;
        }
    }
}

/**
 * Parses one `>` or `<` continuation line into the objective/completion text of the step above it.
 * @param line Line without its terminator, marker included.
 * @param output Roteiro whose most recent step receives the text.
 */
void parse_step_text(std::string_view line, Roteiro& output) noexcept {
    if (output.count == 0 || line.size() < 2 || line[1] != ',') {
        return;
    }
    const std::string_view text = line.substr(2);
    Step& step = output.steps[output.count - 1];
    if (line.front() == kObjectiveMarker) {
        store_step_text(text, step.objectiveText, step.objectiveTextLength);
    } else {
        store_step_text(text, step.completionText, step.completionTextLength);
    }
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
 * Appends one step's continuation lines: gate, objective text, and completion text.
 * @param step Step to write.
 * @param document Whole document storage.
 * @param used Bytes already written, advanced on success.
 * @return True when every line fit.
 */
[[nodiscard]] bool append_continuations(const Step& step,
                                        Document& document,
                                        std::size_t& used) noexcept {
    int written = 0;
    if (step.gate == Gate::delay) {
        written = std::snprintf(document.data() + used,
                                document.size() - used,
                                "%c,%u\r\n",
                                kGateMarker,
                                static_cast<unsigned>(step.delayMs));
    } else if (step.gate == Gate::interaction) {
        written = std::snprintf(document.data() + used,
                                document.size() - used,
                                "%c,interaction\r\n",
                                kGateMarker);
    } else if (step.gate == Gate::clearArea) {
        written = std::snprintf(document.data() + used,
                                document.size() - used,
                                "%c,clear,%u\r\n",
                                kGateMarker,
                                static_cast<unsigned>(step.targetActorCount));
    }
    if (written > 0) {
        if (static_cast<std::size_t>(written) >= document.size() - used) {
            return false;
        }
        used += static_cast<std::size_t>(written);
    }
    if (step.objectiveTextLength > 0) {
        const int obj = std::snprintf(document.data() + used,
                                      document.size() - used,
                                      "%c,%.*s\r\n",
                                      kObjectiveMarker,
                                      static_cast<int>(step.objectiveTextLength),
                                      step.objectiveText.data());
        if (obj <= 0 || static_cast<std::size_t>(obj) >= document.size() - used) {
            return false;
        }
        used += static_cast<std::size_t>(obj);
    }
    if (step.completionTextLength > 0) {
        const int comp = std::snprintf(document.data() + used,
                                       document.size() - used,
                                       "%c,%.*s\r\n",
                                       kCompletionMarker,
                                       static_cast<int>(step.completionTextLength),
                                       step.completionText.data());
        if (comp <= 0 || static_cast<std::size_t>(comp) >= document.size() - used) {
            return false;
        }
        used += static_cast<std::size_t>(comp);
    }
    return true;
}

// ── JSON helpers ─────────────────────────────────────────────────────────────────────────────────

/**
 * Minimal forward-only JSON scanner for the one schema this module owns.
 *
 * Every `seek_key` call searches forward from the current position so field reads in write order
 * are O(n) over the document, not O(n²). For step objects a fresh Scanner is scoped to just the
 * object's text, so each step parse is O(objectSize * fieldCount).
 */
struct JsonScanner {
    std::string_view text;

    void skip_ws() noexcept {
        while (!text.empty() && static_cast<unsigned char>(text.front()) <= ' ') {
            text.remove_prefix(1);
        }
    }

    /** Advances past `"key":` in the remaining text. @return True when found. */
    [[nodiscard]] bool seek_key(const char* key) noexcept {
        std::array<char, 80> pat{};
        const int n = std::snprintf(pat.data(), pat.size(), "\"%s\":", key);
        if (n <= 0) {
            return false;
        }
        const std::string_view sv{pat.data(), static_cast<std::size_t>(n)};
        const auto pos = text.find(sv);
        if (pos == std::string_view::npos) {
            return false;
        }
        text.remove_prefix(pos + sv.size());
        return true;
    }

    /** Reads a JSON-quoted string, handling `\"` and `\\` escapes. */
    [[nodiscard]] bool read_string(char* out, std::size_t cap, std::uint8_t& len) noexcept {
        skip_ws();
        if (text.empty() || text.front() != '"') {
            return false;
        }
        text.remove_prefix(1);
        len = 0;
        while (!text.empty() && text.front() != '"') {
            char ch = text.front();
            text.remove_prefix(1);
            if (ch == '\\' && !text.empty()) {
                ch = text.front();
                text.remove_prefix(1);
            }
            if (len < cap && static_cast<unsigned char>(ch) >= 0x20) {
                out[len++] = ch;
            }
        }
        if (!text.empty()) {
            text.remove_prefix(1); // closing quote
        }
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t& out) noexcept {
        skip_ws();
        const char* b = text.data();
        const auto r = std::from_chars(b, b + text.size(), out);
        if (r.ec != std::errc{}) {
            return false;
        }
        text.remove_prefix(static_cast<std::size_t>(r.ptr - b));
        return true;
    }

    [[nodiscard]] bool read_i32(std::int32_t& out) noexcept {
        skip_ws();
        const char* b = text.data();
        const auto r = std::from_chars(b, b + text.size(), out);
        if (r.ec != std::errc{}) {
            return false;
        }
        text.remove_prefix(static_cast<std::size_t>(r.ptr - b));
        return true;
    }

    [[nodiscard]] bool read_float(float& out) noexcept {
        skip_ws();
        std::array<char, kFieldCapacity> buf{};
        std::size_t i = 0;
        while (i < text.size() && i < buf.size() - 1) {
            const char c = text[i];
            if (c == ',' || c == ']' || c == '}' || static_cast<unsigned char>(c) <= ' ') {
                break;
            }
            buf[i++] = c;
        }
        if (i == 0) {
            return false;
        }
        char* end = nullptr;
        const float val = std::strtof(buf.data(), &end);
        if (end == buf.data() || val != val || val > 3.0e38F || val < -3.0e38F) {
            return false;
        }
        text.remove_prefix(i);
        out = val;
        return true;
    }

    [[nodiscard]] bool read_bool(bool& out) noexcept {
        skip_ws();
        if (text.size() >= 4 && text.substr(0, 4) == "true") {
            out = true;
            text.remove_prefix(4);
            return true;
        }
        if (text.size() >= 5 && text.substr(0, 5) == "false") {
            out = false;
            text.remove_prefix(5);
            return true;
        }
        return false;
    }

    /**
     * Returns the text of the next JSON object `{}` in the remaining text, including braces, and
     * advances past it. Handles nested objects and strings so braces inside strings do not confuse
     * the depth counter.
     */
    [[nodiscard]] std::string_view next_object() noexcept {
        const auto start = text.find('{');
        if (start == std::string_view::npos) {
            return {};
        }
        text.remove_prefix(start);
        int depth = 0;
        bool in_str = false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (in_str) {
                if (c == '\\') {
                    ++i; // skip escaped char
                } else if (c == '"') {
                    in_str = false;
                }
            } else if (c == '"') {
                in_str = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                if (--depth == 0) {
                    const std::string_view obj = text.substr(0, i + 1);
                    text.remove_prefix(i + 1);
                    return obj;
                }
            }
        }
        return {};
    }
};

/**
 * Appends a JSON-escaped string literal (with surrounding quotes) to the document.
 * Escapes `"` and `\`; other printable bytes pass through verbatim.
 */
[[nodiscard]] bool append_json_str(std::string_view s, Document& doc, std::size_t& used) noexcept {
    if (used >= doc.size()) {
        return false;
    }
    doc[used++] = '"';
    for (const char ch : s) {
        if (ch == '"' || ch == '\\') {
            if (used >= doc.size()) {
                return false;
            }
            doc[used++] = '\\';
        }
        if (used >= doc.size()) {
            return false;
        }
        doc[used++] = ch;
    }
    if (used >= doc.size()) {
        return false;
    }
    doc[used++] = '"';
    return true;
}

/** Appends `"key": "value",\n` (with a leading indent) to the document. */
[[nodiscard]] bool append_kv_str(const char* indent,
                                 const char* key,
                                 std::string_view val,
                                 Document& doc,
                                 std::size_t& used) noexcept {
    const int n = std::snprintf(doc.data() + used, doc.size() - used, "%s\"%s\": ", indent, key);
    if (n <= 0 || static_cast<std::size_t>(n) >= doc.size() - used) {
        return false;
    }
    used += static_cast<std::size_t>(n);
    if (!append_json_str(val, doc, used)) {
        return false;
    }
    const int n2 = std::snprintf(doc.data() + used, doc.size() - used, ",\n");
    if (n2 <= 0 || static_cast<std::size_t>(n2) >= doc.size() - used) {
        return false;
    }
    used += static_cast<std::size_t>(n2);
    return true;
}

/** Gate enum → JSON string token. */
[[nodiscard]] const char* gate_token(Gate g) noexcept {
    switch (g) {
        case Gate::delay: return "delay";
        case Gate::interaction: return "interaction";
        case Gate::clearArea: return "clearArea";
        default: return "place";
    }
}

/** JSON string token → Gate enum. Falls back to `Gate::place` for unknown values. */
[[nodiscard]] Gate gate_from_token(std::string_view token) noexcept {
    if (token == "delay") return Gate::delay;
    if (token == "interaction") return Gate::interaction;
    if (token == "clearArea") return Gate::clearArea;
    return Gate::place;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────

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

/** Builds one destination's legacy CSV file path. Same validation as `resolve_path`. */
bool resolve_legacy_path(const core::path::Buffer& directory,
                         std::string_view destination,
                         core::path::Buffer& output) noexcept {
    if (destination.empty()) {
        return false;
    }
    output = directory;
    if (!core::path::append(output, L"\\")) {
        return false;
    }
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
           && core::path::append(output, kLegacyFileExtension);
}

// ── JSON load path ────────────────────────────────────────────────────────────────────────────────

/** Parses one step JSON object. @return True when every mandatory field was valid. */
[[nodiscard]] bool parse_json_step(std::string_view obj, Step& output) noexcept {
    JsonScanner sc{obj};
    Step step{};

    std::uint32_t bubble = 0;
    std::uint32_t sliceState = 0;
    std::int32_t region = 0;
    std::uint32_t spawnHash = 0;
    float x = 0.0F, y = 0.0F, z = 0.0F, radius = kMinimumRadius;

    if (!sc.seek_key("bubble") || !sc.read_u32(bubble)) return false;
    JsonScanner sc2{obj};
    if (!sc2.seek_key("sliceState") || !sc2.read_u32(sliceState)) return false;
    JsonScanner sc3{obj};
    if (!sc3.seek_key("region") || !sc3.read_i32(region)) return false;
    JsonScanner sc4{obj};
    if (!sc4.seek_key("spawnHash") || !sc4.read_u32(spawnHash)) return false;
    JsonScanner sc5{obj};
    if (!sc5.seek_key("x") || !sc5.read_float(x)) return false;
    JsonScanner sc6{obj};
    if (!sc6.seek_key("y") || !sc6.read_float(y)) return false;
    JsonScanner sc7{obj};
    if (!sc7.seek_key("z") || !sc7.read_float(z)) return false;
    JsonScanner sc8{obj};
    if (!sc8.seek_key("radius") || !sc8.read_float(radius)) return false;

    if (radius < kMinimumRadius || radius > kMaximumRadius) return false;

    step.bubble = bubble;
    step.sliceState = sliceState;
    step.region = region;
    step.spawnHash = spawnHash;
    step.position[0] = x;
    step.position[1] = y;
    step.position[2] = z;
    step.radius = radius;

    // audioTag is optional (default = kNoAudioTag = 0).
    {
        JsonScanner sa{obj};
        std::uint32_t tag = 0;
        if (sa.seek_key("audioTag") && sa.read_u32(tag) && tag != 0) {
            step.audioTag = tag;
        }
    }

    // gate (string) + numeric parameters.
    {
        JsonScanner sg{obj};
        std::array<char, 16> gateBuf{};
        std::uint8_t gateLen = 0;
        if (sg.seek_key("gate") && sg.read_string(gateBuf.data(), gateBuf.size() - 1, gateLen)) {
            step.gate = gate_from_token({gateBuf.data(), gateLen});
        }
    }
    if (step.gate == Gate::delay) {
        JsonScanner sd{obj};
        std::uint32_t ms = 0;
        if (sd.seek_key("delayMs") && sd.read_u32(ms) && ms <= kMaximumDelayMs) {
            step.delayMs = static_cast<std::uint16_t>(ms);
        }
    }
    if (step.gate == Gate::clearArea) {
        JsonScanner sc9{obj};
        std::uint32_t target = 0;
        if (sc9.seek_key("targetActorCount") && sc9.read_u32(target)) {
            step.targetActorCount = static_cast<std::uint16_t>(target);
        }
    }

    // Text fields (optional).
    {
        JsonScanner sl{obj};
        std::uint8_t len = 0;
        if (sl.seek_key("label")) {
            sl.read_string(step.label.data(), step.label.size() - 1, len);
            step.labelLength = len;
        }
    }
    {
        JsonScanner so{obj};
        std::uint8_t len = 0;
        if (so.seek_key("objectiveText")) {
            so.read_string(step.objectiveText.data(), step.objectiveText.size() - 1, len);
            step.objectiveTextLength = len;
        }
    }
    {
        JsonScanner sc10{obj};
        std::uint8_t len = 0;
        if (sc10.seek_key("completionText")) {
            sc10.read_string(step.completionText.data(), step.completionText.size() - 1, len);
            step.completionTextLength = len;
        }
    }

    output = step;
    return true;
}

/** Parses the top-level JSON roteiro object into `output`. */
[[nodiscard]] bool load_json(std::string_view text, Roteiro& output) noexcept {
    // Minimal version check: the format key must be present.
    {
        JsonScanner sc{text};
        if (!sc.seek_key("format")) {
            report_fail("load", "magic");
            return false;
        }
    }

    // Metadata fields.
    {
        JsonScanner sc{text};
        bool seq = false;
        if (sc.seek_key("sequential") && sc.read_bool(seq)) {
            output.sequential = seq;
        }
    }
    {
        JsonScanner sc{text};
        std::uint8_t len = 0;
        if (sc.seek_key("author")) {
            sc.read_string(output.author.value.data(), output.author.value.size() - 1, len);
            output.author.length = len;
        }
    }
    {
        JsonScanner sc{text};
        std::uint8_t len = 0;
        if (sc.seek_key("description")) {
            sc.read_string(output.description.value.data(),
                           output.description.value.size() - 1,
                           len);
            output.description.length = len;
        }
    }
    {
        JsonScanner sc{text};
        std::uint8_t len = 0;
        if (sc.seek_key("game_build")) {
            sc.read_string(output.gameBuild.value.data(),
                           output.gameBuild.value.size() - 1,
                           len);
            output.gameBuild.length = len;
        }
    }

    // Steps array.
    {
        JsonScanner sc{text};
        if (!sc.seek_key("steps")) {
            return true; // No steps key means an empty roteiro — not an error.
        }
        std::size_t skipped = 0;
        for (;;) {
            const std::string_view obj = sc.next_object();
            if (obj.empty()) {
                break;
            }
            if (output.count >= output.steps.size()) {
                report_fail("load", "capacity");
                break;
            }
            Step step{};
            if (parse_json_step(obj, step)) {
                output.steps[output.count++] = step;
            } else {
                ++skipped;
            }
        }
        if (skipped != 0) {
            report_fail("load", "step");
        }
    }
    return true;
}

// ── CSV load path (backward compat, v1/v2/v3) ────────────────────────────────────────────────────

/** Parses the CSV document into `output`. Used only for pre-v4 files. */
[[nodiscard]] bool load_csv(std::string_view text, Roteiro& output) noexcept {
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
            read_metadata(line, output);
            continue;
        }
        if (!header) {
            if (line == kMagicV2 || line == kMagicV3) {
                withSubtitle = true;
            } else if (line != kMagicV1) {
                report_fail("load", "magic");
                return false;
            }
            header = true;
            continue;
        }
        if (line.front() == kLineMarker) {
            continue; // Legacy subtitle line, silently skipped.
        }
        if (line.front() == kGateMarker) {
            skipped += parse_gate(line, output) ? 0U : 1U;
            continue;
        }
        if (line.front() == kObjectiveMarker || line.front() == kCompletionMarker) {
            parse_step_text(line, output);
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

// ── Shared file I/O ───────────────────────────────────────────────────────────────────────────────

/** Opens a file, reads its content into the document buffer, and closes it. */
[[nodiscard]] bool read_file(const wchar_t* path, Document& document, DWORD& bytesRead) noexcept {
    const HANDLE file = CreateFileW(path,
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    bytesRead = 0;
    const bool ok =
        ReadFile(file, document.data(), static_cast<DWORD>(document.size() - 1), &bytesRead, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    return ok;
}

/** Reads one roteiro from disk. Detects the format by the first non-whitespace byte. */
bool load(const wchar_t* path, Roteiro& output) noexcept {
    // On the heap: a whole roteiro document is far past what belongs on the render thread's stack.
    auto storage = std::make_unique<Document>();
    if (!storage) {
        report_fail("load", "storage");
        return false;
    }
    Document& document = *storage;
    DWORD bytesRead = 0;
    if (!read_file(path, document, bytesRead)) {
        const DWORD error = GetLastError();
        // A destination with no roteiro yet is the ordinary case, not a failure.
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    std::string_view text(document.data(), bytesRead);
    // Skip leading whitespace to find the format marker.
    while (!text.empty() && static_cast<unsigned char>(text.front()) <= ' ') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        report_fail("load", "empty");
        return false;
    }
    // v4 JSON starts with `{`; all earlier versions start with the ASCII magic string.
    if (text.front() == '{') {
        return load_json(text, output);
    }
    return load_csv(text, output);
}

// ── JSON save path ────────────────────────────────────────────────────────────────────────────────

/** Appends one step as a JSON object to the document. */
[[nodiscard]] bool append_json_step(const Step& step,
                                    std::size_t ordinal,
                                    bool last,
                                    Document& doc,
                                    std::size_t& used) noexcept {
    const std::string_view label = label_of(step);
    const std::string_view objective{step.objectiveText.data(), step.objectiveTextLength};
    const std::string_view completion{step.completionText.data(), step.completionTextLength};

    const int hdr = std::snprintf(doc.data() + used,
                                  doc.size() - used,
                                  "    {\n"
                                  "      \"ordinal\": %zu,\n"
                                  "      \"bubble\": %u,\n"
                                  "      \"sliceState\": %u,\n"
                                  "      \"region\": %d,\n"
                                  "      \"spawnHash\": %u,\n"
                                  "      \"x\": %.3f,\n"
                                  "      \"y\": %.3f,\n"
                                  "      \"z\": %.3f,\n"
                                  "      \"radius\": %.1f,\n"
                                  "      \"audioTag\": %u,\n"
                                  "      \"gate\": \"%s\",\n"
                                  "      \"delayMs\": %u,\n"
                                  "      \"targetActorCount\": %u,\n",
                                  ordinal,
                                  static_cast<unsigned>(step.bubble),
                                  static_cast<unsigned>(step.sliceState),
                                  static_cast<int>(step.region),
                                  static_cast<unsigned>(step.spawnHash),
                                  static_cast<double>(step.position[0]),
                                  static_cast<double>(step.position[1]),
                                  static_cast<double>(step.position[2]),
                                  static_cast<double>(step.radius),
                                  static_cast<unsigned>(step.audioTag),
                                  gate_token(step.gate),
                                  static_cast<unsigned>(step.delayMs),
                                  static_cast<unsigned>(step.targetActorCount));
    if (hdr <= 0 || static_cast<std::size_t>(hdr) >= doc.size() - used) {
        return false;
    }
    used += static_cast<std::size_t>(hdr);

    // String fields: written with JSON escaping.
    if (!append_kv_str("      ", "label", label, doc, used)) return false;
    if (!append_kv_str("      ", "objectiveText", objective, doc, used)) return false;
    // completionText is the last field; its trailing comma is replaced by no comma when it's last.
    {
        const int n = std::snprintf(doc.data() + used, doc.size() - used, "      \"completionText\": ");
        if (n <= 0 || static_cast<std::size_t>(n) >= doc.size() - used) return false;
        used += static_cast<std::size_t>(n);
        if (!append_json_str(completion, doc, used)) return false;
        const int n2 = std::snprintf(doc.data() + used, doc.size() - used, "\n    }%s\n",
                                     last ? "" : ",");
        if (n2 <= 0 || static_cast<std::size_t>(n2) >= doc.size() - used) return false;
        used += static_cast<std::size_t>(n2);
    }
    return true;
}

/** Writes one roteiro to disk as a JSON document, replacing the file. */
bool save(const wchar_t* path, const Roteiro& value) noexcept {
    auto storage = std::make_unique<Document>();
    if (!storage) {
        report_fail("save", "storage");
        return false;
    }
    Document& document = *storage;
    std::size_t used = 0;

    const std::string_view dest = destination_of(value);
    const std::string_view author = value_of(value.author);
    const std::string_view description = value_of(value.description);
    const std::string_view gameBuild = value_of(value.gameBuild);

    // JSON header (all scalar metadata fields).
    {
        const int n = std::snprintf(document.data(),
                                    document.size(),
                                    "{\n"
                                    "  \"format\": \"sunrise_playbook\",\n"
                                    "  \"version\": 4,\n"
                                    "  \"sequential\": %s,\n",
                                    value.sequential ? "true" : "false");
        if (n <= 0 || static_cast<std::size_t>(n) >= document.size()) {
            report_fail("save", "header");
            return false;
        }
        used = static_cast<std::size_t>(n);
    }
    if (!append_kv_str("  ", "destination", dest, document, used)) {
        report_fail("save", "header");
        return false;
    }
    if (!append_kv_str("  ", "author", author, document, used)) {
        report_fail("save", "header");
        return false;
    }
    if (!append_kv_str("  ", "description", description, document, used)) {
        report_fail("save", "header");
        return false;
    }
    if (!append_kv_str("  ", "game_build", gameBuild, document, used)) {
        report_fail("save", "header");
        return false;
    }

    // Steps array.
    {
        const int n = std::snprintf(document.data() + used, document.size() - used, "  \"steps\": [\n");
        if (n <= 0 || static_cast<std::size_t>(n) >= document.size() - used) {
            report_fail("save", "header");
            return false;
        }
        used += static_cast<std::size_t>(n);
    }
    for (std::size_t index = 0; index < value.count; ++index) {
        const bool last = index + 1 == value.count;
        if (!append_json_step(value.steps[index], index + 1, last, document, used)) {
            report_fail("save", "capacity");
            return false;
        }
    }
    {
        const int n = std::snprintf(document.data() + used, document.size() - used, "  ]\n}\n");
        if (n <= 0 || static_cast<std::size_t>(n) >= document.size() - used) {
            report_fail("save", "footer");
            return false;
        }
        used += static_cast<std::size_t>(n);
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
