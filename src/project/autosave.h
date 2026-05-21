#pragma once
// autosave.h — Background autosave thread with recovery detection.
//
// Usage:
//   AutosaveManager mgr;
//   mgr.start(project_ref, interval_seconds);
//   // ... run the application ...
//   mgr.stop();
//
//   // On startup, check for a stale save:
//   if (AutosaveManager::has_recovery_file(project_name)) {
//       if (user_confirms_recovery())
//           project = AutosaveManager::load_recovery(project_name);
//   }
//
// Thread safety:
//   The autosave thread calls a user-provided snapshot callback to obtain a
//   copy of the project (the callback must hold any necessary lock). The
//   AutosaveManager itself is not re-entrant.

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <vector>
#include <filesystem>

#include "project.h"

namespace idhmfis {

class AutosaveManager {
public:
    // Callback type: must return a current copy of the project.
    // Called from the autosave thread; the caller must ensure thread safety.
    using SnapshotFn = std::function<Project()>;

    static constexpr int kDefaultIntervalSecs  = 60;
    static constexpr int kMaxRotationCount     = 3;

    AutosaveManager() = default;
    ~AutosaveManager();

    // Non-copyable, non-movable (owns a thread)
    AutosaveManager(const AutosaveManager&) = delete;
    AutosaveManager& operator=(const AutosaveManager&) = delete;

    // Start autosave thread.
    // snapshot_fn: called periodically to get a project snapshot.
    // project_name: used to build the save file path.
    // interval_secs: how often to save (default 60 s).
    void start(SnapshotFn snapshot_fn,
               const std::string& project_name,
               int interval_secs = kDefaultIntervalSecs);

    // Stop the autosave thread and wait for it to finish.
    void stop();

    // Force an immediate save (called e.g. before a risky operation).
    void save_now();

    // Change the project name (updates the target path; next save uses it).
    void set_project_name(const std::string& name);

    // ------------------------------------------------------------------
    // Static recovery API
    // ------------------------------------------------------------------

    // Returns the autosave directory path (%APPDATA%\IDHMFIS\autosave on Win,
    // ~/.local/share/IDHMFIS/autosave on Linux,
    // ~/Library/Application Support/IDHMFIS/autosave on macOS).
    static std::filesystem::path autosave_dir();

    // Returns true if a recovery file exists for project_name.
    static bool has_recovery_file(const std::string& project_name);

    // Returns the path to the most-recent recovery file for project_name.
    // Returns empty path if none exists.
    static std::filesystem::path latest_recovery_path(const std::string& project_name);

    // Load the most-recent recovery file. Throws on error.
    static Project load_recovery(const std::string& project_name);

    // Delete all recovery files for project_name.
    static void clear_recovery(const std::string& project_name);

    // List all recovery files for project_name, newest first.
    static std::vector<std::filesystem::path> list_recoveries(const std::string& project_name);

    // Build the canonical recovery file name stem for project_name.
    static std::string recovery_stem(const std::string& project_name);

private:
    // Worker thread function
    void run(int interval_secs);

    // Perform one save cycle: get snapshot, write rotated file.
    void do_save();

    // Rotate: keep only kMaxRotationCount most-recent files.
    void rotate_files();

    SnapshotFn         snapshot_fn_;
    std::string        project_name_;
    std::atomic<bool>  running_{ false };
    std::thread        thread_;
    std::mutex         mutex_;             // protects project_name_ and snapshot_fn_

    // Condition variable so stop() wakes the sleeping thread immediately
    std::condition_variable cv_;
    std::mutex              cv_mutex_;
    bool                    wake_flag_{ false };
};

} // namespace idhmfis
