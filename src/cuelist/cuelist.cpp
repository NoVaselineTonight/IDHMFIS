// cuelist.cpp — CueList playback state machine implementation (spec §B7 + §B8)
// No UI, no engine, no platform dependencies beyond the C++20 STL.

#include "cuelist.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Edit API
// ─────────────────────────────────────────────────────────────────────────────

void CueList::add_entry(FullCueEntry entry)
{
    // BUG #28 fix: reject entries whose CueNumber already exists in the list.
    // Duplicate numbers cause find_by_number() to return the wrong index, silently
    // corrupting the active cue pointer after any sort or index-based lookup.
    for (const auto& e : entries_) {
        if (e.number == entry.number) {
            // Duplicate detected — reject silently. Caller should assign a
            // unique CueNumber before calling add_entry().
            return;
        }
    }

    // H-8: stable_sort can reorder entries, invalidating current_idx_.
    // Save the active entry's CueNumber before the sort so we can re-find it.
    CueNumber saved_number{};
    const bool had_current = (current_idx_ >= 0 &&
                              current_idx_ < static_cast<int>(entries_.size()));
    if (had_current)
        saved_number = entries_[static_cast<std::size_t>(current_idx_)].number;

    entries_.push_back(std::move(entry));
    // Keep entries sorted by CueNumber so binary searches stay valid.
    std::stable_sort(entries_.begin(), entries_.end(),
        [](const FullCueEntry& a, const FullCueEntry& b) {
            return a.number < b.number;
        });

    // Restore current_idx_ to the (possibly moved) active entry.
    if (had_current)
        current_idx_ = find_by_number(saved_number);
}

void CueList::remove_entry(int index)
{
    if (index < 0 || index >= static_cast<int>(entries_.size()))
        return;
    entries_.erase(entries_.begin() + index);

    // Fix up current_idx_ after removal.
    if (current_idx_ == index) {
        current_idx_ = -1;
        state_       = PlaybackState::Idle;
        fade_level_  = 0.0;
        cue_time_    = 0.0;
        in_hold_     = false;
    } else if (current_idx_ > index) {
        --current_idx_;
    }
}

void CueList::move_entry(int from, int to)
{
    const int n = static_cast<int>(entries_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;

    FullCueEntry entry = std::move(entries_[from]);
    entries_.erase(entries_.begin() + from);
    const int insert_at = (to > from) ? (to - 1) : to;
    entries_.insert(entries_.begin() + insert_at, std::move(entry));

    // Fix up current_idx_.
    if (current_idx_ == from) {
        current_idx_ = insert_at;
    } else {
        if (from < to) {
            if (current_idx_ > from && current_idx_ <= insert_at)
                --current_idx_;
        } else {
            if (current_idx_ >= to && current_idx_ < from)
                ++current_idx_;
        }
    }
}

void CueList::update_entry(int index, FullCueEntry entry)
{
    if (index < 0 || index >= static_cast<int>(entries_.size()))
        return;

    // BUG #28 fix: reject if the new number collides with another existing entry.
    // (It is allowed for the entry to keep its own current number.)
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (i != index && entries_[static_cast<std::size_t>(i)].number == entry.number) {
            // Duplicate number on a different entry — reject update.
            return;
        }
    }

    // H-9: stable_sort can reorder entries, invalidating current_idx_.
    // If we are updating the active entry, track its *new* number (it may change).
    const bool had_current = (current_idx_ >= 0 &&
                              current_idx_ < static_cast<int>(entries_.size()));
    CueNumber saved_number{};
    if (had_current) {
        saved_number = (current_idx_ == index)
                     ? entry.number  // the updated entry may carry a new number
                     : entries_[static_cast<std::size_t>(current_idx_)].number;
    }

    entries_[index] = std::move(entry);
    // Re-sort because the CueNumber may have changed.
    std::stable_sort(entries_.begin(), entries_.end(),
        [](const FullCueEntry& a, const FullCueEntry& b) {
            return a.number < b.number;
        });

    // Restore current_idx_ to the (possibly moved) active entry.
    if (had_current)
        current_idx_ = find_by_number(saved_number);
}

