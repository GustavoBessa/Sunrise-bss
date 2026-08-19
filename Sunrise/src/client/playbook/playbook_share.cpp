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
#include <memory>

#include "../../core/filesystem/path.h"
#include "../../state/build_data/runtime.h"
#include "internal.h"
#include "playbook.h"

namespace sunrise::client::playbook::share {
namespace {

/** The folder both ends of a share agree on. */
constexpr std::wstring_view kSharedSuffix = L"\\shared";
/** Filter the listing enumerates under. */
constexpr std::wstring_view kFilter = L"\\*.json";

SRWLOCK g_lock{SRWLOCK_INIT};
core::path::Buffer g_local{};
core::path::Buffer g_shared{};
/**
 * Working paths, held here rather than on the caller stack.
 *
 * One `Buffer` is 64 KB, and these run on the game's render thread. A handful of locals would put a
 * third of a megabyte on a stack this module does not own, so the lock owns them instead.
 */
core::path::Buffer g_scratchA{};
core::path::Buffer g_scratchB{};
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

/**
 * @param destination Package name from a listing.
 * @return True when this install carries that destination's bubble layout.
 *
 * The layout row is wide, so it is read into heap storage rather than onto the caller's stack.
 */
[[nodiscard]] bool layout_known(std::string_view destination) noexcept {
    auto layout = std::make_unique<state::build_data::scenarios::Definition>();
    return layout && state::build_data::find_scenario_layout(destination, *layout);
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
    AcquireSRWLockExclusive(&g_lock);
    const bool built = g_resolved && internal::resolve_path(g_shared, destination, g_scratchA);
    const bool written = built && internal::save(g_scratchA.chars.data(), roteiro);
    ReleaseSRWLockExclusive(&g_lock);
    if (!built) {
        internal::report_fail("export", "path");
    }
    return written;
}

/** Lists the shared folder. */
std::size_t list(std::span<Entry> output) noexcept {
    if (output.empty()) {
        return 0;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_scratchA = g_shared;
    if (!g_resolved || !core::path::append(g_scratchA, kFilter)) {
        ReleaseSRWLockExclusive(&g_lock);
        return 0;
    }

    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(g_scratchA.chars.data(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&g_lock);
        return 0;
    }
    // Reused across entries, because reading a roteiro to count its steps is the point of the walk.
    auto scratch = std::make_unique<Roteiro>();
    std::size_t count = 0;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !scratch) {
            continue;
        }
        *scratch = {};
        Roteiro& roteiro = *scratch;
        if (!leaf_destination(found.cFileName, roteiro)) {
            continue;
        }
        const std::string_view destination = playbook::destination_of(roteiro);
        if (!internal::resolve_path(g_shared, destination, g_scratchA)
            || !internal::load(g_scratchA.chars.data(), roteiro) || roteiro.count == 0) {
            continue;
        }
        Entry& entry = output[count];
        entry = {};
        entry.destination = roteiro.destination;
        entry.destinationLength = roteiro.destinationLength;
        entry.author = roteiro.author;
        entry.description = roteiro.description;
        entry.steps = roteiro.count;
        entry.destinationKnown = layout_known(destination);
        entry.collides = internal::resolve_path(g_local, destination, g_scratchB)
                         && GetFileAttributesW(g_scratchB.chars.data()) != INVALID_FILE_ATTRIBUTES;
        ++count;
    } while (count < output.size() && FindNextFileW(search, &found) != FALSE);
    (void)FindClose(search);
    ReleaseSRWLockExclusive(&g_lock);
    return count;
}

/** Installs one shared roteiro locally. */
bool import_entry(std::string_view destination, bool replace) noexcept {
    auto scratch = std::make_unique<Roteiro>();
    if (!scratch) {
        internal::report_fail("import", "storage");
        return false;
    }
    Roteiro& roteiro = *scratch;
    const std::size_t length = (std::min)(destination.size(), roteiro.destination.size());
    std::copy_n(destination.begin(), length, roteiro.destination.begin());
    roteiro.destinationLength = static_cast<std::uint8_t>(length);

    const char* stage = nullptr;
    bool installed = false;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_resolved || !internal::resolve_path(g_shared, destination, g_scratchA)
        || !internal::resolve_path(g_local, destination, g_scratchB)) {
        stage = "path";
    } else if (!replace && GetFileAttributesW(g_scratchB.chars.data()) != INVALID_FILE_ATTRIBUTES) {
        // A local roteiro is captured work, so replacing it is never implicit.
        stage = "collision";
    } else if (!internal::load(g_scratchA.chars.data(), roteiro) || roteiro.count == 0) {
        stage = "read";
    } else {
        installed = internal::save(g_scratchB.chars.data(), roteiro);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (stage != nullptr) {
        internal::report_fail("import", stage);
        return false;
    }
    if (!installed) {
        return false;
    }
    // The runtime only reloads on a destination change, so an import of the current one needs this.
    // Called outside this module's lock, because it takes the playbook runtime's own.
    reload();
    return true;
}

} // namespace sunrise::client::playbook::share
