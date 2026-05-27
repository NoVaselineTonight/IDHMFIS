// auto_save.cpp — Periodic project auto-save implementation.

#include "auto_save.h"
#include "serialization.h"
#include "project.h"
#include "../core/logger.h"

#include <cstring>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

#if defined(_WIN32) || defined(IDHMFIS_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace idhmfis {

// ─── helpers ──────────────────────────────────────────────────────────────────

// BUG #53: Store the current process creation time so the orphan scanner
// can distinguish a recycled PID from a genuine surviving IDHMFIS process.
#if defined(_WIN32) || defined(IDHMFIS_WINDOWS)
static FILETIME get_current_process_creation_time() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        std::memset(&creation, 0, sizeof(creation));
    return creation;
}
// Lazily initialised once at first call (thread-safe under C++11 rules).
static FILETIME s_own_creation_time = get_current_process_creation_time();
#endif

static std::string autosave_path(const std::string& base_path) {
    if (base_path.empty()) {
#if defined(_WIN32) || defined(IDHMFIS_WINDOWS)
        auto pid = std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));
#else
        auto pid = std::to_string(static_cast<unsigned long>(getpid()));
#endif
        auto tmp = fs::temp_directory_path() / ("idhmfis_autosave_" + pid + ".idhmfis");
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

std::vector<std::string> AutoSave::scan_orphaned_autosaves() {
    std::vector<std::string> candidates;
    std::error_code ec;
    auto tmp_dir = fs::temp_directory_path();

    for (auto& entry : fs::directory_iterator(tmp_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        auto fname = entry.path().filename().string();
        static constexpr std::string_view kPrefix = "idhmfis_autosave_";
        if (fname.size() <= kPrefix.size()) continue;
        if (fname.substr(0, kPrefix.size()) != kPrefix) continue;
        if (entry.path().extension() != ".idhmfis") continue;

        auto stem    = entry.path().stem().string();
        auto pid_str = stem.substr(kPrefix.size());
        if (pid_str.empty()) continue;

        bool process_running = false;
        try {
            auto pid_val = std::stoul(pid_str);
#if defined(_WIN32) || defined(IDHMFIS_WINDOWS)
            // PROCESS_QUERY_LIMITED_INFORMATION is required for GetExitCodeProcess.
            // SYNCHRONIZE alone is insufficient (BUG #23).
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   static_cast<DWORD>(pid_val));
            if (h != nullptr) {
                DWORD exit_code = 0;
                bool alive = (GetExitCodeProcess(h, &exit_code) &&
                              exit_code == STILL_ACTIVE);
                if (alive) {
                    // BUG #53: guard against PID reuse by comparing process
                    // creation times.  If the creation time matches our own
                    // process, this *is* the current session — not an orphan.
                    FILETIME ct{}, ex{}, kn{}, us{};
                    if (GetProcessTimes(h, &ct, &ex, &kn, &us)) {
                        bool same_session = (ct.dwLowDateTime  == s_own_creation_time.dwLowDateTime &&
                                             ct.dwHighDateTime == s_own_creation_time.dwHighDateTime);
                        // If same creation time, this is our own auto-save — skip.
                        // If different creation time, a different process reused the PID —
                        // the original process is gone, so this IS an orphan.
                        if (!same_session)
                            alive = false; // PID reused — treat as orphan
                    }
                }
                process_running = alive;
                CloseHandle(h);
            }
#else
            process_running = (kill(static_cast<pid_t>(pid_val), 0) == 0);
#endif
        } catch (...) {
            continue;
        }

        if (!process_running) {
            std::error_code fec;
            if (entry.file_size(fec) > 0)
                candidates.push_back(entry.path().string());
        }
    }
    return candidates;
}

} // namespace idhmfis
