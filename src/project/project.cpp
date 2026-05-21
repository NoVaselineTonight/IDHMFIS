// project.cpp — Project utility functions: UUID generation, save/load, migration.

#include "serialization.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <random>
#include <iomanip>
#include <ctime>
#include <filesystem>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  UUID v4 generator
//  Uses std::mt19937_64 seeded from std::random_device so that generated IDs
//  are unique per process. Thread-safe under C++11 via function-local statics.
// ─────────────────────────────────────────────────────────────────────────────
std::string Project::new_id() {
    // Thread-local RNG so there is no locking overhead
    thread_local std::mt19937_64 rng{ std::random_device{}() };
    thread_local std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);

    // Set version bits (version 4)
    hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    // Set variant bits (variant 1, RFC 4122)
    lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

    // Format as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<uint32_t>(hi >> 32),
        static_cast<uint16_t>(hi >> 16),
        static_cast<uint16_t>(hi),
        static_cast<uint16_t>(lo >> 48),
        static_cast<unsigned long long>(lo & 0x0000FFFFFFFFFFFFull));
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ISO 8601 UTC timestamp helper
// ─────────────────────────────────────────────────────────────────────────────
static std::string utc_now_iso8601() {
    auto now   = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val{};
#if defined(_MSC_VER) || defined(IDHMFIS_WINDOWS)
    gmtime_s(&tm_val, &tt);
#else
    gmtime_r(&tt, &tm_val);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_val);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Schema migration
// ─────────────────────────────────────────────────────────────────────────────
bool Project::needs_migration(const std::string& schema_ver) {
    // Current version is "1.0.0". Anything older (or empty) triggers migration.
    // Simple lexicographic comparison works here because versions are padded to
    // same length per field. For now there is only one version, so anything
    // that is not exactly "1.0.0" falls through the migration path.
    return schema_ver != kCurrentSchemaVersion;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Migration dispatcher — called from from_json after initial parse.
//  Add new cases here as the schema evolves.
// ─────────────────────────────────────────────────────────────────────────────
static void migrate_project(Project& p, const std::string& from_ver) {
    // 2.8.0 → 2.9.0: added audio_track in TimelineDef and ui_layout in Project.
    // Fields are optional in from_json so old files load cleanly with defaults.
    (void)p;
    (void)from_ver;

    // After all migrations, stamp the new version.
    p.schema_version = kCurrentSchemaVersion;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Serialisation
// ─────────────────────────────────────────────────────────────────────────────
nlohmann::json Project::to_json() const {
    nlohmann::json j;
    idhmfis::to_json(j, *this);
    return j;
}

Project Project::from_json(const nlohmann::json& j) {
    Project p;
    idhmfis::from_json(j, p);

    if (needs_migration(p.schema_version)) {
        std::string from_ver = p.schema_version;
        migrate_project(p, from_ver);
    }

    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Atomic save
//  Writes to <path>.tmp, then renames to <path>. On most OS / file systems
//  rename() is atomic with respect to power-loss, so a partial save never
//  leaves a corrupt .idhmfis file.
// ─────────────────────────────────────────────────────────────────────────────
bool Project::save(const std::string& path) const {
    // Stamp modified_at before serialising
    Project copy = *this;
    copy.modified_at = utc_now_iso8601();
    if (copy.created_at.empty())
        copy.created_at = copy.modified_at;

    nlohmann::json j;
    idhmfis::to_json(j, copy);

    std::string tmp_path = path + ".tmp";

    {
        std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
        if (!ofs.is_open())
            throw std::runtime_error("Cannot open for writing: " + tmp_path);
        ofs << j.dump(2);   // 2-space pretty print
        if (!ofs.good())
            throw std::runtime_error("Write error: " + tmp_path);
    }

    // Atomic rename
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        // Fallback: copy then remove (e.g. cross-device)
        std::filesystem::copy_file(tmp_path, path,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            throw std::runtime_error("Cannot rename/copy save file: " + ec.message());
        std::filesystem::remove(tmp_path, ec);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Load
// ─────────────────────────────────────────────────────────────────────────────
Project Project::load(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open())
        throw ProjectLoadError("File not found: " + path,
                               ProjectLoadError::Reason::FileNotFound);

    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const nlohmann::json::parse_error& e) {
        throw ProjectLoadError(
            std::string("JSON parse error in '") + path + "': " + e.what(),
            ProjectLoadError::Reason::ParseError);
    }

    if (!j.is_object())
        throw ProjectLoadError("Root JSON element is not an object: " + path,
                               ProjectLoadError::Reason::InvalidData);

    // Schema check — warn but continue with migration rather than hard-fail
    std::string file_schema = j.value("schema_version", std::string{});
    if (file_schema.empty())
        throw ProjectLoadError(
            "Missing schema_version in '" + path + "'. File may be corrupt.",
            ProjectLoadError::Reason::SchemaMismatch);

    try {
        return Project::from_json(j);
    } catch (const nlohmann::json::exception& e) {
        throw ProjectLoadError(
            std::string("Failed to deserialise '") + path + "': " + e.what(),
            ProjectLoadError::Reason::MissingField);
    }
}

} // namespace idhmfis
