// auto_save.cpp — Periodic project auto-save implementation.

#include "auto_save.h"
#include "serialization.h"
#include "project.h"
#include "../core/logger.h"

#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace idhmfis {

// ─── helpers ──────────────────────────────────────────────────────────────────

static std::string autosave_path(const std::string& base_path) {
    if (base_path.empty()) {
        // Unsaved project: use temp directory
        auto tmp = fs::temp_directory_path() / "idhmfis_autosave.idh";
        return tmp.string();
    }
    return base_path + ".autosave";
}

static std::string lock_path(const std::string& base_path) {
    return autosave_path(base_path) + ".lock";
}

static bool write_file(const std::string& path, const std::string& data) {
    // Ensure parent directory exists
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        log::error("AutoSave: cannot create directory for \"%s\": %s",
                   path.c_str(), ec.message().c_str());
        return false;
    }

    // Write to a temp file then rename atomically
    std::string tmp_path = path + ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            log::error("AutoSave: cannot open temp file for writing: \"%s\"", tmp_path.c_str());
            return false;
        }
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!ofs.good()) {
            log::error("AutoSave: write error on temp file: \"%s\"", tmp_path.c_str());
            return false;
        }
    }
    fs::rename(tmp_path, path, ec);
    if (ec) {
        // Fallback: copy then remove (e.g. cross-device)
        fs::copy_file(tmp_path, path, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            log::error("AutoSave: cannot rename/copy \"%s\" to \"%s\": %s",
                       tmp_path.c_str(), path.c_str(), ec.message().c_str());
            return false;
        }
        fs::remove(tmp_path, ec);
    }
    return true;
}

// ─── AutoSave ─────────────────────────────────────────────────────────────────

AutoSave::AutoSave(int interval_s)
    : interval_s_(interval_s)
    , last_save_(std::chrono::steady_clock::now())
{}

void AutoSave::notify_dirty() {
    dirty_ = true;
}

bool AutoSave::is_due() const {
    if (!enabled_ || !dirty_) return false;
    auto elapsed = std::chrono::steady_clock::now() - last_save_;
    return std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()
           >= interval_s_;
}

bool AutoSave::tick(const Project& project, const std::string& base_path) {
    if (!is_due()) return false;

    std::string path = autosave_path(base_path);

    // Write a lock sentinel BEFORE the save — marks unclean if process dies mid-write
    std::string lpath = lock_path(base_path);
    {
        std::ofstream lock(lpath);
        (void)lock;
    }

    try {
        nlohmann::json j;
        idhmfis::to_json(j, project);
        std::string json = j.dump(2);
        if (!write_file(path, json)) {
            log::warn("AutoSave: failed to write %s", path.c_str());
            return false;
        }
    } catch (const std::exception& e) {
        log::warn("AutoSave: exception: %s", e.what());
        return false;
    }

    // Remove lock sentinel on success
    std::error_code ec;
    fs::remove(lpath, ec);

    last_save_ = std::chrono::steady_clock::now();
    dirty_ = false;
    last_path_ = path;
    log::info("AutoSave: saved to %s", path.c_str());
    return true;
}

std::string AutoSave::recover_path(const std::string& base_path) {
    std::string apath = autosave_path(base_path);
    std::string lpath = lock_path(base_path);
    // Lock file present → unclean shutdown; auto-save may be valid
    if (fs::exists(lpath) && fs::exists(apath))
        return apath;
    // Auto-save present even without lock → still offer (less urgent)
    if (fs::exists(apath))
        return apath;
    return {};
}

void AutoSave::clear_recover(const std::string& base_path) {
    std::error_code ec;
    fs::remove(autosave_path(base_path), ec);
    fs::remove(lock_path(base_path), ec);
}

void AutoSave::on_clean_shutdown(const std::string& base_path) {
    std::error_code ec;
    fs::remove(lock_path(base_path), ec);
    fs::remove(autosave_path(base_path), ec);
}

} // namespace idhmfis
