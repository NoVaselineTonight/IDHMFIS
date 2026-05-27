// project_io.cpp — Recent files list, validation, and high-level open/save.

#include "project_io.h"
#include "serialization.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <cstdio>
#include <stdexcept>
#include <filesystem>

#if defined(_WIN32) || defined(IDHMFIS_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shlobj.h>
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Platform: APPDATA directory
// ─────────────────────────────────────────────────────────────────────────────
static std::filesystem::path idhmfis_config_dir() {
#if defined(_WIN32) || defined(IDHMFIS_WINDOWS)
    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf)))
        return std::filesystem::path(buf) / "IDHMFIS";
    const char* appdata = std::getenv("APPDATA");
    if (appdata) return std::filesystem::path(appdata) / "IDHMFIS";
    return std::filesystem::temp_directory_path() / "IDHMFIS";
#elif defined(__APPLE__) || defined(IDHMFIS_MACOS)
    const char* home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / "Library" / "Application Support" / "IDHMFIS";
    return std::filesystem::temp_directory_path() / "IDHMFIS";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::filesystem::path(xdg) / "IDHMFIS";
    const char* home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / ".config" / "IDHMFIS";
    return std::filesystem::temp_directory_path() / "IDHMFIS";
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  UTC timestamp for recent entries
// ─────────────────────────────────────────────────────────────────────────────
static std::string utc_now_iso8601() {
    auto now   = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val{};
#if defined(_MSC_VER) || defined(_WIN32) || defined(IDHMFIS_WINDOWS)
    gmtime_s(&tm_val, &tt);
#else
    gmtime_r(&tt, &tm_val);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_val);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  RecentFiles
// ─────────────────────────────────────────────────────────────────────────────
std::filesystem::path RecentFiles::recent_file_path() {
    return idhmfis_config_dir() / "recent.json";
}

RecentFiles RecentFiles::load() {
    RecentFiles rf;
    auto path = recent_file_path();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return rf;

    std::ifstream ifs(path);
    if (!ifs.is_open()) return rf;

    try {
        nlohmann::json j;
        ifs >> j;

        if (j.is_array()) {
            for (const auto& item : j) {
                RecentFileEntry entry;
                entry.path        = item.value("path",        std::string{});
                entry.name        = item.value("name",        std::string{});
                entry.last_opened = item.value("last_opened", std::string{});
                if (!entry.path.empty())
                    rf.entries_.push_back(std::move(entry));
            }
        }
    } catch (...) {
        // Corrupt recent.json — return empty list
        rf.entries_.clear();
    }

    return rf;
}

void RecentFiles::save() const {
    auto dir = recent_file_path().parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    nlohmann::json j = nlohmann::json::array();
    for (const auto& entry : entries_) {
        j.push_back({
            {"path",        entry.path},
            {"name",        entry.name},
            {"last_opened", entry.last_opened}
        });
    }

    auto tmp = recent_file_path().string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) return;
        ofs << j.dump(2);
    }
    std::filesystem::rename(tmp, recent_file_path(), ec);
    if (ec) {
        std::filesystem::copy_file(tmp, recent_file_path(),
            std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
}

void RecentFiles::push(const std::string& path, const std::string& project_name) {
    // Remove any existing entry for this path
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [&](const RecentFileEntry& e) { return e.path == path; }),
        entries_.end());

    // Insert at front
    entries_.insert(entries_.begin(),
        RecentFileEntry{ path, project_name, utc_now_iso8601() });

    // Trim to max
    if (static_cast<int>(entries_.size()) > kMaxEntries)
        entries_.resize(static_cast<size_t>(kMaxEntries));
}

void RecentFiles::remove(const std::string& path) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [&](const RecentFileEntry& e) { return e.path == path; }),
        entries_.end());
}

void RecentFiles::prune_missing() {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [](const RecentFileEntry& e) {
            std::error_code ec;
            return !std::filesystem::exists(e.path, ec);
        }),
        entries_.end());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Validation
// ─────────────────────────────────────────────────────────────────────────────
ValidationResult validate_project_file(const std::string& path) {
    ValidationResult result;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        result.error = "File does not exist: " + path;
        return result;
    }

    if (std::filesystem::path(path).extension() != ".idhmfis") {
        result.error = "File does not have the .idhmfis extension: " + path;
        return result;
    }

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        result.error = "Cannot open file: " + path;
        return result;
    }

    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const nlohmann::json::parse_error& e) {
        result.error = std::string("JSON parse error: ") + e.what();
        return result;
    }

    if (!j.is_object()) {
        result.error = "Root element is not a JSON object";
        return result;
    }

    if (!j.contains("schema_version")) {
        result.error = "Missing required field: schema_version";
        return result;
    }

    result.schema_version_found = j.value("schema_version", std::string{});

    {
        auto parse_ver = [](const std::string& v) -> std::tuple<int,int,int> {
            int maj = 0, min = 0, patch = 0;
            std::sscanf(v.c_str(), "%d.%d.%d", &maj, &min, &patch);
            return {maj, min, patch};
        };
        auto file_ver    = parse_ver(result.schema_version_found);
        auto current_ver = parse_ver(kCurrentSchemaVersion);

        if (file_ver > current_ver) {
            result.error = "This file was created by a newer version of IDHMFIS (schema "
                         + result.schema_version_found
                         + "). Please update the application.";
            result.ok = false;
            return result;
        }
    }

    result.ok = true;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  High-level open/save
// ─────────────────────────────────────────────────────────────────────────────
Project open_project(const std::string& path) {
    ValidationResult vr = validate_project_file(path);
    if (!vr.ok) {
        throw ProjectLoadError(
            "Validation failed for '" + path + "': " + vr.error,
            ProjectLoadError::Reason::InvalidData);
    }

    Project project = Project::load(path);

    // Update recent files
    try {
        auto recent = RecentFiles::load();
        recent.push(path, project.name);
        recent.save();
    } catch (...) {
        // Never fail the open because recent.json couldn't be updated
    }

    return project;
}

bool save_project(const Project& project, const std::string& path) {
    bool ok = project.save(path);

    if (ok) {
        try {
            auto recent = RecentFiles::load();
            recent.push(path, project.name);
            recent.save();
        } catch (...) {}
    }

    return ok;
}

std::string default_filename(const std::string& project_name) {
    std::string safe;
    safe.reserve(project_name.size());
    for (char c : project_name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            safe += c;
        else if (c == ' ')
            safe += '_';
    }
    if (safe.empty()) safe = "untitled";
    return safe + "." + kProjectExtension;
}

} // namespace idhmfis
