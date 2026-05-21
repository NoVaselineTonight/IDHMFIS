// autosave.cpp — AutosaveManager implementation.
//
// Recovery files are plain JSON with .idhmfis extension, named:
//   <project_name>_autosave_<timestamp>.idhmfis
//
// Compression: currently raw JSON. A miniz/zlib wrapper can be added later
// without changing the public API — just wrap the write/read calls below.
//
// Platform paths:
//   Windows : %APPDATA%\IDHMFIS\autosave\
//   macOS   : ~/Library/Application Support/IDHMFIS/autosave/
//   Linux   : ~/.local/share/IDHMFIS/autosave/

#include "autosave.h"
#include "serialization.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>

#if defined(_WIN32) || defined(IDHMFIS_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shlobj.h>
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Platform-specific data directory
// ─────────────────────────────────────────────────────────────────────────────
static std::filesystem::path app_data_dir() {
#if defined(_WIN32) || defined(IDHMFIS_WINDOWS)
    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf)))
        return std::filesystem::path(buf) / "IDHMFIS";
    // Fallback
    const char* appdata = std::getenv("APPDATA");
    if (appdata) return std::filesystem::path(appdata) / "IDHMFIS";
    return std::filesystem::temp_directory_path() / "IDHMFIS";
#elif defined(__APPLE__) || defined(IDHMFIS_MACOS)
    const char* home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / "Library" / "Application Support" / "IDHMFIS";
    return std::filesystem::temp_directory_path() / "IDHMFIS";
#else
    // Linux / other POSIX
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return std::filesystem::path(xdg) / "IDHMFIS";
    const char* home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / ".local" / "share" / "IDHMFIS";
    return std::filesystem::temp_directory_path() / "IDHMFIS";
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sanitise project name for use in file names
// ─────────────────────────────────────────────────────────────────────────────
static std::string sanitise_name(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            out += c;
        else
            out += '_';
    }
    if (out.empty()) out = "untitled";
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Timestamp string for file names
// ─────────────────────────────────────────────────────────────────────────────
static std::string timestamp_str() {
    auto now   = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val{};
#if defined(_MSC_VER) || defined(_WIN32) || defined(IDHMFIS_WINDOWS)
    gmtime_s(&tm_val, &tt);
#else
    gmtime_r(&tt, &tm_val);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm_val);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static API
// ─────────────────────────────────────────────────────────────────────────────
std::filesystem::path AutosaveManager::autosave_dir() {
    return app_data_dir() / "autosave";
}

std::string AutosaveManager::recovery_stem(const std::string& project_name) {
    return sanitise_name(project_name) + "_autosave";
}

std::vector<std::filesystem::path>
AutosaveManager::list_recoveries(const std::string& project_name) {
    auto dir  = autosave_dir();
    auto stem = recovery_stem(project_name);

    std::vector<std::filesystem::path> results;

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return results;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() != ".idhmfis") continue;
        const std::string fn = p.stem().string();
        if (fn.rfind(stem, 0) == 0) // starts with stem
            results.push_back(p);
    }

    // Sort newest first (lexicographic on timestamp-encoded name is correct)
    std::sort(results.begin(), results.end(),
        [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return a.filename() > b.filename();
        });

    return results;
}

bool AutosaveManager::has_recovery_file(const std::string& project_name) {
    return !list_recoveries(project_name).empty();
}

std::filesystem::path
AutosaveManager::latest_recovery_path(const std::string& project_name) {
    auto list = list_recoveries(project_name);
    if (list.empty()) return {};
    return list.front();
}

Project AutosaveManager::load_recovery(const std::string& project_name) {
    auto path = latest_recovery_path(project_name);
    if (path.empty())
        throw std::runtime_error("No recovery file found for: " + project_name);
    return Project::load(path.string());
}

void AutosaveManager::clear_recovery(const std::string& project_name) {
    for (const auto& p : list_recoveries(project_name)) {
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Instance: start / stop
// ─────────────────────────────────────────────────────────────────────────────
AutosaveManager::~AutosaveManager() {
    stop();
}

void AutosaveManager::start(SnapshotFn snapshot_fn,
                             const std::string& project_name,
                             int interval_secs) {
    stop(); // Stop any existing thread

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_fn_   = std::move(snapshot_fn);
        project_name_  = project_name;
        wake_flag_     = false;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&AutosaveManager::run, this, interval_secs);
}

void AutosaveManager::stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(cv_mutex_);
        wake_flag_ = true;
    }
    cv_.notify_all();

    if (thread_.joinable())
        thread_.join();
}

void AutosaveManager::save_now() {
    {
        std::lock_guard<std::mutex> lk(cv_mutex_);
        wake_flag_ = true;
    }
    cv_.notify_all();
}

void AutosaveManager::set_project_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    project_name_ = name;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Worker thread
// ─────────────────────────────────────────────────────────────────────────────
void AutosaveManager::run(int interval_secs) {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk,
                std::chrono::seconds(interval_secs),
                [this] { return wake_flag_ || !running_.load(std::memory_order_acquire); });
            wake_flag_ = false;
        }

        if (!running_.load(std::memory_order_acquire)) break;

        try {
            do_save();
        } catch (const std::exception& ex) {
            // Swallow autosave errors — we must not crash the app
            (void)ex;
        }
    }
}

void AutosaveManager::do_save() {
    SnapshotFn fn;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_fn_) return;
        fn   = snapshot_fn_;
        name = project_name_;
    }

    Project snapshot = fn();
    if (snapshot.name.empty() && name.empty()) return;
    if (snapshot.name.empty()) snapshot.name = name;

    // Ensure directory exists
    auto dir = autosave_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return;

    std::string stem    = recovery_stem(name.empty() ? snapshot.name : name);
    std::string ts      = timestamp_str();
    std::filesystem::path save_path = dir / (stem + "_" + ts + ".idhmfis");

    snapshot.save(save_path.string());

    rotate_files();
}

void AutosaveManager::rotate_files() {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        name = project_name_;
    }

    auto files = list_recoveries(name);
    // files is sorted newest first; delete everything past index kMaxRotationCount-1
    for (size_t i = static_cast<size_t>(kMaxRotationCount); i < files.size(); ++i) {
        std::error_code ec;
        std::filesystem::remove(files[i], ec);
    }
}

} // namespace idhmfis
