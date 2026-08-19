/**
 * Sharing roteiros between installs. A roteiro is already one portable text file, so sharing is a
 * folder both ends agree on plus the checks that stop a bad file from costing captured work.
 *
 * The project has no file dialog and this does not introduce one: files are dropped into
 * `<game>\Sunrise\playbooks\shared\`, and the listing below is what the page shows.
 */

#include "playbook_share.h"

#include <Windows.h>

#include <algorithm>

#include "../../core/filesystem/path.h"
#include "../../state/build_data/runtime.h"
#include "internal.h"
#include "playbook.h"

namespace sunrise::client::playbook::share {
namespace {

/** The folder both ends of a share agree on. */
constexpr std::wstring_view kSharedSuffix = L"\\shared";
/** Filter the listing enumerates under. */
constexpr std::wstring_view kFilter = L"\\*.csv";

SRWLOCK g_lock{SRWLOCK_INIT};
core::path::Buffer g_local{};
core::path::Buffer g_shared{};
bool g_resolved{};

/** @return True when a wide leaf name is a roteiro file this module wrote or accepts. */
[[nodiscard]] bool leaf_destination(std::wstring_view leaf, Roteiro& output) noexcept {
    if (leaf.size() <= internal::kFileExtension.size()) {
        return false;
    }
    const std::wstring_view stem = leaf.substr(0, leaf.size() - internal::kFileExtension.size());
    if (stem.size() > output.destination.size()) {
        return false;
    }
    output.destinationLength = 0;
    for (const wchar_t value : stem) {
        // Package names are lowercase identifiers, so anything else is not one of ours.
        const bool ok = (value >= L'a' && value <= L'z') || (value >= L'0' && value <= L'9')
                        || value == L'_';
        if (!ok) {
            return false;
        }
        output.destination[output.destinationLength++] = static_cast<char>(value);
    }
    return output.destinationLength != 0;
}

/** Builds one path under a resolved folder. */
[[nodiscard]] bool path_in(const core::path::Buffer& directory,
                           std::string_view destination,
                           core::path::Buffer& output) noexcept {
    return g_resolved && internal::resolve_path(directory, destination, output);
}

} // namespace

/** Resolves and creates the shared folder. */
void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_local = {};
    g_shared = {};
    g_resolved = core::path::artifact_directory(module, g_local)
                 && core::path::append(g_local, internal::kDirectorySuffix);
    if (g_resolved) {
        g_shared = g_local;
        g_resolved = core::path::append(g_shared, kSharedSuffix);
    }
    if (!g_resolved) {
        internal::report_fail("share", "path");
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    // The playbook runtime creates the parent, and this creates the child. Both tolerate existing.
    if (CreateDirectoryW(g_shared.chars.data(), nullptr) == FALSE
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_resolved = false;
        internal::report_fail("share", "directory");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Drops the resolved folders. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_local = {};
    g_shared = {};
    g_resolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Writes the loaded roteiro into the shared folder. */
bool export_current() noexcept {
    const Roteiro roteiro = get();
    const std::string_view destination = playbook::destination_of(roteiro);
    if (destination.empty() || roteiro.count == 0) {
        internal::report_fail("export", "empty");
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    core::path::Buffer path{};
    const bool built = path_in(g_shared, destination, path);
    ReleaseSRWLockShared(&g_lock);
    if (!built) {
        internal::report_fail("export", "path");
        return false;
    }
    return internal::save(path.chars.data(), roteiro);
}

/** Lists the shared folder. */
std::size_t list(std::span<Entry> output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool resolved = g_resolved;
    core::path::Buffer shared = g_shared;
    core::path::Buffer local = g_local;
    ReleaseSRWLockShared(&g_lock);
    if (!resolved || output.empty()) {
        return 0;
    }
    core::path::Buffer filter = shared;
    if (!core::path::append(filter, kFilter)) {
        return 0;
    }

    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(filter.chars.data(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return 0;
    }
    std::size_t count = 0;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        Roteiro roteiro{};
        if (!leaf_destination(found.cFileName, roteiro)) {
            continue;
        }
        core::path::Buffer path{};
        if (!internal::resolve_path(shared, playbook::destination_of(roteiro), path)
            || !internal::load(path.chars.data(), roteiro) || roteiro.count == 0) {
            continue;
        }
        Entry& entry = output[count];
        entry = {};
        entry.destination = roteiro.destination;
        entry.destinationLength = roteiro.destinationLength;
        entry.author = roteiro.author;
        entry.description = roteiro.description;
        entry.steps = roteiro.count;
        state::build_data::scenarios::Definition layout{};
        entry.destinationKnown =
            state::build_data::find_scenario_layout(playbook::destination_of(roteiro), layout);
        core::path::Buffer localPath{};
        entry.collides =
            internal::resolve_path(local, playbook::destination_of(roteiro), localPath)
            && GetFileAttributesW(localPath.chars.data()) != INVALID_FILE_ATTRIBUTES;
        ++count;
    } while (count < output.size() && FindNextFileW(search, &found) != FALSE);
    (void)FindClose(search);
    return count;
}

/** Installs one shared roteiro locally. */
bool import_entry(std::string_view destination, bool replace) noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool resolved = g_resolved;
    core::path::Buffer shared = g_shared;
    core::path::Buffer local = g_local;
    ReleaseSRWLockShared(&g_lock);
    core::path::Buffer sharedPath{};
    core::path::Buffer localPath{};
    if (!resolved || !internal::resolve_path(shared, destination, sharedPath)
        || !internal::resolve_path(local, destination, localPath)) {
        internal::report_fail("import", "path");
        return false;
    }
    if (!replace && GetFileAttributesW(localPath.chars.data()) != INVALID_FILE_ATTRIBUTES) {
        // A local roteiro is captured work, so replacing it is never implicit.
        internal::report_fail("import", "collision");
        return false;
    }

    Roteiro roteiro{};
    const std::size_t length =
        (std::min)(destination.size(), roteiro.destination.size());
    std::copy_n(destination.begin(), length, roteiro.destination.begin());
    roteiro.destinationLength = static_cast<std::uint8_t>(length);
    if (!internal::load(sharedPath.chars.data(), roteiro) || roteiro.count == 0) {
        internal::report_fail("import", "read");
        return false;
    }
    if (!internal::save(localPath.chars.data(), roteiro)) {
        return false;
    }
    // The runtime only reloads on a destination change, so an import of the current one needs this.
    reload();
    return true;
}

} // namespace sunrise::client::playbook::share
