/**
 * The catalog of the game's localized strings, which is what makes a subtitle findable by its text.
 *
 * Built once per game build and cached on disk, in the same shape `entity_name_cache` uses: a JSON
 * document with a generator marker, staged through a temporary sibling and renamed into place.
 *
 * The build runs on the game's own pump, so it walks one string container per slice. A whole sweep
 * in one call decompresses every package block it touches, which would stall the frame it ran on.
 *
 * Storage is one text blob plus fixed rows pointing into it. A row per string with inline text would
 * cost an order of magnitude more memory for a table this size, and nothing needs the text in place.
 */

#include "subtitle_catalog.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/filesystem/temporary_sibling.h"
#include "../../../core/logging/log.h"
#include "../items/packages/internal.h"
#include "string_table.h"

namespace sunrise::client::content::strings::catalog {
namespace {

namespace package_reader = middleware::content::packages::reader;

/** The cache lives beside the other generated artifacts. */
constexpr std::wstring_view kCacheSuffix = L"\\Subtitles.json";
constexpr std::wstring_view kTemporarySuffix = L".tmp";
/** Bumping this invalidates every cache written by an older build of the walk. */
constexpr std::string_view kMarker = "\"generator\": \"sunrise_string_catalog_v1\"";
/** The member the reader scans for before walking rows. */
constexpr std::string_view kRowsMember = "\"strings\"";
/** Largest cache accepted, matching the ceiling the entity-name cache reads under. */
constexpr std::uint64_t kMaximumCacheBytes = 32ULL * 1024ULL * 1024ULL;
/** Strings the catalog holds. The table is large, and a build reaching this stops and says so. */
constexpr std::size_t kMaximumRows = 1U << 19U;
/** Text bytes the catalog holds across every string. */
constexpr std::size_t kMaximumTextBytes = 24U * 1024U * 1024U;
/** Containers one slice walks. One keeps the stall inside a frame's budget. */
constexpr std::size_t kContainersPerSlice = 1;
/** Shortest search term accepted, because one letter matches most of the table. */
constexpr std::size_t kMinimumNeedle = 2;

/** One catalogued string, pointing into the text blob. */
struct Row {
    std::uint32_t hash{};
    std::uint32_t offset{};
    std::uint16_t length{};
};

/** What a build carries between slices. */
struct Build {
    /** Installed packages directory, which is what the walk reads from. */
    core::path::Buffer directory{};
    std::vector<std::uint32_t> containers{};
    std::size_t cursor{};
    std::unique_ptr<package_reader::Scratch> scratch{};
    package_reader::BlockKeys keys{};
    Container container{};
    std::string decoded{};
    /** Set once the container sweep has run. The sweep is its own slice. */
    bool swept{};
};

SRWLOCK g_lock{SRWLOCK_INIT};
std::vector<Row> g_rows{};
std::string g_text{};
core::path::Buffer g_path{};
bool g_pathResolved{};
bool g_ready{};
bool g_failed{};
bool g_loadAttempted{};
const char* g_failure{};
std::unique_ptr<Build> g_build{};

/** Reports one catalog outcome on the Client channel. */
void report(core::log::Level level, const char* stage, const char* detail) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=subtitles stage=%s result=%s", stage, detail);
    if (written > 0) {
        core::log::write(
            core::log::Channel::client, level, {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Records a failure and ends any running build. */
void fail(const char* stage) noexcept {
    g_failed = true;
    g_failure = stage;
    g_build.reset();
    report(core::log::Level::warn, stage, "fail");
}

/** @return Lowercased byte, for a search that ignores case without touching locale. */
[[nodiscard]] char lowered(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

/** Appends one decoded string to the catalog. @return False when a ceiling was reached. */
[[nodiscard]] bool append_row(std::uint32_t hash, std::string_view text) noexcept {
    if (text.empty()) {
        // A string that decoded to nothing is not findable and not worth a row.
        return true;
    }
    if (g_rows.size() >= kMaximumRows || g_text.size() >= kMaximumTextBytes) {
        return false;
    }
    const std::size_t length =
        (std::min)(text.size(), (std::min)(kMaximumTextBytes - g_text.size(), std::size_t{0xFFFF}));
    Row row{};
    row.hash = hash;
    row.offset = static_cast<std::uint32_t>(g_text.size());
    row.length = static_cast<std::uint16_t>(length);
    g_text.append(text.data(), length);
    g_rows.push_back(row);
    return true;
}

/** @param row Catalogued row. @return Its text as a view into the blob. */
[[nodiscard]] std::string_view text_of(const Row& row) noexcept {
    if (row.offset > g_text.size() || row.length > g_text.size() - row.offset) {
        return {};
    }
    return std::string_view(g_text).substr(row.offset, row.length);
}

/** Copies one row into a caller-owned match. */
void store_match(const Row& row, Match& output) noexcept {
    output = {};
    const std::string_view text = text_of(row);
    const std::size_t length = (std::min)(text.size(), output.text.size());
    std::copy_n(text.begin(), length, output.text.begin());
    output.hash = row.hash;
    output.length = static_cast<std::uint8_t>(length);
}

/** Escapes one string into a JSON literal. @return False when it does not fit. */
[[nodiscard]] bool append_escaped(std::string_view text, std::string& document) noexcept {
    for (const char value : text) {
        switch (value) {
        case '"':
            document.append("\\\"");
            break;
        case '\\':
            document.append("\\\\");
            break;
        default:
            // Control bytes have no place in a subtitle and no short JSON escape worth keeping.
            if (static_cast<unsigned char>(value) >= 0x20) {
                document.push_back(value);
            }
            break;
        }
    }
    return true;
}

/** Writes the catalog to disk through a temporary sibling. @return True when it landed. */
[[nodiscard]] bool store_cache() noexcept {
    if (!g_pathResolved) {
        return false;
    }
    core::path::remove_stale_siblings(g_path.chars.data());
    core::path::Buffer temporary = g_path;
    if (!core::path::append(temporary, kTemporarySuffix)) {
        return false;
    }

    std::string document{};
    document.reserve(g_text.size() + g_rows.size() * 32 + 128);
    document.append("{\n  ").append(kMarker).append(",\n  ").append(kRowsMember).append(": [\n");
    for (std::size_t index = 0; index < g_rows.size(); ++index) {
        std::array<char, 32> prefix{};
        (void)std::snprintf(prefix.data(),
                            prefix.size(),
                            "    { \"h\": %u, \"t\": \"",
                            static_cast<unsigned>(g_rows[index].hash));
        document.append(prefix.data());
        (void)append_escaped(text_of(g_rows[index]), document);
        document.append(index + 1 == g_rows.size() ? "\" }\n" : "\" },\n");
    }
    document.append("  ]\n}\n");

    const HANDLE file = CreateFileW(temporary.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool complete = WriteFile(file,
                              document.data(),
                              static_cast<DWORD>(document.size()),
                              &written,
                              nullptr)
                        != FALSE
                    && written == static_cast<DWORD>(document.size());
    complete = CloseHandle(file) != FALSE && complete;
    if (!complete) {
        (void)DeleteFileW(temporary.chars.data());
        return false;
    }
    if (MoveFileExW(temporary.chars.data(), g_path.chars.data(), MOVEFILE_REPLACE_EXISTING)
        == FALSE) {
        (void)DeleteFileW(temporary.chars.data());
        return false;
    }
    return true;
}

/** Reads one unsigned field that a cursor is parked on. */
[[nodiscard]] bool scan_unsigned(std::string_view document,
                                 std::size_t& cursor,
                                 std::uint32_t& output) noexcept {
    while (cursor < document.size() && (document[cursor] == ' ' || document[cursor] == ':')) {
        ++cursor;
    }
    std::uint64_t value = 0;
    const std::size_t begin = cursor;
    while (cursor < document.size() && document[cursor] >= '0' && document[cursor] <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(document[cursor] - '0');
        if (value > 0xFFFFFFFFULL) {
            return false;
        }
        ++cursor;
    }
    if (cursor == begin) {
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

/**
 * Loads the cache.
 *
 * Scanned rather than parsed, which is how the entity-name cache is read back too: the document is
 * written by this module, so the shapes it has to accept are the shapes it emits.
 *
 * @return True when a cache with a current marker was read.
 */
[[nodiscard]] bool load_cache() noexcept {
    if (!g_pathResolved) {
        return false;
    }
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER length{};
    std::vector<char> bytes{};
    bool readOk = GetFileSizeEx(file, &length) != FALSE && length.QuadPart > 0
                  && static_cast<std::uint64_t>(length.QuadPart) <= kMaximumCacheBytes;
    if (readOk) {
        bytes.resize(static_cast<std::size_t>(length.QuadPart));
        DWORD read = 0;
        readOk = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
                     != FALSE
                 && read == bytes.size();
    }
    (void)CloseHandle(file);
    if (!readOk) {
        return false;
    }

    const std::string_view document(bytes.data(), bytes.size());
    if (document.find(kMarker) == std::string_view::npos) {
        // Written by an older walk, so it is discarded rather than trusted.
        return false;
    }
    std::size_t cursor = document.find(kRowsMember);
    if (cursor == std::string_view::npos) {
        return false;
    }
    g_rows.clear();
    g_text.clear();
    while (true) {
        const std::size_t hashAt = document.find("\"h\"", cursor);
        if (hashAt == std::string_view::npos) {
            break;
        }
        cursor = hashAt + 3;
        std::uint32_t hash = 0;
        if (!scan_unsigned(document, cursor, hash)) {
            break;
        }
        const std::size_t textAt = document.find("\"t\"", cursor);
        if (textAt == std::string_view::npos) {
            break;
        }
        const std::size_t open = document.find('"', textAt + 3);
        if (open == std::string_view::npos) {
            break;
        }
        std::string text{};
        std::size_t at = open + 1;
        bool closed = false;
        while (at < document.size()) {
            if (document[at] == '\\' && at + 1 < document.size()) {
                text.push_back(document[at + 1]);
                at += 2;
                continue;
            }
            if (document[at] == '"') {
                closed = true;
                break;
            }
            text.push_back(document[at]);
            ++at;
        }
        cursor = at + 1;
        if (!closed || !append_row(hash, text)) {
            break;
        }
    }
    return !g_rows.empty();
}

/** @param build Build to read from. @return Its package directory as a bounded view. */
[[nodiscard]] std::wstring_view directory_of(const Build& build) noexcept {
    return {build.directory.chars.data(), build.directory.length};
}

/**
 * Sets a build up without doing any package work yet.
 * The container sweep walks every installed package, so it belongs in a slice rather than in the
 * call that presses the button.
 * @return True when the build can start.
 */
[[nodiscard]] bool begin_build_locked() noexcept {
    g_rows.clear();
    g_text.clear();
    g_ready = false;
    g_failed = false;
    g_failure = nullptr;
    auto build = std::make_unique<Build>();
    if (!build) {
        g_failure = "storage";
        return false;
    }
    if (!client::content::items::packages::package_directory(build->directory)) {
        g_failure = "directory";
        return false;
    }
    if (!client::content::items::packages::collect_keys(build->keys)) {
        // The keys come out of the running client, so there is nothing to build without a game.
        g_failure = "keys";
        return false;
    }
    build->scratch = std::make_unique<package_reader::Scratch>();
    if (!build->scratch) {
        g_failure = "storage";
        return false;
    }
    g_build = std::move(build);
    return true;
}

/** Runs the container sweep. @return True when at least one container was found. */
[[nodiscard]] bool sweep_locked(Build& build) noexcept {
    const package_reader::Source source{directory_of(build), &build.keys};
    build.swept = true;
    return collect_containers(source, *build.scratch, build.containers)
           && !build.containers.empty();
}

/**
 * Walks one container, cataloguing every string it holds.
 * @return False when a catalog ceiling was reached, which ends the build early.
 */
[[nodiscard]] bool walk_container_locked(Build& build) noexcept {
    const package_reader::Source source{directory_of(build), &build.keys};
    const std::uint32_t tag = build.containers[build.cursor++];
    if (!open_container(source, *build.scratch, tag, build.container)
        || !open_data(source, *build.scratch, build.container)) {
        // A container this walk cannot read costs its strings and nothing else.
        return true;
    }
    const std::uint64_t total = count(build.container);
    for (std::uint64_t index = 0; index < total; ++index) {
        std::uint32_t hash = 0;
        if (!hash_at(build.container, index, hash)
            || !decode(build.container, index, build.decoded)) {
            continue;
        }
        if (!append_row(hash, build.decoded)) {
            return false;
        }
    }
    return true;
}

/** Ends a finished build, writing the cache. */
void finish_build_locked(bool complete) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=subtitles stage=build rows=%zu result=%s",
                                      g_rows.size(),
                                      complete ? "ok" : "capped");
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    package_reader::close_files(*g_build->scratch);
    package_reader::release_caches();
    g_build.reset();
    g_ready = !g_rows.empty();
    if (!g_ready) {
        fail("build");
        return;
    }
    if (!store_cache()) {
        // The catalog is usable in memory, so a failed write costs the next launch, not this one.
        report(core::log::Level::warn, "store", "fail");
    }
}

} // namespace

/** Resolves the cache path. */
void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_rows.clear();
    g_text.clear();
    g_ready = false;
    g_failed = false;
    g_failure = nullptr;
    g_loadAttempted = false;
    g_build.reset();
    g_path = {};
    g_pathResolved = core::path::artifact_directory(module, g_path)
                     && core::path::append(g_path, kCacheSuffix);
    if (!g_pathResolved) {
        report(core::log::Level::warn, "initialize", "fail");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Drops the catalog and the resolved path. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_build.reset();
    g_rows.clear();
    g_rows.shrink_to_fit();
    g_text.clear();
    g_text.shrink_to_fit();
    g_ready = false;
    g_failed = false;
    g_failure = nullptr;
    g_loadAttempted = false;
    g_path = {};
    g_pathResolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Runs one bounded slice of work. */
void service(std::uint64_t now) noexcept {
    static_cast<void>(now);
    AcquireSRWLockExclusive(&g_lock);
    if (!g_loadAttempted) {
        g_loadAttempted = true;
        if (load_cache()) {
            g_ready = true;
            report(core::log::Level::info, "load", "ok");
        }
    }
    if (!g_build) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    Build& build = *g_build;
    if (!build.swept) {
        // The sweep is its own slice: it walks every installed package once.
        if (!sweep_locked(build)) {
            fail("sweep");
        }
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    bool room = true;
    for (std::size_t slice = 0; slice < kContainersPerSlice && room; ++slice) {
        if (build.cursor >= build.containers.size()) {
            break;
        }
        room = walk_container_locked(build);
    }
    if (!room || build.cursor >= build.containers.size()) {
        finish_build_locked(room);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Starts a build, discarding the current catalog. */
void rebuild() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!begin_build_locked()) {
        fail(g_failure != nullptr ? g_failure : "build");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Reports how far the build has got. */
Progress progress() noexcept {
    AcquireSRWLockShared(&g_lock);
    Progress value{};
    value.rows = g_rows.size();
    value.ready = g_ready;
    value.failed = g_failed;
    if (g_build) {
        value.building = true;
        value.containers = g_build->containers.size();
        value.containersDone = g_build->cursor;
    }
    ReleaseSRWLockShared(&g_lock);
    return value;
}

/** Names the step that failed. */
std::string_view failure() noexcept {
    AcquireSRWLockShared(&g_lock);
    const char* const value = g_failure;
    ReleaseSRWLockShared(&g_lock);
    return value != nullptr ? std::string_view(value) : std::string_view{};
}

/** Copies the catalogued strings whose text contains one term. */
std::size_t search(std::string_view needle, std::span<Match> output) noexcept {
    if (needle.size() < kMinimumNeedle || output.empty()) {
        return 0;
    }
    std::string lower{};
    lower.reserve(needle.size());
    for (const char value : needle) {
        lower.push_back(lowered(value));
    }

    std::size_t found = 0;
    AcquireSRWLockShared(&g_lock);
    if (g_ready) {
        for (const Row& row : g_rows) {
            if (found >= output.size()) {
                break;
            }
            const std::string_view text = text_of(row);
            const auto at = std::search(text.begin(),
                                        text.end(),
                                        lower.begin(),
                                        lower.end(),
                                        [](char left, char right) noexcept {
                                            return lowered(left) == right;
                                        });
            if (at != text.end()) {
                store_match(row, output[found++]);
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

/** Resolves one string hash to its catalogued text. */
bool text_for(std::uint32_t hash, Match& output) noexcept {
    output = {};
    bool found = false;
    AcquireSRWLockShared(&g_lock);
    if (g_ready) {
        for (const Row& row : g_rows) {
            if (row.hash != hash) {
                continue;
            }
            store_match(row, output);
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

} // namespace sunrise::client::content::strings::catalog
