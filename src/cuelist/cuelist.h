#pragma once
// cuelist.h — idhmfis_cuelist: standalone MagicQ-class cue list library (spec §B7 + §B8)
// No UI, no engine, no platform dependencies beyond the C++20 STL.

#include "cuelist_types.h"
#include <vector>
#include <string>
#include <functional>
#include <optional>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  PlaybackState — the cue list's top-level state machine node
// ─────────────────────────────────────────────────────────────────────────────
enum class PlaybackState {
    Idle,       // no cue active; list has stopped or never started
    Playing,    // a cue is active and advancing
    Paused,     // time is frozen; fade level held
    Releasing,  // fade_level_ decaying to zero after stop() call
    Holding     // inside the hold phase of the current cue
};

// ─────────────────────────────────────────────────────────────────────────────
//  PlaybackEvent — what just happened (delivered to callers via tick())
// ─────────────────────────────────────────────────────────────────────────────
enum class PlaybackEvent {
    CueActivated,       // a new cue became current
    CueReleased,        // a cue was deactivated without a successor
    PlaybackStopped,    // state transitioned to Idle
    TriggerFired,       // a Follow/Wait trigger was evaluated and fired
    HoldStarted,        // cue entered its hold phase
    HoldEnded,          // hold phase concluded; about to advance
    LoopedBack,         // link == Loop; wrapped to entry 0
    JumpedTo,           // link == Jump or jump() API call
};

struct PlaybackEventData {
    PlaybackEvent event;
    int           entry_index = -1;   // which entry produced this event (-1 = none)
    CueNumber     cue_number;
    double        time_s      = 0.0;  // wall-clock accumulator at event time
};

// ─────────────────────────────────────────────────────────────────────────────
//  CueList — MagicQ-class show data structure and playback state machine (§B7)
// ─────────────────────────────────────────────────────────────────────────────
class CueList {
public:
    CueList() = default;

    // ── Edit API (intended for use from UI / undo-redo system) ───────────────

    void add_entry(FullCueEntry entry);
    void remove_entry(int index);
    void move_entry(int from, int to);
    void update_entry(int index, FullCueEntry entry);

    const FullCueEntry& entry_at(int index) const;
    FullCueEntry&       entry_at(int index);
    // Returns nullptr if index is out of range (safe variant for engine use).
    const FullCueEntry* entry_at_safe(int index) const;
    int                 entry_count() const;

    // Returns the index of the entry whose CueNumber matches n, or -1.
    int  find_by_number(CueNumber n) const;

    // ── Playback controls ────────────────────────────────────────────────────

    // GO: Idle → activate entry 0; Playing → advance; Paused → resume.
    void go();

    // BACK: activate the previous entry; rebuilds tracking from the nearest
    //       BLOCK cue or the list start.
    void back();

    void pause();
    void resume();

    // Begin a graceful release: fade_level_ decays to 0 over release_time_s,
    // then state transitions to Idle.
    void stop(float release_time_s = 0.5f);

    // Jump directly to the entry matching target; rebuilds tracking.
    void jump(CueNumber target);

    // Re-fire (reassert) the current cue — resets cue_time_ to 0 and
    // rebuilds the tracking snapshot.
    void reassert();

    // Non-destructive momentary GO: activates the given entry while the
    // current cue continues underneath (flash-style).
    void flash(int index);

    // Exclusive flash: like flash() but releases all other active entries
    // first (swop-style). In this pure data model "release" means the tracked
    // params revert to the state before flash().
    void swop(int index);

    // ── Tick (1000 Hz engine call) ───────────────────────────────────────────
    // dt: elapsed seconds since the last call.
    // Returns all events that occurred during this tick.
    std::vector<PlaybackEventData> tick(double dt);

    // ── Read-only state ──────────────────────────────────────────────────────
    PlaybackState state()          const { return state_; }
    int           current_idx()    const { return current_idx_; }
    int           next_idx()       const;
    double        cue_time()       const { return cue_time_; }
    double        fade_level()     const { return fade_level_; }
    CueNumber     current_number() const;

    // Returns the LTP-merged parameter map for the current playback position.
    // Applies all tracked overrides from the nearest BLOCK cue up to current.
    // Returns an empty vector when no cue is active.
    std::vector<std::pair<std::string, float>> resolved_params() const;

    // ── Direct access for serialisation ─────────────────────────────────────
    std::vector<FullCueEntry>&       entries()       { return entries_; }
    const std::vector<FullCueEntry>& entries() const { return entries_; }

private:
    std::vector<FullCueEntry> entries_;

    PlaybackState state_        = PlaybackState::Idle;
    int           current_idx_  = -1;
    double        cue_time_     = 0.0;   // seconds within the current cue
    double        fade_level_   = 0.0;   // 0..1 fade level
    double        hold_elapsed_ = 0.0;   // seconds spent in hold phase
    bool          in_hold_      = false;
    float         release_time_ = 0.5f;  // seconds for the release decay

    // Flash / swop bookkeeping
    int           flash_idx_    = -1;    // entry currently being flashed (-1 = none)
    bool          swop_active_  = false;

    // Events generated by go()/back()/jump() between ticks — flushed by next tick()
    std::vector<PlaybackEventData> pending_events_;

    // LTP tracking snapshot: accumulated param_overrides from BLOCK to current
    std::vector<std::pair<std::string, float>> tracked_params_;

    // ── Private helpers ──────────────────────────────────────────────────────

    // Activate the entry at index, optionally rebuilding the tracking snapshot.
    void activate_entry(int index, bool rebuild_tracking,
                        std::vector<PlaybackEventData>& out_events);

    // Rebuild tracked_params_ by walking from start (or the nearest BLOCK
    // cue before up_to_index) up to (and including) up_to_index.
    void rebuild_tracking_from(int up_to_index);

    // Return the index of the entry after from (-1 if none).
    int  find_next_from(int from) const;

    // Return the index of the entry before from (-1 if none).
    int  find_prev_from(int from) const;

    // Compute the 0..1 fade level for a cue that has been running for t
    // seconds, given its timing block.
    double compute_fade_level(double t, const CueTimingBlock& timing) const;

    // Apply the PathInterp curve to a linear alpha in [0, 1].
    double apply_curve(double alpha, PathInterp interp) const;

    // Handle LinkMode when a cue completes naturally (trigger fires).
    // Writes events and transitions state.
    void handle_link(std::vector<PlaybackEventData>& out_events);
};

} // namespace idhmfis