const FullCueEntry& CueList::entry_at(int index) const
{
    // BUG #29 fix: negative index would wrap to a huge size_t, bypassing at()'s
    // bounds check and causing undefined behaviour. Catch it with a signed check first.
    if (index < 0 || static_cast<std::size_t>(index) >= entries_.size())
        throw std::out_of_range("CueList::entry_at: index out of range");
    return entries_[static_cast<std::size_t>(index)];
}

FullCueEntry& CueList::entry_at(int index)
{
    // BUG #29 fix: same guard as const overload.
    if (index < 0 || static_cast<std::size_t>(index) >= entries_.size())
        throw std::out_of_range("CueList::entry_at: index out of range");
    return entries_[static_cast<std::size_t>(index)];
}

const FullCueEntry* CueList::entry_at_safe(int index) const
{
    if (index < 0 || index >= static_cast<int>(entries_.size()))
        return nullptr;
    return &entries_[static_cast<std::size_t>(index)];
}

int CueList::entry_count() const
{
    return static_cast<int>(entries_.size());
}

int CueList::find_by_number(CueNumber n) const
{
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (entries_[i].number == n)
            return i;
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Playback controls
// ─────────────────────────────────────────────────────────────────────────────

void CueList::go()
{
    if (entries_.empty())
        return;

    if (state_ == PlaybackState::Idle || state_ == PlaybackState::Releasing) {
        // Start from the top.
        activate_entry(0, true, pending_events_);
        return;
    }

    if (state_ == PlaybackState::Paused) {
        resume();
        return;
    }

    if (state_ == PlaybackState::Playing || state_ == PlaybackState::Holding) {
        // Advance to next entry.
        const int next = find_next_from(current_idx_);
        if (next < 0) {
            // End of list — stop.
            state_      = PlaybackState::Releasing;
            release_time_ = 0.5f;
            return;
        }
        activate_entry(next, true, pending_events_);
    }
}

void CueList::back()
{
    if (entries_.empty())
        return;

    const int prev = (current_idx_ >= 0) ? find_prev_from(current_idx_)
                                          : static_cast<int>(entries_.size()) - 1;
    if (prev < 0)
        return;

    activate_entry(prev, true, pending_events_);
}

void CueList::pause()
{
    if (state_ == PlaybackState::Playing || state_ == PlaybackState::Holding)
        state_ = PlaybackState::Paused;
}

void CueList::resume()
{
    if (state_ == PlaybackState::Paused) {
        state_ = in_hold_ ? PlaybackState::Holding : PlaybackState::Playing;
    }
}

void CueList::stop(float release_time_s)
{
    if (state_ == PlaybackState::Idle)
        return;
    release_time_ = (release_time_s > 0.f) ? release_time_s : 0.f;
    if (release_time_ <= 0.f) {
        fade_level_  = 0.0;
        state_       = PlaybackState::Idle;
        current_idx_ = -1;
        in_hold_     = false;
    } else {
        state_ = PlaybackState::Releasing;
    }
}

void CueList::jump(CueNumber target)
{
    const int idx = find_by_number(target);
    if (idx < 0)
        return;
    activate_entry(idx, true, pending_events_);
}

void CueList::reassert()
{
    if (current_idx_ < 0 || entries_.empty())
        return;
    activate_entry(current_idx_, true, pending_events_);
}

void CueList::flash(int index)
{
    if (index < 0 || index >= static_cast<int>(entries_.size()))
        return;
    flash_idx_   = index;
    swop_active_ = false;
    // Flash does not change the main playback position; it only sets the
    // flash_idx_ which callers can observe via resolved_params() or directly.
    // The momentary override is returned in resolved_params() when active.
}

void CueList::swop(int index)
{
    if (index < 0 || index >= static_cast<int>(entries_.size()))
        return;
    flash_idx_   = index;
    swop_active_ = true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Tick — 1000 Hz state machine update
// ─────────────────────────────────────────────────────────────────────────────

std::vector<PlaybackEventData> CueList::tick(double dt)
{
    // Flush events queued by go()/back()/jump() between ticks
    std::vector<PlaybackEventData> events = std::move(pending_events_);
    pending_events_.clear();

    switch (state_) {
    case PlaybackState::Idle:
        break;

    case PlaybackState::Releasing: {
        if (release_time_ <= 0.f) {
            fade_level_ = 0.0;
        } else {
            fade_level_ -= static_cast<double>(dt) / static_cast<double>(release_time_);
            if (fade_level_ < 0.0) fade_level_ = 0.0;
        }
        if (fade_level_ <= 0.0) {
            fade_level_  = 0.0;
            state_       = PlaybackState::Idle;
            current_idx_ = -1;
            in_hold_     = false;
            PlaybackEventData ev;
            ev.event       = PlaybackEvent::PlaybackStopped;
            ev.entry_index = -1;
            ev.time_s      = cue_time_;
            events.push_back(ev);
        }
        break;
    }

    case PlaybackState::Paused:
        // Time is frozen; nothing advances.
        break;

    case PlaybackState::Holding: {
        hold_elapsed_ += dt;

        // H-10/H-11: guard against current_idx_ going out of range (e.g. after
        // remove_entry() while in Holding state).
        if (current_idx_ < 0 || current_idx_ >= static_cast<int>(entries_.size()))
            break;

        const FullCueEntry& cur = entries_[static_cast<std::size_t>(current_idx_)];
        const float hold_dur    = cur.timing.hold;

        if (hold_dur > 0.f && hold_elapsed_ >= static_cast<double>(hold_dur)) {
            // Hold phase complete.
            in_hold_ = false;
            state_   = PlaybackState::Playing;

            PlaybackEventData ev;
            ev.event       = PlaybackEvent::HoldEnded;
            ev.entry_index = current_idx_;
            ev.cue_number  = cur.number;
            ev.time_s      = cue_time_;
            events.push_back(ev);

            // Wait trigger fires in the Playing state via post-hold time check below.
        }
        break;
    }

    case PlaybackState::Playing: {
        cue_time_ += dt;

        if (current_idx_ < 0 || current_idx_ >= static_cast<int>(entries_.size()))
            break;

        const FullCueEntry& cur = entries_[static_cast<std::size_t>(current_idx_)];
        fade_level_ = compute_fade_level(cue_time_, cur.timing);

        // Check for entry into hold phase.
        // Use a time-based threshold (>= p2 with a small epsilon) rather than
        // comparing fade_level_ to exactly 1.0 to avoid floating-point precision
        // issues where the computed level approaches but never quite reaches 1.0.
        if (!in_hold_ && cur.timing.hold > 0.f) {
            const double hold_start = static_cast<double>(cur.timing.delay_in)
                                    + static_cast<double>(cur.timing.fade_in);
            if (cue_time_ >= hold_start && fade_level_ >= 0.999) {
                in_hold_      = true;
                hold_elapsed_ = 0.0;
                state_        = PlaybackState::Holding;

                PlaybackEventData ev;
                ev.event       = PlaybackEvent::HoldStarted;
                ev.entry_index = current_idx_;
                ev.cue_number  = cur.number;
                ev.time_s      = cue_time_;
                events.push_back(ev);
                break; // continue next tick from Holding
            }
        }

        bool link_fired = false;

        // Check Follow trigger.
        // M-6: guard with trigger_fired_ latch — without it the trigger re-fires
        // every tick after time_s, calling handle_link() thousands of times per second
        // and producing a storm of CueActivated events.
        if (!trigger_fired_ && cur.trigger.type == TriggerType::Follow) {
            if (cue_time_ >= static_cast<double>(cur.trigger.time_s)) {
                trigger_fired_ = true;
                PlaybackEventData ev;
                ev.event       = PlaybackEvent::TriggerFired;
                ev.entry_index = current_idx_;
                ev.cue_number  = cur.number;
                ev.time_s      = cue_time_;
                events.push_back(ev);
                handle_link(events);
                link_fired = true;
            }
        }

        // Check Wait trigger.
        // For hold=0: fires trigger.time_s seconds from cue start.
        // For hold>0: fires trigger.time_s seconds after hold ends (cue_time_ resumes
        //             from delay_in+fade_in after hold, so check from that threshold).
        if (!trigger_fired_ && !link_fired && cur.trigger.type == TriggerType::Wait) {
            const double trigger_at = (cur.timing.hold > 0.f)
                ? (static_cast<double>(cur.timing.delay_in)
                   + static_cast<double>(cur.timing.fade_in)
                   + static_cast<double>(cur.trigger.time_s))
                : static_cast<double>(cur.trigger.time_s);
            if (!in_hold_ && cue_time_ >= trigger_at) {
                trigger_fired_ = true;
                PlaybackEventData ev;
                ev.event       = PlaybackEvent::TriggerFired;
                ev.entry_index = current_idx_;
                ev.cue_number  = cur.number;
                ev.time_s      = cue_time_;
                events.push_back(ev);
                handle_link(events);
            }
        }

        break;
    }
    } // switch

    return events;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Read state
// ─────────────────────────────────────────────────────────────────────────────

int CueList::next_idx() const
{
    return find_next_from(current_idx_);
}

CueNumber CueList::current_number() const
{
    if (current_idx_ < 0 || current_idx_ >= static_cast<int>(entries_.size()))
        return CueNumber{0, 0};
    return entries_[static_cast<std::size_t>(current_idx_)].number;
}

std::vector<std::pair<std::string, float>> CueList::resolved_params() const
{
    if (current_idx_ < 0)
        return {};

    // Start from the LTP-tracked baseline.
    std::vector<std::pair<std::string, float>> result = tracked_params_;

    // If a flash/swop is active, layer those overrides on top.
    if (flash_idx_ >= 0 && flash_idx_ < static_cast<int>(entries_.size())) {
        const FullCueEntry& flash_entry =
            entries_[static_cast<std::size_t>(flash_idx_)];
        for (const auto& override_pair : flash_entry.param_overrides) {
            const std::string& ovr_name  = override_pair.first;
            const float        ovr_value = override_pair.second;
            auto it = std::find_if(result.begin(), result.end(),
                [&ovr_name](const std::pair<std::string, float>& p) {
                    return p.first == ovr_name;
                });
            if (it != result.end())
                it->second = ovr_value;
            else
                result.emplace_back(ovr_name, ovr_value);
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private helpers
// ─────────────────────────────────────────────────────────────────────────────

void CueList::activate_entry(int index, bool rebuild_tracking,
                              std::vector<PlaybackEventData>& out_events)
{
    if (index < 0 || index >= static_cast<int>(entries_.size()))
        return;

    current_idx_   = index;
    cue_time_      = 0.0;
    fade_level_    = 0.0;
    in_hold_       = false;
    hold_elapsed_  = 0.0;
    trigger_fired_ = false;  // M-6: reset latch so Follow/Wait fires exactly once
    state_         = PlaybackState::Playing;

    if (rebuild_tracking)
        rebuild_tracking_from(index);

    const FullCueEntry& entry = entries_[static_cast<std::size_t>(index)];
    PlaybackEventData ev;
    ev.event       = PlaybackEvent::CueActivated;
    ev.entry_index = index;
    ev.cue_number  = entry.number;
    ev.time_s      = cue_time_;
    out_events.push_back(ev);
}

void CueList::rebuild_tracking_from(int up_to_index)
{
    tracked_params_.clear();

    if (up_to_index < 0)
        return;

    // Find the BLOCK cue nearest to (but not after) up_to_index.
    int start = 0;
    for (int i = up_to_index; i >= 0; --i) {
        if (entries_[static_cast<std::size_t>(i)].is_block) {
            start = i;
            break;
        }
    }

    // Walk entries from start to up_to_index (inclusive), accumulating
    // param_overrides with LTP semantics (later values overwrite earlier).
    for (int i = start; i <= up_to_index; ++i) {
        const FullCueEntry& e = entries_[static_cast<std::size_t>(i)];
        for (const auto& override_pair : e.param_overrides) {
            const std::string& ovr_name  = override_pair.first;
            const float        ovr_value = override_pair.second;
            auto it = std::find_if(tracked_params_.begin(), tracked_params_.end(),
                [&ovr_name](const std::pair<std::string, float>& p) {
                    return p.first == ovr_name;
                });
            if (it != tracked_params_.end())
                it->second = ovr_value;
            else
                tracked_params_.emplace_back(ovr_name, ovr_value);
        }
    }
}

int CueList::find_next_from(int from) const
{
    if (entries_.empty())
        return -1;
    const int next = from + 1;
    if (next >= static_cast<int>(entries_.size()))
        return -1;
    return next;
}

int CueList::find_prev_from(int from) const
{
    if (entries_.empty())
        return -1;
    const int prev = from - 1;
    if (prev < 0)
        return -1;
    return prev;
}

double CueList::apply_curve(double alpha, PathInterp interp) const
{
    // Clamp alpha to [0, 1] before applying any curve.
    const double a = (alpha < 0.0) ? 0.0 : ((alpha > 1.0) ? 1.0 : alpha);

    switch (interp) {
    case PathInterp::Linear:
        return a;

    case PathInterp::SCurve: {
        // Smoothstep: 3t² − 2t³
        return a * a * (3.0 - 2.0 * a);
    }

    case PathInterp::EaseIn:
        return a * a;

    case PathInterp::EaseOut:
        return 1.0 - (1.0 - a) * (1.0 - a);

    case PathInterp::EaseInOut: {
        // Cubic smoothstep: same formula as SCurve but kept as a named alias
        // to match MagicQ's naming.
        return a * a * (3.0 - 2.0 * a);
    }

    case PathInterp::SnapLate:
        // Held at 0 until very end, then snaps to 1.
        return (a >= 0.99) ? 1.0 : 0.0;

    case PathInterp::SnapEarly:
        // Snaps to 1 immediately at the start.
        return (a > 0.0) ? 1.0 : 0.0;

    case PathInterp::Expression:
        // Expression-based curves require evaluation context not available here;
        // fall back to linear so the cue still advances cleanly.
        return a;
    }

    return a; // unreachable; silences MSVC C4715
}

double CueList::compute_fade_level(double t, const CueTimingBlock& timing) const
{
    // BUG #55 fix: NaN/Inf in timing fields propagates through the phase boundaries
    // and ultimately to the int16_t cast in the engine, producing undefined behaviour.
    // Clamp each field to a finite, non-negative value before use.
    auto safe_timing = [](float v) -> double {
        return std::isfinite(v) && v >= 0.f ? static_cast<double>(v) : 0.0;
    };
    const double delay_in  = safe_timing(timing.delay_in);
    const double fade_in   = safe_timing(timing.fade_in);
    const double hold_dur  = safe_timing(timing.hold);
    const double delay_out = safe_timing(timing.delay_out);
    const double fade_out  = safe_timing(timing.fade_out);

    // ── Phase boundaries ─────────────────────────────────────────────────────
    const double p1 = delay_in;               // end of delay_in / start of fade_in
    const double p2 = p1 + fade_in;           // end of fade_in  / start of hold
    const double p3 = p2 + hold_dur;          // end of hold     / start of delay_out
    const double p4 = p3 + delay_out;         // end of delay_out / start of fade_out
    // p4 + fade_out = end of fade_out

    if (t < p1) {
        // Pre-fade delay: level is 0 during delay_in.
        return 0.0;
    }

    if (t < p2) {
        // Fade-in phase.
        if (fade_in <= 0.0)
            return 1.0;
        const double alpha = (t - p1) / fade_in;
        return apply_curve(alpha, timing.path);
    }

    if (t < p3) {
        // Hold phase: full intensity.
        return 1.0;
    }

    if (t < p4) {
        // Post-hold delay: level stays at 1 during delay_out.
        return 1.0;
    }

    // Fade-out phase.
    if (fade_out <= 0.0)
        return 0.0;
    const double alpha = 1.0 - ((t - p4) / fade_out);
    // BUG #55 fix: clamp the final level to [0,1] so any residual floating-point
    // imprecision never produces a value outside valid range before int16_t casts.
    const double level = apply_curve(alpha, timing.path);
    return (level < 0.0) ? 0.0 : ((level > 1.0) ? 1.0 : level);
}

void CueList::handle_link(std::vector<PlaybackEventData>& out_events)
{
    if (current_idx_ < 0 || current_idx_ >= static_cast<int>(entries_.size()))
        return;

    const FullCueEntry& cur = entries_[static_cast<std::size_t>(current_idx_)];

    switch (cur.link) {
    case LinkMode::Stop: {
        // Begin release.
        state_        = PlaybackState::Releasing;
        release_time_ = 0.5f;

        PlaybackEventData ev;
        ev.event       = PlaybackEvent::CueReleased;
        ev.entry_index = current_idx_;
        ev.cue_number  = cur.number;
        ev.time_s      = cue_time_;
        out_events.push_back(ev);
        break;
    }

    case LinkMode::Loop: {
        if (!entries_.empty()) {
            // BUG #76: capture time_s BEFORE activate_entry resets cue_time_ to 0.
            float reported_time_s = static_cast<float>(cue_time_);
            activate_entry(0, true, out_events);

            PlaybackEventData ev;
            ev.event       = PlaybackEvent::LoopedBack;
            ev.entry_index = 0;
            ev.cue_number  = entries_.front().number;
            ev.time_s      = reported_time_s;
            out_events.push_back(ev);
        }
        break;
    }

    case LinkMode::Next: {
        const int next = find_next_from(current_idx_);
        if (next < 0) {
            // Past end of list — stop.
            state_        = PlaybackState::Releasing;
            release_time_ = 0.5f;

            PlaybackEventData ev;
            ev.event       = PlaybackEvent::CueReleased;
            ev.entry_index = current_idx_;
            ev.cue_number  = cur.number;
            ev.time_s      = cue_time_;
            out_events.push_back(ev);
        } else {
            activate_entry(next, true, out_events);
        }
        break;
    }

    case LinkMode::Jump: {
        const CueNumber target{cur.jump_major, cur.jump_minor};
        const int       target_idx = find_by_number(target);
        if (target_idx < 0) {
            // Target not found — treat as Stop.
            state_        = PlaybackState::Releasing;
            release_time_ = 0.5f;

            PlaybackEventData ev;
            ev.event       = PlaybackEvent::CueReleased;
            ev.entry_index = current_idx_;
            ev.cue_number  = cur.number;
            ev.time_s      = cue_time_;
            out_events.push_back(ev);
        } else {
            // BUG #76: capture time_s BEFORE activate_entry resets cue_time_ to 0.
            float reported_time_s = static_cast<float>(cue_time_);
            activate_entry(target_idx, true, out_events);

            PlaybackEventData ev;
            ev.event       = PlaybackEvent::JumpedTo;
            ev.entry_index = target_idx;
            ev.cue_number  = entries_[static_cast<std::size_t>(target_idx)].number;
            ev.time_s      = reported_time_s;
            out_events.push_back(ev);
        }
        break;
    }
    } // switch link
}

} // namespace idhmfis
