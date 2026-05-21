#pragma once
// project_io.h — Higher-level file I/O: recent files list and file validation.

#include <string>
#include <vector>
#include <filesystem>
#include <chrono>

#include "project.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Recent files list
//  Stored in %APPDATA%\IDHMFIS\recent.json (max 20 entries)
// ─────────────────────────────────────────────────────────────────────────────
struct RecentFileEntry {
    std::string path;
    std::string name;           // project name (cached for display)
    std::string last_opened;    // ISO 8601 UTC
};

class RecentFiles {
public:
    static constexpr int kMaxEntries = 20;

    // Load the list from disk (creates empty list if file does not exist).
    static RecentFiles load();

    // Save the list to disk.
    void save() const;

    // Add or promote an entry to the front of the list.
    void push(const std::string& path, const std::string& project_name);

    // Remove an entry by path.
    void remove(const std::string& path);

    // Remove entries whose paths no longer exist on disk.
    void prune_missing();

    const std::vector<RecentFileEntry>& entries() const { return entries_; }

    // Path to the recent.json file.
    static std::filesystem::path recent_file_path();

private:
    std::vector<RecentFileEntry> entries_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  File validation
// ─────────────────────────────────────────────────────────────────────────────
struct ValidationResult {
    bool        ok = false;
    std::string error;    // human-readable, empty if ok
    std::string schema_version_found;
};

// Validate a file without fully loading it (fast check).
ValidationResult validate_project_file(const std::string& path);

// ─────────────────────────────────────────────────────────────────────────────
//  High-level load/save wrappers (update recent files automatically)
// ─────────────────────────────────────────────────────────────────────────────

// Validate then load; throws ProjectLoadError on failure.
// Updates the recent files list on success.
Project open_project(const std::string& path);

// Atomic save + update recent files list.
// Returns true on success; throws std::runtime_error on failure.
bool save_project(const Project& project, const std::string& path);

// Return the default file extension (without dot).
inline constexpr const char* kProjectExtension = "idhmfis";

// Build a default file name from a project name.
std::string default_filename(const std::string& project_name);

} // namespace idhmfis
