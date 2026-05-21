#pragma once
// timeline_engine.h — 1000 Hz SMPTE-chase timeline engine.
// Called once per engine tick; drives timeline state machines and fires events.

#include "timeline_types.h"
#include "../input/timecode.h"
#include <string>
#include <vector>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  TimelineEngine
// ─────────────────────────────────────────────────────────────────────────────
class TimelineEngine {
public:
    TimelineEngine() = default;

    // ── Event output ─────────────────────────────────────────────────────────
    struct FiredEvent {
        std::string   timeline_id;
        TimelineEvent event;
    };

    // Called once per engine tick. Returns all events that fired this tick.
    // tc = current external timecode from the router.
    // dt_s = tick delta in seconds (for internal-clock timelines).
    std::vector<FiredEvent> tick(const TimecodeState& tc, double dt_s);

    // ── Transport controls (internal-clock timelines) ─────────────────────────
    void play   (const std::string& id);
    void pause  (const std::string& id);
    void stop   (const std::string& id);
    void rewind (const std::string& id);
    void seek   (const std::string& id, int64_t frame);
    void set_armed(const std::string& id, bool armed);

    // ── CRUD ─────────────────────────────────────────────────────────────────
    void create     (TimelineDef def);
    void remove     (const std::string& id);
    void update_def (TimelineDef def);   // full replace

    // ── Track helpers (called from engine command handlers) ───────────────────
    void add_track    (const std::string& timeline_id, const std::string& name);
    void remove_track (const std::string& timeline_id, int track_id);
    void update_track (const std::string& timeline_id, int track_id,
                       const std::string& name, bool muted, bool locked, bool collapsed);

    // ── Event helpers ─────────────────────────────────────────────────────────
    void add_event    (const std::string& timeline_id, int track_id, TimelineEvent ev);
    void remove_event (const std::string& timeline_id, int track_id, int64_t event_id);
    void update_event (const std::string& timeline_id, int track_id, TimelineEvent ev);

    // ── WaitForGo advance ─────────────────────────────────────────────────────
    void wait_for_go_advance(const std::string& id);
    void set_wait_for_go    (const std::string& id);

    // ── TC config (jump-detect thresholds per slot) ───────────────────────────
    void set_tc_config(const ProjectTimecodeConfig& cfg) { tc_config_ = cfg; }

    // ── Accessors ─────────────────────────────────────────────────────────────
    const std::vector<TimelineDef>& defs() const { return defs_; }

    TimecodeSourceStatus source_status(const TimecodeState& tc,
                                       const std::string&   slot) const;

    // ── Runtime snapshot per timeline ─────────────────────────────────────────
    struct RuntimeSnap {
        std::string          id;
        TimelineState        state;
        int64_t              position_frames;
        TimecodeSourceStatus source_status;
    };
    std::vector<RuntimeSnap> runtime_snaps() const;

private:
    // Per-timeline runtime state (engine-thread only)
    struct Runtime {
        TimelineState state          = TimelineState::Idle;
        int64_t       pos            = 0;
        int64_t       last_event_pos = -1;
        double        internal_clock = 0.0;   // accumulated seconds (internal mode)
        bool          wait_for_go    = false;
        int           next_track_id  = 1;     // monotonic track ID counter
        int64_t       next_event_id  = 1;     // monotonic event ID counter
    };

    std::vector<TimelineDef> defs_;
    std::vector<Runtime>     runtimes_;
    ProjectTimecodeConfig    tc_config_;

    Runtime*        find_runtime    (const std::string& id);
    const Runtime*  find_runtime    (const std::string& id) const;
    TimelineDef*    find_def_mut    (const std::string& id);
    const TimelineDef* find_def     (const std::string& id) const;
    TimelineTrack*  find_track      (TimelineDef& def, int track_id);

    // Fire all events in (last_pos, new_pos] for a given runtime.
    void collect_events(const TimelineDef& def, Runtime& rt,
                        int64_t new_pos, std::vector<FiredEvent>& out);
};

} // namespace idhmfis
