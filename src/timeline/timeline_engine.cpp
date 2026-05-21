// timeline_engine.cpp — SMPTE-chase timeline engine implementation.

#include "timeline_engine.h"
#include <algorithm>
#include <cmath>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Private helpers
// ─────────────────────────────────────────────────────────────────────────────
TimelineEngine::Runtime* TimelineEngine::find_runtime(const std::string& id)
{
    for (size_t i = 0; i < defs_.size(); ++i)
        if (defs_[i].id == id) return &runtimes_[i];
    return nullptr;
}

const TimelineEngine::Runtime* TimelineEngine::find_runtime(const std::string& id) const
{
    for (size_t i = 0; i < defs_.size(); ++i)
        if (defs_[i].id == id) return &runtimes_[i];
    return nullptr;
}

TimelineDef* TimelineEngine::find_def_mut(const std::string& id)
{
    for (auto& d : defs_)
        if (d.id == id) return &d;
    return nullptr;
}

const TimelineDef* TimelineEngine::find_def(const std::string& id) const
{
    for (const auto& d : defs_)
        if (d.id == id) return &d;
    return nullptr;
}

TimelineTrack* TimelineEngine::find_track(TimelineDef& def, int track_id)
{
    for (auto& t : def.tracks)
        if (t.id == track_id) return &t;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  collect_events — fire events in (last_event_pos, new_pos]
// ─────────────────────────────────────────────────────────────────────────────
void TimelineEngine::collect_events(const TimelineDef& def, Runtime& rt,
                                    int64_t new_pos, std::vector<FiredEvent>& out)
{
    for (const auto& track : def.tracks) {
        if (track.muted) continue;
        // Binary search to the first event after last_event_pos
        TimelineEvent key;
        key.tc_position = rt.last_event_pos + 1;
        auto it = std::lower_bound(track.events.begin(), track.events.end(), key,
            [](const TimelineEvent& a, const TimelineEvent& b){
                return a.tc_position < b.tc_position;
            });
        for (; it != track.events.end() && it->tc_position <= new_pos; ++it) {
            FiredEvent fe;
            fe.timeline_id = def.id;
            fe.event       = *it;
            // If this is a WaitForGo event, set the wait flag before adding to output
            if (it->type == TimelineEventType::WaitForGo)
                rt.wait_for_go = true;
            out.push_back(std::move(fe));
        }
    }
    rt.last_event_pos = new_pos;
}

// ─────────────────────────────────────────────────────────────────────────────
//  tick — called once per engine tick (1000 Hz)
// ─────────────────────────────────────────────────────────────────────────────
std::vector<TimelineEngine::FiredEvent>
TimelineEngine::tick(const TimecodeState& tc, double dt_s)
{
    std::vector<FiredEvent> fired;

    for (size_t i = 0; i < defs_.size(); ++i) {
        const TimelineDef& def = defs_[i];
        Runtime&           rt  = runtimes_[i];

        // Internal-clock mode (link_mode == true OR slot == "Internal")
        bool use_internal = def.link_mode
                         || def.tc_slot == "Internal";

        if (use_internal) {
            // Advance internal clock only when Playing and not waiting for Go
            if (rt.state == TimelineState::Playing && !rt.wait_for_go) {
                rt.internal_clock += dt_s;
                int fps_int = smpte_max_frames(def.fps);
                if (fps_int <= 0) fps_int = 25;
                int64_t new_pos = static_cast<int64_t>(
                    rt.internal_clock * static_cast<double>(fps_int));

                // Clamp to length if bounded
                if (def.length_frames > 0 && new_pos >= def.length_frames) {
                    new_pos = def.length_frames;
                    // Collect events up to length then stop
                    collect_events(def, rt, new_pos, fired);
                    rt.pos   = new_pos;
                    rt.state = TimelineState::Idle;
                    continue;
                }

                collect_events(def, rt, new_pos, fired);
                rt.pos = new_pos;
            }
            // Armed → Playing when user calls play()
        } else {
            // External TC chase mode
            if (!tc.valid) continue;

            int fps_int = smpte_max_frames(def.fps);
            if (fps_int <= 0) fps_int = 25;

            int64_t tc_abs   = tc.total_frames();
            int64_t new_pos  = tc_abs - def.time_offset_frames;

            // Ignore negative positions (before timeline start)
            if (new_pos < 0) {
                rt.state = TimelineState::Armed;
                continue;
            }

            // Jump detect: if position jumped more than threshold → re-sync.
            // Use the per-slot jump_detect_frames from the TC config if available.
            int jump_thresh = 10; // default
            {
                auto it = tc_config_.slots.find(def.tc_slot);
                if (it != tc_config_.slots.end())
                    jump_thresh = it->second.jump_detect_frames;
            }
            int64_t delta = new_pos - rt.pos;
            if (delta < 0) delta = -delta;
            if (rt.state == TimelineState::Playing && delta > jump_thresh) {
                // Re-sync: update pos but don't fire skipped/jumped events
                rt.pos            = new_pos;
                rt.last_event_pos = new_pos;  // not new_pos-1: avoids spurious re-fire
                continue;
            }

            // Transition Armed → Playing when we enter [0, length_frames]
            bool in_range = (def.length_frames <= 0)
                         || (new_pos < def.length_frames);
            if (rt.state == TimelineState::Armed && in_range) {
                rt.state = TimelineState::Playing;
            }

            if (rt.state == TimelineState::Playing) {
                // Clamp to length
                if (def.length_frames > 0 && new_pos >= def.length_frames) {
                    new_pos = def.length_frames;
                    collect_events(def, rt, new_pos, fired);
                    rt.pos   = new_pos;
                    rt.state = TimelineState::Idle;
                    continue;
                }
                collect_events(def, rt, new_pos, fired);
                rt.pos = new_pos;
            }
        }
    }

    return fired;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Transport controls
// ─────────────────────────────────────────────────────────────────────────────
void TimelineEngine::play(const std::string& id)
{
    Runtime* rt = find_runtime(id);
    if (!rt) return;
    rt->state       = TimelineState::Playing;
    rt->wait_for_go = false;
}

void TimelineEngine::pause(const std::string& id)
{
    Runtime* rt = find_runtime(id);
    if (!rt) return;
    if (rt->state == TimelineState::Playing)
        rt->state = TimelineState::Paused;
    else if (rt->state == TimelineState::Paused)
        rt->state = TimelineState::Playing;
}

void TimelineEngine::stop(const std::string& id)
{
    Runtime* rt = find_runtime(id);
    if (!rt) return;
    rt->state          = TimelineState::Idle;
    rt->internal_clock = 0.0;
    rt->pos            = 0;
    rt->last_event_pos = -1;
    rt->wait_for_go    = false;
}

void TimelineEngine::rewind(const std::string& id)
{
    Runtime* rt = find_runtime(id);
    if (!rt) return;
    rt->internal_clock = 0.0;
    rt->pos            = 0;
    rt->last_event_pos = -1;
}

void TimelineEngine::seek(const std::string& id, int64_t frame)
{
    Runtime* rt = find_runtime(id);
    const TimelineDef* def = find_def(id);
    if (!rt || !def) return;
    int fps_int = smpte_max_frames(def->fps);
    if (fps_int <= 0) fps_int = 25;
    if (frame < 0) frame = 0;
    rt->pos            = frame;
    rt->last_event_pos = frame - 1;
    rt->internal_clock = static_cast<double>(frame) / static_cast<double>(fps_int);
}

void TimelineEngine::set_armed(const std::string& id, bool armed)
{
    Runtime* rt = find_runtime(id);
    if (!rt) return;
    if (armed && rt->state == TimelineState::Idle)
        rt->state = TimelineState::Armed;
    else if (!armed && rt->state == TimelineState::Armed)
        rt->state = TimelineState::Idle;
}

void TimelineEngine::wait_for_go_advance(const std::string& id)
{
    Runtime* rt = find_runtime(id);
    if (!rt) return;
    rt->wait_for_go = false;
}

void TimelineEngine::set_wait_for_go(const std::string& id)
{
    Runtime* rt = find_runtime(id);
    if (!rt) return;
    rt->wait_for_go = true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CRUD
// ─────────────────────────────────────────────────────────────────────────────
void TimelineEngine::create(TimelineDef def)
{
    // Avoid duplicates
    for (const auto& d : defs_)
        if (d.id == def.id) return;

    defs_.push_back(std::move(def));
    runtimes_.emplace_back();
}

void TimelineEngine::remove(const std::string& id)
{
    for (size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].id == id) {
            defs_.erase(defs_.begin() + static_cast<ptrdiff_t>(i));
            runtimes_.erase(runtimes_.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

void TimelineEngine::update_def(TimelineDef def)
{
    for (size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].id == def.id) {
            defs_[i] = std::move(def);
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Track helpers
// ─────────────────────────────────────────────────────────────────────────────
void TimelineEngine::add_track(const std::string& timeline_id,
                                const std::string& name)
{
    Runtime*    rt  = find_runtime(timeline_id);
    TimelineDef* def = find_def_mut(timeline_id);
    if (!rt || !def) return;

    TimelineTrack track;
    track.id   = rt->next_track_id++;
    track.name = name;
    def->tracks.push_back(std::move(track));
}

void TimelineEngine::remove_track(const std::string& timeline_id, int track_id)
{
    TimelineDef* def = find_def_mut(timeline_id);
    if (!def) return;
    def->tracks.erase(
        std::remove_if(def->tracks.begin(), def->tracks.end(),
                       [track_id](const TimelineTrack& t){ return t.id == track_id; }),
        def->tracks.end());
}

void TimelineEngine::update_track(const std::string& timeline_id, int track_id,
                                   const std::string& name, bool muted,
                                   bool locked, bool collapsed)
{
    TimelineDef* def = find_def_mut(timeline_id);
    if (!def) return;
    TimelineTrack* t = find_track(*def, track_id);
    if (!t) return;
    t->name      = name;
    t->muted     = muted;
    t->locked    = locked;
    t->collapsed = collapsed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Event helpers
// ─────────────────────────────────────────────────────────────────────────────
void TimelineEngine::add_event(const std::string& timeline_id, int track_id,
                                TimelineEvent ev)
{
    Runtime*    rt  = find_runtime(timeline_id);
    TimelineDef* def = find_def_mut(timeline_id);
    if (!rt || !def) return;
    TimelineTrack* t = find_track(*def, track_id);
    if (!t) return;

    ev.id = rt->next_event_id++;
    // Insert in sorted order by tc_position
    auto it = std::lower_bound(t->events.begin(), t->events.end(), ev,
        [](const TimelineEvent& a, const TimelineEvent& b){
            return a.tc_position < b.tc_position;
        });
    t->events.insert(it, std::move(ev));
}

void TimelineEngine::remove_event(const std::string& timeline_id, int track_id,
                                   int64_t event_id)
{
    TimelineDef* def = find_def_mut(timeline_id);
    if (!def) return;
    TimelineTrack* t = find_track(*def, track_id);
    if (!t) return;
    // event_id is the tc_position passed from the UI layer — not the internal sequential id.
    t->events.erase(
        std::remove_if(t->events.begin(), t->events.end(),
                       [event_id](const TimelineEvent& e){ return e.tc_position == event_id; }),
        t->events.end());
}

void TimelineEngine::update_event(const std::string& timeline_id, int track_id,
                                   TimelineEvent ev)
{
    TimelineDef* def = find_def_mut(timeline_id);
    if (!def) return;
    TimelineTrack* t = find_track(*def, track_id);
    if (!t) return;
    for (auto& e : t->events) {
        if (e.id == ev.id) {
            e = std::move(ev);
            break;
        }
    }
    // Re-sort after possible position change
    std::sort(t->events.begin(), t->events.end(),
              [](const TimelineEvent& a, const TimelineEvent& b){
                  return a.tc_position < b.tc_position;
              });
}

// ─────────────────────────────────────────────────────────────────────────────
//  source_status
// ─────────────────────────────────────────────────────────────────────────────
TimecodeSourceStatus TimelineEngine::source_status(const TimecodeState& tc,
                                                    const std::string&   slot) const
{
    if (slot == "Internal") return TimecodeSourceStatus::Disabled;
    if (!tc.valid)          return TimecodeSourceStatus::Disabled;
    // Check if any timeline using this slot is actively playing
    for (size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].tc_slot == slot && !defs_[i].link_mode) {
            if (runtimes_[i].state == TimelineState::Playing)
                return TimecodeSourceStatus::Active;
        }
    }
    return TimecodeSourceStatus::Present;
}

// ─────────────────────────────────────────────────────────────────────────────
//  runtime_snaps
// ─────────────────────────────────────────────────────────────────────────────
std::vector<TimelineEngine::RuntimeSnap> TimelineEngine::runtime_snaps() const
{
    std::vector<RuntimeSnap> out;
    out.reserve(defs_.size());
    for (size_t i = 0; i < defs_.size(); ++i) {
        RuntimeSnap rs;
        rs.id              = defs_[i].id;
        rs.state           = runtimes_[i].state;
        rs.position_frames = runtimes_[i].pos;
        rs.source_status   = TimecodeSourceStatus::Disabled;
        out.push_back(std::move(rs));
    }
    return out;
}

} // namespace idhmfis
