#pragma once
// auto_save.h — Periodic project auto-save with crash recovery support.
//
// Usage:
//   auto_save.notify_dirty();       // call whenever project changes
//   auto_save.tick(project, path);  // call each UI frame; saves when due
//   auto_save.recover_path();       // on startup, non-empty → offer restore
//
// Auto-save files are written alongside the project file with suffix ".autosave".
// A sentinel file ".autosave_lock" marks an unclean shutdown; if present on
// startup, recover_path() returns the stale auto-save path for restore prompt.

#include <chrono>
#include <string>

namespace idhmfis {

struct Project;

class AutoSave {
public:
    // interval_s: seconds between auto-saves (default 120 = 2 minutes)
    explicit AutoSave(int interval_s = 120);

    // Call whenever the project is modified
    void notify_dirty();

    // Call once per UI frame with the current project and base save path.
    // base_path may be empty (unsaved project) — uses temp dir in that case.
    // Returns true if an auto-save was performed this call.
    bool tick(const Project& project, const std::string& base_path);

    // Returns the last auto-save file written (empty if none)
    const std::string& last_autosave_path() const { return last_path_; }

    // Returns non-empty path if a crash-recovery file exists from a prior run.
    // Call once on startup; clear with clear_recover().
    static std::string recover_path(const std::string& base_path);
    static void        clear_recover(const std::string& base_path);

    // Called on clean shutdown to remove the lock sentinel
    void on_clean_shutdown(const std::string& base_path);

    // True if a save is currently due (project dirty and interval elapsed)
    bool is_due() const;

    void set_interval(int interval_s) { interval_s_ = std::max(5, interval_s); }
    void set_enabled(bool enabled)    { enabled_ = enabled; }
    bool enabled() const              { return enabled_; }

private:
    int                                         interval_s_;
    bool                                        enabled_     = true;
    bool                                        dirty_       = false;
    std::chrono::steady_clock::time_point       last_save_;
    std::string                                 last_path_;
};

} // namespace idhmfis
