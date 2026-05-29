// show_engine.cpp -- 1000 Hz show engine implementation.
// Timing strategy: record the absolute "next tick deadline" in nanoseconds and
// spin-sleep to it. This gives much better than 1 ms jitter without requiring
// SCHED_FIFO/real-time on Windows. The engine thread is set to THREAD_PRIORITY_HIGHEST.

#include "show_engine.h"
#include "thread_utils.h"
#include "logger.h"
#include "../project/project.h"
#include "../project/cue.h"
#include "../generators/igenerator.h"
#include "../generators/generator_registry.h"
#include "../audio/audio_analyzer.h"
#include "../cuelist/cuelist_types.h"
#include "../fx/fx_block.h"

#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include "../audio/timeline_audio_player.h"

namespace idhmfis {

// Globals defined in generator translation units — declared here at namespace scope
// so block-scope extern decls inside functions don't fall into global namespace.
extern AudioSnapshot g_last_audio_snap;
extern void set_ilda_path(const std::string&);

// Mutex protecting g_last_audio_snap against concurrent reads from generator threads.
static std::mutex g_audio_snap_mtx;

// ─────────────────────────────────────────────────────────────────────────────
//  ShowEngine constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
ShowEngine::ShowEngine(RenderBus& bus, AudioAnalyzer& audio)
    : bus_(bus), audio_(audio)
{
    // Create a default empty project so the engine never dereferences null.
    project_ = std::make_shared<Project>();

    // Disable scan-fail by default — no physical laser hardware at startup.
    // The operator enables it in Settings → Safety when real hardware is connected.
    {
        ScanFailMonitor::Config sf_cfg;
        sf_cfg.enabled = false;
        safety_.scan_fail.configure(sf_cfg);
    }

    // Register hot-plug callback — called from the scan thread.
    // We only set a flag here; the engine thread reads it on its own tick.
    dac_registry_.on_changed = [this](const std::vector<DacDescriptor>&) {
        dac_changed_.store(true, std::memory_order_release);
    };

    // Start background scanning for laser DAC devices.
    dac_registry_.start_scan();
}

ShowEngine::~ShowEngine()
{
    stop();
    dac_registry_.stop_scan();
}

// ─────────────────────────────────────────────────────────────────────────────
//  start / stop
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::start()
{
    // H-1: lifecycle_mtx_ prevents concurrent start() calls from both passing
    // the running_ check and spawning two engine threads.
    std::lock_guard<std::mutex> lk(lifecycle_mtx_);
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ShowEngine::engine_loop, this);
    set_thread_priority(thread_, ThreadPriority::High);
    set_thread_name("idhmfis-engine");
    log::info("ShowEngine started");
}

void ShowEngine::stop()
{
    // H-1: lifecycle_mtx_ prevents a concurrent stop() from calling join() on a
    // thread that is already joined or not yet joinable.
    std::lock_guard<std::mutex> lk(lifecycle_mtx_);
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    log::info("ShowEngine stopped");
}

void ShowEngine::restart()
{
    // Signal the engine thread to stop, wait up to 2 seconds, then detach if frozen.
    std::lock_guard<std::mutex> lk(lifecycle_mtx_);
    if (running_.load(std::memory_order_acquire)) {
        running_.store(false, std::memory_order_release);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (thread_.joinable() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (thread_.joinable()) {
            log::warn("ShowEngine::restart: engine thread frozen — detaching");
            thread_.detach(); // frozen — abandon old thread
        }
    }
    // Start a fresh engine thread.
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ShowEngine::engine_loop, this);
    set_thread_priority(thread_, ThreadPriority::High);
    set_thread_name("idhmfis-engine");
    log::info("ShowEngine restarted");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Thread-safe command / dmx input interfaces
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::send(EngineCommand cmd)
{
    if (!cmd_queue_.try_push(std::move(cmd)))
        log::warn("ShowEngine: command queue full, command dropped");
}

void ShowEngine::push_dmx(int universe, const DmxUniverse& data)
{
    if (universe < 0 || universe >= kMaxUniverses) return;
    dmx_queue_.try_push({ universe, data });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Snapshot (double-buffered, non-blocking read)
// ─────────────────────────────────────────────────────────────────────────────
EngineSnapshot ShowEngine::snapshot() const
{
    std::lock_guard<std::mutex> lk(snap_mtx_);
    // C-2: acquire pairs with the release in update_snapshot() for correct
    // visibility of snap_[] contents written before the index flip.
    int ri = 1 - snap_write_idx_.load(std::memory_order_acquire);
    return snap_[ri];
}

// ─────────────────────────────────────────────────────────────────────────────
//  engine_loop — 1000 Hz main loop
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::engine_loop()
{
    set_thread_priority_current(ThreadPriority::High);
    set_thread_name("idhmfis-engine");

    using Clock    = std::chrono::steady_clock;
    using ns       = std::chrono::nanoseconds;
    constexpr int64_t kPeriodNs = 1'000'000LL; // 1 ms = 1000 Hz

    frame_timer_.reset();

    auto next_tick = Clock::now();
    double prev_time = HRTimer::now_s();

    // FPS tracking
    RollingStats<256> fps_stats;
    HRTimer last_fps_timer;

    while (running_.load(std::memory_order_acquire))
    {
        // C-1: guard the entire tick body against exceptions so a bug in any
        // subsystem (generators, FX, command handlers, …) cannot kill the engine
        // thread — it logs the error and continues ticking at the next deadline.
        try
        {
        double now_s = HRTimer::now_s();
        double dt    = now_s - prev_time;
        prev_time    = now_s;

        // Clamp dt to prevent spiral-of-death after a stall
        if (dt > 0.01) dt = 0.01;

        // ── Process all pending commands ──────────────────────────────
        process_commands();

        // ── Tick timeline engine — chase timecode and fire events ─────
        // Halted when emergency shutoff is active: a show file must never
        // be able to reconfigure safety subsystems while the e-stop is latched.
        if (!emergency_shutoff_active_) {
            TimecodeState tc = timecode_router_.active();
            auto fired = timeline_engine_.tick(tc, dt);
            for (const auto& fe : fired)
                dispatch_timeline_event(fe);
        }

        // ── Apply queued DMX data ─────────────────────────────────────
        process_dmx_input();

        // ── Audio tick (runs its own capture/FFT on audio thread;
        //    this just drains results into the snapshot cache) ──────────
        audio_.tick();

        // Store dt for use in build_frame()
        last_dt_ = static_cast<float>(dt);

        // ── Update expression context ─────────────────────────────────
        {
            expr_ctx_.t   = time_;
            expr_ctx_.bpm = bpm_;
            AudioSnapshot audio_s = audio_.snapshot();
            expr_ctx_.rms      = audio_s.rms;
            expr_ctx_.sub      = audio_s.sub_band;
            expr_ctx_.mid      = audio_s.mid_band;
            expr_ctx_.hi       = audio_s.high_band;
            expr_ctx_.beat_now = audio_s.beat_now;
            expr_ctx_.audio    = audio_s;
            if (bpm_ > 0.f) {
                double beat_period = 60.0 / static_cast<double>(bpm_);
                expr_ctx_.beat = std::fmod(time_, beat_period) / beat_period;
                expr_ctx_.bar  = std::fmod(time_, beat_period * 4.0) / (beat_period * 4.0);
            }
        }

        // ── Always advance FX time (not tied to transport play state) ──
        fx_time_ += dt;

        // ── Advance time if playing ───────────────────────────────────
        if (playing_)
        {
            time_ += dt;

            // Tick the cue list state machine
            auto events = cue_list_.tick(dt);
            for (const auto& ev : events) {
                if (ev.event == PlaybackEvent::CueActivated) {
                    if (ev.entry_index >= 0
                        && ev.entry_index < cue_list_.entry_count())
                    {
                        const auto& fe = cue_list_.entry_at(ev.entry_index);
                        int ci = project_ ? project_->find_cue(fe.cue_id) : -1;
                        if (ci >= 0) {
                            active_cue_ = ci;
                            time_       = 0.0;
                        }
                    }
                }
            }
            cue_list_idx_ = cue_list_.current_idx();

            // Wrap cue time if looping
            if (project_ && active_cue_ >= 0
                && active_cue_ < static_cast<int>(project_->cues.size()))
            {
                const Cue& cue = project_->cues[active_cue_];
                if (cue.loop && cue.duration > 0.0 && time_ > cue.duration)
                    time_ = std::fmod(time_, cue.duration);
            }

            // ── Evaluate automation / DMX for active cue ─────────────
            evaluate_cues(time_, dt);

            // ── Generate frame ────────────────────────────────────────
            build_frame();
        }
        else
        {
            // Not playing — tick cue list with zero dt to keep state stable
            cue_list_.tick(0.0);
            cue_list_idx_ = cue_list_.current_idx();

            // Not playing — still evaluate and build to keep preview live
            evaluate_cues(time_, 0.0);
            build_frame();
        }

        // ── Update watchdog ───────────────────────────────────────────
        watchdog_heartbeat_.fetch_add(1, std::memory_order_relaxed);

        ++frame_count_;

        // ── FPS measurement ────────────────────────────────────────────
        double elapsed = last_fps_timer.elapsed_s();
        if (elapsed > 0.0)
            fps_stats.push(1.0 / elapsed);
        last_fps_timer.reset();

        // ── Update snapshot every 16 ms (~60 fps) ─────────────────────
        if (frame_count_ % 16 == 0)
            update_snapshot(fps_stats.mean());

        // ── Sleep until next tick ─────────────────────────────────────
        next_tick += ns(kPeriodNs);
        auto now_tp = Clock::now();
        if (now_tp < next_tick)
        {
            // For the final ~200 µs spin rather than sleep to reduce jitter.
            auto sleep_until = next_tick - std::chrono::microseconds(200);
            if (now_tp < sleep_until)
                std::this_thread::sleep_until(sleep_until);
            // Tight spin for the last 200 µs
            while (Clock::now() < next_tick) { /* tight spin */ }
        }
        else
        {
            // We overran — reset deadline to avoid catching up for many frames.
            next_tick = Clock::now();
        }

        } // end try
        catch (const std::exception& ex) {
            log::error("ShowEngine: exception in engine loop: %s", ex.what());
            // Reset deadline so we don't cascade overruns after the exception.
            next_tick = Clock::now();
        }
        catch (...) {
            log::error("ShowEngine: unknown exception in engine loop");
            next_tick = Clock::now();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Playback helper accessors
// ─────────────────────────────────────────────────────────────────────────────
PlaybackDef* ShowEngine::find_playback_def(int id)
{
    if (!project_) return nullptr;
    for (auto& pb : project_->playbacks)
        if (pb.id == id) return &pb;
    return nullptr;
}

const PlaybackDef* ShowEngine::find_playback_def(int id) const
{
    if (!project_) return nullptr;
    for (const auto& pb : project_->playbacks)
        if (pb.id == id) return &pb;
    return nullptr;
}

ShowEngine::PlaybackRunState* ShowEngine::find_playback_state(int id)
{
    for (auto& rs : playback_states_)
        if (rs.id == id) return &rs;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  render_keyframe_layer — rasterise LaserObjects to a PointBuffer
// ─────────────────────────────────────────────────────────────────────────────
// Apply point-level symmetry copies to a PointBuffer, appending them.
// Mirrors/rotates all non-blanked segments. cx/cy are normalized centre coords.
static void apply_pb_symmetry(PointBuffer& buf, int mode, float cx, float cy)
{
    if (mode == 0) return;
    const int orig_sz = static_cast<int>(buf.size());
    if (orig_sz == 0) return;

    // Snapshot the original points so push_back can't invalidate references
    PointBuffer orig(buf.begin(), buf.end());

    auto make_mirror = [&](auto xform) {
        bool need_blank = true;
        for (int i = 0; i < orig_sz; ++i) {
            const LaserPoint& p = orig[i];
            float nx = p.nx() - cx;
            float ny = p.ny() - cy;
            float tx, ty;
            xform(nx, ny, tx, ty);
            tx += cx; ty += cy;
            buf.push_back(LaserPoint::from_norm(tx, ty, p.r, p.g, p.b,
                                                need_blank || p.blanked));
            need_blank = false;
        }
    };

    auto make_rotate = [&](float angle) {
        float ca = std::cos(angle), sa = std::sin(angle);
        bool need_blank = true;
        for (int i = 0; i < orig_sz; ++i) {
            const LaserPoint& p = orig[i];
            float nx = p.nx() - cx;
            float ny = p.ny() - cy;
            float tx = nx * ca - ny * sa + cx;
            float ty = nx * sa + ny * ca + cy;
            buf.push_back(LaserPoint::from_norm(tx, ty, p.r, p.g, p.b,
                                                need_blank || p.blanked));
            need_blank = false;
        }
    };

    static constexpr float kPi = 3.14159265f;

    if (mode == 1) { // MirrorX
        make_mirror([](float nx, float ny, float& tx, float& ty){ tx = -nx; ty =  ny; });
    } else if (mode == 2) { // MirrorY
        make_mirror([](float nx, float ny, float& tx, float& ty){ tx =  nx; ty = -ny; });
    } else if (mode == 3) { // MirrorXY
        make_mirror([](float nx, float ny, float& tx, float& ty){ tx = -nx; ty =  ny; });
        make_mirror([](float nx, float ny, float& tx, float& ty){ tx =  nx; ty = -ny; });
        make_mirror([](float nx, float ny, float& tx, float& ty){ tx = -nx; ty = -ny; });
    } else {
        // Radial copies
        int n_copies = 2;
        if      (mode == 4) n_copies = 2;
        else if (mode == 5) n_copies = 3;
        else if (mode == 6) n_copies = 4;
        else if (mode == 7) n_copies = 6;
        else if (mode == 8) n_copies = 8;
        else if (mode == 9) n_copies = 12;
        for (int k = 1; k < n_copies; ++k)
            make_rotate(k * 2.f * kPi / static_cast<float>(n_copies));
    }
}

PointBuffer ShowEngine::render_keyframe_layer(const KeyframeLayer& kf, int target_pts)
{
    PointBuffer out;
    out.reserve(static_cast<size_t>(target_pts));
    for (const auto& obj : kf.objects) {
        // Skip symmetry-copy objects stored by the UI — symmetry is applied at PointBuffer level
        if (obj.text.rfind("__sym", 0) == 0) continue;
        if (obj.pts.empty()) continue;
        uint8_t r8 = static_cast<uint8_t>(std::min(255.f, obj.r * 255.f));
        uint8_t g8 = static_cast<uint8_t>(std::min(255.f, obj.g * 255.f));
        uint8_t b8 = static_cast<uint8_t>(std::min(255.f, obj.b * 255.f));
        // Travel to first point (blanked)
        out.push_back(LaserPoint::from_norm(obj.pts[0].x, obj.pts[0].y, r8, g8, b8, true));
        static constexpr float kTwoPi = 6.28318f;
        if (obj.type == LaserObjectType::Dot) {
            // Dot: render as a small circle at pts[0] with radius obj.size
            int circle_pts = std::max(8, target_pts / std::max(1, static_cast<int>(kf.objects.size()) + 1));
            for (int ci = 0; ci <= circle_pts; ++ci) {
                float a  = kTwoPi * static_cast<float>(ci) / static_cast<float>(circle_pts);
                float cx = obj.pts[0].x + obj.size * std::cos(a);
                float cy = obj.pts[0].y + obj.size * std::sin(a);
                out.push_back(LaserPoint::from_norm(cx, cy, r8, g8, b8, ci == 0));
            }
        } else if (obj.type == LaserObjectType::Circle && obj.pts.size() >= 2) {
            // Circle: pts[0]=center, pts[1]=edge point — compute radius and rasterize
            float cx2 = obj.pts[0].x, cy2 = obj.pts[0].y;
            float dx   = obj.pts[1].x - cx2, dy = obj.pts[1].y - cy2;
            float radius = std::sqrt(dx * dx + dy * dy);
            int n_pts = std::max(32, target_pts / std::max(1, (int)kf.objects.size()));
            for (int k = 0; k <= n_pts; ++k) {
                float a  = kTwoPi * static_cast<float>(k) / static_cast<float>(n_pts);
                float px = cx2 + radius * std::cos(a);
                float py = cy2 + radius * std::sin(a);
                out.push_back(LaserPoint::from_norm(px, py, r8, g8, b8, k == 0));
            }
        } else if (obj.type == LaserObjectType::Bezier) {
            // Cubic Bezier: chained segments sharing endpoints (seg += 3 stepping)
            // Segment N: pts[3N]=p0, pts[3N+1]=cp1, pts[3N+2]=cp2, pts[3N+3]=p3
            static constexpr int kBezierSteps = 32;
            for (int seg = 0; seg + 3 < (int)obj.pts.size(); seg += 3) {
                float x0 = obj.pts[seg+0].x, y0 = obj.pts[seg+0].y;
                float x1 = obj.pts[seg+1].x, y1 = obj.pts[seg+1].y;
                float x2 = obj.pts[seg+2].x, y2 = obj.pts[seg+2].y;
                float x3 = obj.pts[seg+3].x, y3 = obj.pts[seg+3].y;
                for (int step = 0; step <= kBezierSteps; ++step) {
                    float t  = static_cast<float>(step) / static_cast<float>(kBezierSteps);
                    float mt = 1.f - t;
                    float px = mt*mt*mt*x0 + 3.f*mt*mt*t*x1 + 3.f*mt*t*t*x2 + t*t*t*x3;
                    float py = mt*mt*mt*y0 + 3.f*mt*mt*t*y1 + 3.f*mt*t*t*y2 + t*t*t*y3;
                    out.push_back(LaserPoint::from_norm(px, py, r8, g8, b8, false));
                }
            }
        } else if (obj.type == LaserObjectType::Arc && obj.pts.size() >= 3) {
            // Arc: pts[0]=center, pts[1]=start-angle/radius point, pts[2]=end-angle point
            float cx = obj.pts[0].x, cy = obj.pts[0].y;
            float dx = obj.pts[1].x - cx, dy = obj.pts[1].y - cy;
            float radius = std::sqrt(dx * dx + dy * dy);
            float a_start = std::atan2(dy, dx);
            float dx2 = obj.pts[2].x - cx, dy2 = obj.pts[2].y - cy;
            float a_end = std::atan2(dy2, dx2);
            if (a_end <= a_start) a_end += kTwoPi;
            int n_pts = std::max(32, target_pts / std::max(1, (int)kf.objects.size()));
            for (int k = 0; k <= n_pts; ++k) {
                float t = static_cast<float>(k) / static_cast<float>(n_pts);
                float a = a_start + t * (a_end - a_start);
                float px = cx + radius * std::cos(a);
                float py = cy + radius * std::sin(a);
                out.push_back(LaserPoint::from_norm(px, py, r8, g8, b8, k == 0));
            }
        } else {
            // Line / Text / other: subdivide each segment proportionally to its length
            if (obj.pts.size() == 1) {
                out.push_back(LaserPoint::from_norm(obj.pts[0].x, obj.pts[0].y,
                    r8, g8, b8, false));
            } else if (obj.pts.size() >= 2) {
                int total_steps = std::max(2, target_pts / std::max(1, (int)kf.objects.size()));

                // Compute total polyline length
                float total_len = 0.f;
                for (size_t i = 0; i + 1 < obj.pts.size(); ++i) {
                    float ddx = obj.pts[i + 1].x - obj.pts[i].x;
                    float ddy = obj.pts[i + 1].y - obj.pts[i].y;
                    total_len += std::sqrt(ddx * ddx + ddy * ddy);
                }
                if (total_len < 1e-9f) total_len = 1e-9f;

                // Subdivide each segment proportionally
                for (size_t i = 0; i + 1 < obj.pts.size(); ++i) {
                    float ddx = obj.pts[i + 1].x - obj.pts[i].x;
                    float ddy = obj.pts[i + 1].y - obj.pts[i].y;
                    float seg_len = std::sqrt(ddx * ddx + ddy * ddy);
                    // Pen-up convention: a control point coincident with its
                    // predecessor marks a blank travel. The segment LEAVING that
                    // duplicate is the unlit jump to the next stroke, so its
                    // points are blanked — preventing ghost connector lines.
                    bool seg_blank = (i >= 1)
                        && obj.pts[i].x == obj.pts[i - 1].x
                        && obj.pts[i].y == obj.pts[i - 1].y;
                    int steps = std::max(2, static_cast<int>(total_steps * seg_len / total_len));
                    for (int k = 0; k < steps; ++k) {
                        float t = static_cast<float>(k) / static_cast<float>(steps - 1);
                        float px = obj.pts[i].x + t * ddx;
                        float py = obj.pts[i].y + t * ddy;
                        out.push_back(LaserPoint::from_norm(px, py, r8, g8, b8, seg_blank));
                    }
                }
            }
        }
    }
    // Apply symmetry at PointBuffer level (avoids ID-mismatch morph bugs)
    if (kf.symmetry_mode != 0)
        apply_pb_symmetry(out, kf.symmetry_mode, kf.sym_cx, kf.sym_cy);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Global Layer FX helpers
// ─────────────────────────────────────────────────────────────────────────────

// Deterministic per-segment noise, updated at `t*rate` quantised frequency.
static float det_noise(int seg, float rate_t, int channel = 0) {
    uint32_t h = (static_cast<uint32_t>(seg) * 2654435761u)
               ^ (static_cast<uint32_t>(channel) * 1234567u)
               ^ static_cast<uint32_t>(rate_t * 64.f);
    h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16;
    return (h & 0xFFFFu) / 65535.f;
}

// Evaluate a single global FX entry at time t (+ optional extra phase), returns 0..1
// smooth_state: optional pointer to per-slot smoothing accumulator (for CrossFade)
static float eval_global_fx(const GlobalFxEntry& e, float t, float extra_phase = 0.f, int seg_idx = 0,
                             float* smooth_state = nullptr) {
    float phase = std::fmod(t * e.rate + e.offset + extra_phase, 1.f);
    if (phase < 0.f) phase += 1.f;
    float raw = 0.f;
    switch (e.type) {
    case GlobalFxType::Sine:
        raw = 0.5f + 0.5f * std::sin(phase * 6.28318f); break;
    case GlobalFxType::Square:
        raw = (phase < e.duty) ? 1.f : 0.f; break;
    case GlobalFxType::Saw:
        raw = phase; break;
    case GlobalFxType::Triangle:
        raw = (phase < 0.5f) ? phase * 2.f : 2.f - phase * 2.f; break;
    case GlobalFxType::Flicker:
        raw = det_noise(seg_idx, t * e.rate); break;
    case GlobalFxType::RampUp:
        raw = phase; break;
    case GlobalFxType::RampDown:
        raw = 1.f - phase; break;
    case GlobalFxType::Bump: {
        float x = (phase - 0.5f) * 4.f;
        raw = std::exp(-x * x);
        break;
    }
    case GlobalFxType::Chase:
        raw = (phase < e.duty) ? 1.f : 0.f; break;
    case GlobalFxType::Strobe:
        raw = (phase < e.duty) ? 1.f : 0.f; break;
    }
    float fx_val = raw * e.depth;
    // CrossFade smoothing (low-pass filter)
    if (e.crossfade > 0.f && smooth_state != nullptr) {
        float alpha = 1.f - std::min(0.999f, e.crossfade * 0.98f);
        *smooth_state = *smooth_state + alpha * (fx_val - *smooth_state);
        fx_val = *smooth_state;
    }
    return fx_val;
}

// Apply global layer geometry (size + spread) to point coordinates
static void apply_global_geometry(PointBuffer& buf, const GlobalLayer& gl) {
    if (gl.size == 1.f && gl.spread == 1.f) return;
    static constexpr float kScale = 32767.f;
    for (auto& pt : buf) {
        if (pt.blanked) continue;
        float nx = pt.nx() * gl.size * gl.spread;
        float ny = pt.ny() * gl.size;
        pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
    }
}

// Apply global layer FX stack to intensity — returns final 0..1 multiplier
// gfx_smooth: optional 32-element per-slot CrossFade low-pass state array
static float eval_global_layer(const GlobalLayer& gl, float t, float* gfx_smooth = nullptr) {
    float intensity = gl.global_dim;
    for (int gfi = 0; gfi < static_cast<int>(gl.fx.size()); ++gfi) {
        const auto& e = gl.fx[static_cast<size_t>(gfi)];
        if (!e.enabled) continue;
        float* sptr = (gfx_smooth && gfi < 32) ? &gfx_smooth[gfi] : nullptr;
        float v = eval_global_fx(e, t, 0.f, 0, sptr);
        switch (e.blend) {
        case FxBlendMode::Add:
            intensity = std::clamp(intensity + v, 0.f, 1.f); break;
        case FxBlendMode::Subtract:
            intensity = std::clamp(intensity - v, 0.f, 1.f); break;
        case FxBlendMode::Absolute:
            intensity = std::clamp(v, 0.f, 1.f); break;
        case FxBlendMode::Multiply:
            intensity = std::clamp(intensity * v, 0.f, 1.f); break;
        }
    }
    return intensity;
}

// ─────────────────────────────────────────────────────────────────────────────
//  HSV ↔ RGB helpers used by colour FX
// ─────────────────────────────────────────────────────────────────────────────
static void rgb_to_hsv(float r, float g, float b, float& h, float& s, float& v) {
    float cmax = std::max({r,g,b}), cmin = std::min({r,g,b}), d = cmax - cmin;
    v = cmax;
    s = (cmax > 0.f) ? d / cmax : 0.f;
    if (d == 0.f) { h = 0.f; return; }
    if      (cmax == r) h = std::fmod((g - b) / d,       6.f) / 6.f;
    else if (cmax == g) h =           ((b - r) / d + 2.f)     / 6.f;
    else                h =           ((r - g) / d + 4.f)     / 6.f;
    if (h < 0.f) h += 1.f;
}
static void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b) {
    h = h - std::floor(h);    // normalise 0..1
    float i = std::floor(h * 6.f);
    float f = h * 6.f - i;
    float p = v * (1.f - s), q = v * (1.f - f * s), t2 = v * (1.f - (1.f - f) * s);
    switch (static_cast<int>(i) % 6) {
    case 0: r=v;  g=t2; b=p;  break;
    case 1: r=q;  g=v;  b=p;  break;
    case 2: r=p;  g=v;  b=t2; break;
    case 3: r=p;  g=q;  b=v;  break;
    case 4: r=t2; g=p;  b=v;  break;
    default:r=v;  g=p;  b=q;  break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  compute_seg_centroids — spatial centroid (normalized -1..1) for each segment
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::array<float,2>> compute_seg_centroids(const PointBuffer& buf, int n_segs) {
    std::vector<std::array<float,2>> c(static_cast<size_t>(n_segs), {0.f, 0.f});
    std::vector<int> cnt(static_cast<size_t>(n_segs), 0);
    int seg = -1;
    bool prev = true;
    for (const auto& pt : buf) {
        if (pt.blanked) { prev = true; continue; }
        if (prev) { ++seg; prev = false; }
        if (seg < n_segs) {
            c[static_cast<size_t>(seg)][0] += pt.nx();
            c[static_cast<size_t>(seg)][1] += pt.ny();
            cnt[static_cast<size_t>(seg)]++;
        }
    }
    for (int i = 0; i < n_segs; ++i) {
        if (cnt[static_cast<size_t>(i)] > 0) {
            c[static_cast<size_t>(i)][0] /= static_cast<float>(cnt[static_cast<size_t>(i)]);
            c[static_cast<size_t>(i)][1] /= static_cast<float>(cnt[static_cast<size_t>(i)]);
        }
    }
    return c;
}

// ─────────────────────────────────────────────────────────────────────────────
//  direction_phase — per-object phase offset.
//  segs: N adjacent objects share the same phase (grouping).
//  parts: number of complete wave cycles across the group (multiplies spread).
//  dir_width: overall spread scale (0=sync, 1=full wave).
// ─────────────────────────────────────────────────────────────────────────────
static float direction_phase(FxDirection dir, int seg_idx,
                              const std::vector<std::array<float,2>>& centroids,
                              float dir_width, int parts, int segs) {
    if (dir == FxDirection::Sync || centroids.size() <= 1 || dir_width == 0.f) return 0.f;
    if (seg_idx < 0 || seg_idx >= static_cast<int>(centroids.size())) return 0.f;

    // Grouping: N adjacent objects share the same phase
    const int eff_segs = std::max(1, segs);
    const int eff_idx  = seg_idx / eff_segs;   // effective group index
    // Source centroid: use first object in this group
    const int src = std::min(eff_idx * eff_segs, static_cast<int>(centroids.size()) - 1);
    const float cx = centroids[static_cast<size_t>(src)][0];
    const float cy = centroids[static_cast<size_t>(src)][1];

    float base;
    switch (dir) {
    case FxDirection::Forward:
        base = (cx + 1.f) * 0.5f; break;
    case FxDirection::Backward:
        base = 1.f - (cx + 1.f) * 0.5f; break;
    case FxDirection::CentreOut: {
        float dist = std::sqrt(cx*cx + cy*cy);
        base = dist * 0.7071f; break;
    }
    case FxDirection::CentreIn: {
        float dist = std::sqrt(cx*cx + cy*cy);
        base = 1.f - dist * 0.7071f; break;
    }
    case FxDirection::OddEven:
        base = (eff_idx & 1) ? 0.5f : 0.f; break;
    case FxDirection::Random:
        base = std::fmod(std::abs(std::sin(static_cast<float>(eff_idx) * 127.1f + 311.7f) * 43758.5453f), 1.f); break;
    default:
        return 0.f;
    }
    // parts multiplies wave cycles; fmod keeps result in [0,1) before scaling by dir_width
    const int   p = std::max(1, parts);
    return std::fmod(base * static_cast<float>(p), 1.f) * dir_width;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Frame FX helper — apply FrameFxEntry list to a PointBuffer.
//  Each lit object segment gets a per-segment phase offset based on direction,
//  creating ChamSys-style sliding/chasing effects across objects.
// ─────────────────────────────────────────────────────────────────────────────
static void apply_frame_fx(PointBuffer& buf, const FxLayer& layer, float t) {
    if (layer.fx.empty()) return;
    static constexpr float kScale  = 32767.f;
    static constexpr float kTwoPi  = 6.28318f;

    // Count distinct lit segments via rising-edge (blanked→lit) detection.
    int n_segs = 0;
    {
        bool prev = true;
        for (const auto& pt : buf) {
            if (!pt.blanked && prev) ++n_segs;
            prev = pt.blanked;
        }
    }
    if (n_segs == 0) n_segs = 1;

    // Pre-compute spatial centroids for direction-aware FX
    auto centroids = compute_seg_centroids(buf, n_segs);

    for (const auto& e : layer.fx) {
        if (!e.enabled) continue;
        int  seg  = -1;
        bool prev = true;
        bool prev_clipped = false;
        for (auto& pt : buf) {
            if (pt.blanked) { prev = true; prev_clipped = false; continue; }
            if (prev)       { ++seg; prev = false; }

            // Bug 23: direction_phase() returns 0 when centroids.size()<=1 (all single-segment
            // generators: waves, lines, etc.).  For those cases use the point's own X position
            // as the phase carrier so Spread (dir_width) has a visible effect.
            float dir_off;
            if (n_segs <= 1 && e.direction != FxDirection::Sync && e.dir_width > 0.f) {
                float pos_phase = pt.nx() * 0.5f + 0.5f;  // 0..1 left-to-right
                const int p_m = std::max(1, e.parts);
                dir_off = std::fmod(pos_phase * static_cast<float>(p_m), 1.f) * e.dir_width;
            } else {
                dir_off = direction_phase(e.direction, seg, centroids, e.dir_width, e.parts, e.segs);
            }
            float raw_phase = t * e.rate + e.offset + dir_off;
            float phase     = std::fmod(raw_phase, 1.f);
            if (phase < 0.f) phase += 1.f;
            if (e.width < 0.999f) {
                float gate_val = (e.dir_width > 0.f)
                    ? std::fmod(dir_off / e.dir_width, 1.f)
                    : phase;  // duty-cycle gate for Sync direction
                if (gate_val >= e.width) { pt.blanked = true; prev_clipped = false; continue; }
            }
            float sine_val = std::sin(phase * kTwoPi);
            float cos_val  = std::cos(phase * kTwoPi);
            float nx = pt.nx() * e.size * e.spread;
            float ny = pt.ny() * e.size;
            switch (e.type) {
            case FrameFxType::PanX:
                nx += sine_val * e.depth; break;
            case FrameFxType::PanY:
                ny += sine_val * e.depth; break;
            case FrameFxType::Rotate: {
                float angle = sine_val * e.depth * 3.14159f;
                float cos_a = std::cos(angle), sin_a = std::sin(angle);
                float rx = nx * cos_a - ny * sin_a;
                float ry = nx * sin_a + ny * cos_a;
                nx = rx; ny = ry; break;
            }
            case FrameFxType::Scale: {
                float s = 1.f + sine_val * e.depth;
                if (s < 0.01f) s = 0.01f;
                nx *= s; ny *= s; break;
            }
            case FrameFxType::BounceX:
                nx += std::abs(sine_val) * e.depth; break;
            case FrameFxType::BounceY:
                ny += std::abs(sine_val) * e.depth; break;
            case FrameFxType::ShakeX:
                nx += (det_noise(seg, t * e.rate, 0) * 2.f - 1.f) * e.depth; break;
            case FrameFxType::ShakeY:
                ny += (det_noise(seg, t * e.rate, 1) * 2.f - 1.f) * e.depth; break;
            case FrameFxType::Spiral:
                nx += sine_val * e.depth;
                ny += cos_val  * e.depth; break;
            case FrameFxType::ColorCycle: {
                if (e.use_custom_colors) {
                    // Use col_a as the single-color cycling brightness
                    float pulse = 0.5f + 0.5f * sine_val;
                    pt.r = static_cast<uint8_t>(std::clamp(e.col_a_r * pulse * 255.f, 0.f, 255.f));
                    pt.g = static_cast<uint8_t>(std::clamp(e.col_a_g * pulse * 255.f, 0.f, 255.f));
                    pt.b = static_cast<uint8_t>(std::clamp(e.col_a_b * pulse * 255.f, 0.f, 255.f));
                } else {
                    float hue = phase;
                    float r = std::clamp(std::abs(hue * 6.f - 3.f) - 1.f, 0.f, 1.f);
                    float g = std::clamp(2.f - std::abs(hue * 6.f - 2.f), 0.f, 1.f);
                    float b = std::clamp(2.f - std::abs(hue * 6.f - 4.f), 0.f, 1.f);
                    pt.r = static_cast<uint8_t>(r * 255.f);
                    pt.g = static_cast<uint8_t>(g * 255.f);
                    pt.b = static_cast<uint8_t>(b * 255.f);
                }
                continue;
            }
            case FrameFxType::ColorPulse: {
                if (e.use_custom_colors) {
                    float pulse = 0.5f + 0.5f * sine_val;
                    pt.r = static_cast<uint8_t>(std::clamp(e.col_a_r * pulse * 255.f, 0.f, 255.f));
                    pt.g = static_cast<uint8_t>(std::clamp(e.col_a_g * pulse * 255.f, 0.f, 255.f));
                    pt.b = static_cast<uint8_t>(std::clamp(e.col_a_b * pulse * 255.f, 0.f, 255.f));
                } else {
                    float pulse = 0.5f + 0.5f * sine_val;
                    pt.r = static_cast<uint8_t>(static_cast<float>(pt.r) * pulse);
                    pt.g = static_cast<uint8_t>(static_cast<float>(pt.g) * pulse);
                    pt.b = static_cast<uint8_t>(static_cast<float>(pt.b) * pulse);
                }
                continue;
            }
            case FrameFxType::RainbowTrail: {
                if (e.use_custom_colors) {
                    // Interpolate along the trail between col_a and col_b
                    float t_trail = std::fmod(nx * 0.5f + 0.5f + phase, 1.f);
                    pt.r = static_cast<uint8_t>(std::clamp((e.col_a_r + t_trail * (e.col_b_r - e.col_a_r)) * 255.f, 0.f, 255.f));
                    pt.g = static_cast<uint8_t>(std::clamp((e.col_a_g + t_trail * (e.col_b_g - e.col_a_g)) * 255.f, 0.f, 255.f));
                    pt.b = static_cast<uint8_t>(std::clamp((e.col_a_b + t_trail * (e.col_b_b - e.col_a_b)) * 255.f, 0.f, 255.f));
                } else {
                    float hue = std::fmod(nx * 0.5f + 0.5f + phase, 1.f);
                    float r = std::clamp(std::abs(hue * 6.f - 3.f) - 1.f, 0.f, 1.f);
                    float g = std::clamp(2.f - std::abs(hue * 6.f - 2.f), 0.f, 1.f);
                    float b = std::clamp(2.f - std::abs(hue * 6.f - 4.f), 0.f, 1.f);
                    pt.r = static_cast<uint8_t>(r * 255.f);
                    pt.g = static_cast<uint8_t>(g * 255.f);
                    pt.b = static_cast<uint8_t>(b * 255.f);
                }
                continue;
            }
            case FrameFxType::Col2: {
                if (e.use_custom_colors) {
                    // Cycle A→B→A with crossfade at BOTH transitions (A→B and B→A).
                    // Use the same segment approach as Col3/Col4: divide phase into 2 equal
                    // segments and crossfade at the end of each segment so both the A→B
                    // (phase≈0.5) and B→A (phase≈0/1 wrap) transitions are smooth.
                    // Bug fix: the previous midpoint-only formula only crossfaded A→B and
                    // snapped the B→A wrap transition.
                    float seg_phase2 = phase * 2.f;
                    int   seg_idx2   = static_cast<int>(seg_phase2);
                    if (seg_idx2 >= 2) seg_idx2 = 1;
                    float local_t2   = seg_phase2 - static_cast<float>(seg_idx2);

                    float t_blend;
                    if (e.crossfade > 0.001f) {
                        float half_cf2 = e.crossfade * 0.5f;
                        t_blend = (local_t2 > 1.f - half_cf2)
                            ? std::clamp((local_t2 - (1.f - half_cf2)) / (2.f * half_cf2), 0.f, 1.f)
                            : 0.f;
                    } else {
                        t_blend = 0.f;
                    }
                    // seg 0: A→B; seg 1: B→A
                    float ca_r, ca_g, ca_b, cb_r, cb_g, cb_b;
                    if (seg_idx2 == 0) {
                        ca_r = e.col_a_r; ca_g = e.col_a_g; ca_b = e.col_a_b;
                        cb_r = e.col_b_r; cb_g = e.col_b_g; cb_b = e.col_b_b;
                    } else {
                        ca_r = e.col_b_r; ca_g = e.col_b_g; ca_b = e.col_b_b;
                        cb_r = e.col_a_r; cb_g = e.col_a_g; cb_b = e.col_a_b;
                    }
                    pt.r = static_cast<uint8_t>(std::clamp((ca_r + t_blend * (cb_r - ca_r)) * 255.f, 0.f, 255.f));
                    pt.g = static_cast<uint8_t>(std::clamp((ca_g + t_blend * (cb_g - ca_g)) * 255.f, 0.f, 255.f));
                    pt.b = static_cast<uint8_t>(std::clamp((ca_b + t_blend * (cb_b - ca_b)) * 255.f, 0.f, 255.f));
                } else {
                    // Square-wave between object colour and hue-shifted colour.
                    // depth = hue rotation 0..1 (0.5 = complementary).
                    float h, s, v;
                    rgb_to_hsv(pt.r / 255.f, pt.g / 255.f, pt.b / 255.f, h, s, v);
                    if (phase >= 0.5f) h = h + e.depth;  // hsv_to_rgb normalises h
                    float r, g, b;
                    hsv_to_rgb(h, s, v, r, g, b);
                    pt.r = static_cast<uint8_t>(r * 255.f);
                    pt.g = static_cast<uint8_t>(g * 255.f);
                    pt.b = static_cast<uint8_t>(b * 255.f);
                }
                continue;
            }
            case FrameFxType::Col3: {
                if (e.use_custom_colors) {
                    // Cycle through col_a, col_b, col_c with optional crossfade.
                    // Phase is divided into 3 equal segments of 1/3 each.
                    static constexpr float kSeg3 = 1.f / 3.f;
                    float seg_phase = phase * 3.f;       // 0..3
                    int   seg_idx   = static_cast<int>(seg_phase);  // 0, 1, 2
                    if (seg_idx >= 3) seg_idx = 2;
                    float local_t   = seg_phase - static_cast<float>(seg_idx);  // 0..1 within segment

                    // Color triplet
                    const float* colors_r[3] = { &e.col_a_r, &e.col_b_r, &e.col_c_r };
                    const float* colors_g[3] = { &e.col_a_g, &e.col_b_g, &e.col_c_g };
                    const float* colors_b[3] = { &e.col_a_b, &e.col_b_b, &e.col_c_b };

                    float blend_t;
                    if (e.crossfade > 0.001f) {
                        float half_cf = e.crossfade * 0.5f;  // crossfade zone at end of segment
                        half_cf = std::max(half_cf, 1e-6f);
                        if (local_t < (1.f - half_cf))
                            blend_t = 0.f;
                        else
                            blend_t = (local_t - (1.f - half_cf)) / half_cf;
                    } else {
                        blend_t = 0.f;
                    }
                    int next_idx = (seg_idx + 1) % 3;
                    float cr = *colors_r[seg_idx] + blend_t * (*colors_r[next_idx] - *colors_r[seg_idx]);
                    float cg = *colors_g[seg_idx] + blend_t * (*colors_g[next_idx] - *colors_g[seg_idx]);
                    float cb = *colors_b[seg_idx] + blend_t * (*colors_b[next_idx] - *colors_b[seg_idx]);
                    pt.r = static_cast<uint8_t>(std::clamp(cr * 255.f, 0.f, 255.f));
                    pt.g = static_cast<uint8_t>(std::clamp(cg * 255.f, 0.f, 255.f));
                    pt.b = static_cast<uint8_t>(std::clamp(cb * 255.f, 0.f, 255.f));
                } else {
                    // Snap-cycle through 3 hues spaced 120 degrees (1/3 turn) apart.
                    // depth controls hue step size (default 1/3 of the wheel).
                    float h, s, v;
                    rgb_to_hsv(pt.r / 255.f, pt.g / 255.f, pt.b / 255.f, h, s, v);
                    float step  = std::floor(phase * 3.f) / 3.f;  // 0, 1/3, 2/3
                    h = h + step * e.depth * 3.f;                  // depth scales the spread
                    float r, g, b;
                    hsv_to_rgb(h, s, v, r, g, b);
                    pt.r = static_cast<uint8_t>(r * 255.f);
                    pt.g = static_cast<uint8_t>(g * 255.f);
                    pt.b = static_cast<uint8_t>(b * 255.f);
                }
                continue;
            }
            case FrameFxType::ColFlick: {
                if (e.use_custom_colors) {
                    float noise_t = det_noise(seg, t * e.rate, 0);  // 0..1
                    pt.r = static_cast<uint8_t>(std::clamp((e.col_a_r + noise_t * (e.col_b_r - e.col_a_r)) * 255.f, 0.f, 255.f));
                    pt.g = static_cast<uint8_t>(std::clamp((e.col_a_g + noise_t * (e.col_b_g - e.col_a_g)) * 255.f, 0.f, 255.f));
                    pt.b = static_cast<uint8_t>(std::clamp((e.col_a_b + noise_t * (e.col_b_b - e.col_a_b)) * 255.f, 0.f, 255.f));
                } else {
                    float fr = std::clamp(1.f - det_noise(seg, t * e.rate, 0) * e.depth, 0.f, 1.f);
                    float fg = std::clamp(1.f - det_noise(seg, t * e.rate, 1) * e.depth, 0.f, 1.f);
                    float fb = std::clamp(1.f - det_noise(seg, t * e.rate, 2) * e.depth, 0.f, 1.f);
                    pt.r = static_cast<uint8_t>(pt.r * fr);
                    pt.g = static_cast<uint8_t>(pt.g * fg);
                    pt.b = static_cast<uint8_t>(pt.b * fb);
                }
                continue;
            }
            case FrameFxType::Strobe: {
                // Rhythmic blanking — depth acts as duty cycle (0..1).
                // Point is blanked when phase > depth.
                if (phase > e.depth) pt.blanked = true;
                continue;
            }
            case FrameFxType::Col4: {
                if (e.use_custom_colors) {
                    // Cycle through col_a, col_b, col_c, col_d with optional crossfade.
                    float seg_phase = phase * 4.f;
                    int   seg_idx   = static_cast<int>(seg_phase);
                    if (seg_idx >= 4) seg_idx = 3;
                    float local_t   = seg_phase - static_cast<float>(seg_idx);

                    const float* colors_r[4] = { &e.col_a_r, &e.col_b_r, &e.col_c_r, &e.col_d_r };
                    const float* colors_g[4] = { &e.col_a_g, &e.col_b_g, &e.col_c_g, &e.col_d_g };
                    const float* colors_b[4] = { &e.col_a_b, &e.col_b_b, &e.col_c_b, &e.col_d_b };

                    float blend_t;
                    if (e.crossfade > 0.001f) {
                        float half_cf = e.crossfade * 0.5f;
                        half_cf = std::max(half_cf, 1e-6f);
                        blend_t = (local_t < (1.f - half_cf)) ? 0.f : (local_t - (1.f - half_cf)) / half_cf;
                    } else {
                        blend_t = 0.f;
                    }
                    int next_idx = (seg_idx + 1) % 4;
                    float cr = *colors_r[seg_idx] + blend_t * (*colors_r[next_idx] - *colors_r[seg_idx]);
                    float cg = *colors_g[seg_idx] + blend_t * (*colors_g[next_idx] - *colors_g[seg_idx]);
                    float cb = *colors_b[seg_idx] + blend_t * (*colors_b[next_idx] - *colors_b[seg_idx]);
                    pt.r = static_cast<uint8_t>(std::clamp(cr * 255.f, 0.f, 255.f));
                    pt.g = static_cast<uint8_t>(std::clamp(cg * 255.f, 0.f, 255.f));
                    pt.b = static_cast<uint8_t>(std::clamp(cb * 255.f, 0.f, 255.f));
                } else {
                    // Hue-shift path: 4 equally spaced hues
                    float h, s, v;
                    rgb_to_hsv(pt.r / 255.f, pt.g / 255.f, pt.b / 255.f, h, s, v);
                    float step = std::floor(phase * 4.f) / 4.f;
                    h = h + step * e.depth * 4.f;
                    float r, g, b;
                    hsv_to_rgb(h, s, v, r, g, b);
                    pt.r = static_cast<uint8_t>(r * 255.f);
                    pt.g = static_cast<uint8_t>(g * 255.f);
                    pt.b = static_cast<uint8_t>(b * 255.f);
                }
                continue;
            }
            case FrameFxType::Col5: {
                if (e.use_custom_colors) {
                    // Cycle through col_a, col_b, col_c, col_d, col_e with optional crossfade.
                    float seg_phase = phase * 5.f;
                    int   seg_idx   = static_cast<int>(seg_phase);
                    if (seg_idx >= 5) seg_idx = 4;
                    float local_t   = seg_phase - static_cast<float>(seg_idx);

                    const float* colors_r[5] = { &e.col_a_r, &e.col_b_r, &e.col_c_r, &e.col_d_r, &e.col_e_r };
                    const float* colors_g[5] = { &e.col_a_g, &e.col_b_g, &e.col_c_g, &e.col_d_g, &e.col_e_g };
                    const float* colors_b[5] = { &e.col_a_b, &e.col_b_b, &e.col_c_b, &e.col_d_b, &e.col_e_b };

                    float blend_t;
                    if (e.crossfade > 0.001f) {
                        float half_cf = e.crossfade * 0.5f;
                        half_cf = std::max(half_cf, 1e-6f);
                        blend_t = (local_t < (1.f - half_cf)) ? 0.f : (local_t - (1.f - half_cf)) / half_cf;
                    } else {
                        blend_t = 0.f;
                    }
                    int next_idx = (seg_idx + 1) % 5;
                    float cr = *colors_r[seg_idx] + blend_t * (*colors_r[next_idx] - *colors_r[seg_idx]);
                    float cg = *colors_g[seg_idx] + blend_t * (*colors_g[next_idx] - *colors_g[seg_idx]);
                    float cb = *colors_b[seg_idx] + blend_t * (*colors_b[next_idx] - *colors_b[seg_idx]);
                    pt.r = static_cast<uint8_t>(std::clamp(cr * 255.f, 0.f, 255.f));
                    pt.g = static_cast<uint8_t>(std::clamp(cg * 255.f, 0.f, 255.f));
                    pt.b = static_cast<uint8_t>(std::clamp(cb * 255.f, 0.f, 255.f));
                } else {
                    // Hue-shift path: 5 equally spaced hues
                    float h, s, v;
                    rgb_to_hsv(pt.r / 255.f, pt.g / 255.f, pt.b / 255.f, h, s, v);
                    float step = std::floor(phase * 5.f) / 5.f;
                    h = h + step * e.depth * 5.f;
                    float r, g, b;
                    hsv_to_rgb(h, s, v, r, g, b);
                    pt.r = static_cast<uint8_t>(r * 255.f);
                    pt.g = static_cast<uint8_t>(g * 255.f);
                    pt.b = static_cast<uint8_t>(b * 255.f);
                }
                continue;
            }
            case FrameFxType::RotateContinuous: {
                // Use raw_phase (unwrapped) so rotation never jumps at period boundaries.
                float angle = raw_phase * kTwoPi * e.depth;
                float cos_a = std::cos(angle), sin_a = std::sin(angle);
                float rx = nx * cos_a - ny * sin_a;
                float ry = nx * sin_a + ny * cos_a;
                nx = rx; ny = ry; break;
            }
            }
            // Bug 7: a movement/geometry FX that pushes a LIT point outside the
            // ±1 scan field must BLANK it, not clamp it. Clamping piles every
            // out-of-field point onto the boundary coordinate, so the laser draws
            // a lit ghost line/smear along the edge (the "ghost objects and
            // artifacting" with movement FX). Same class as the v5.01 block_scale
            // fix — blank before the clamp; the clamp below stays harmless.
            if (!pt.blanked && (nx < -1.f || nx > 1.f || ny < -1.f || ny > 1.f)) {
                pt.blanked = true;
                prev_clipped = true;
            } else if (prev_clipped) {
                // Bug (v5.x): the point that EXITS the field is blanked above, but
                // the FIRST in-range point after an out-of-field run is the RE-ENTRY:
                // its `blanked` flag governs the beam for the move from the previous
                // (clamped-to-edge) point back into the field. If left lit, that move
                // draws a ghost line/smear from the field edge — constant with the
                // oscillating Scale FX. Blank the re-entry so the return travel is
                // dark on BOTH sides of the boundary, then clear the carry.
                pt.blanked = true;
                prev_clipped = false;
            }
            pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
            pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
        }
    }
}

// Apply global layer intensity to buf per-segment (direction-aware).
// Replaces the old eval_global_layer() + separate colour-multiplication loop.
// extra_mul: playback fader level (pass 1.0 for programmer layer).
// gfx_smooth: optional 32-element array for per-slot CrossFade low-pass state.
static void apply_global_layer(PointBuffer& buf, const GlobalLayer& gl, float t,
                                float extra_mul = 1.f, float* gfx_smooth = nullptr)
{
    // Fast uniform path when no entry uses direction chase, segs/parts, or width gating
    bool has_dir = false;
    for (const auto& e : gl.fx)
        if (e.enabled && ((e.direction != FxDirection::Sync && e.dir_width > 0.f)
                          || e.parts > 1 || e.segs > 1 || e.width < 0.999f))
            { has_dir = true; break; }

    if (!has_dir) {
        float level = eval_global_layer(gl, t, gfx_smooth) * extra_mul;
        for (auto& pt : buf) {
            if (pt.blanked) continue;
            pt.r = static_cast<uint8_t>(pt.r * level);
            pt.g = static_cast<uint8_t>(pt.g * level);
            pt.b = static_cast<uint8_t>(pt.b * level);
        }
        return;
    }

    // Count lit segments (rising-edge)
    int n_segs = 0;
    { bool prev = true;
      for (const auto& pt : buf) { if (!pt.blanked && prev) ++n_segs; prev = pt.blanked; } }
    if (n_segs == 0) n_segs = 1;

    // Pre-compute spatial centroids for direction-aware global FX
    auto centroids = compute_seg_centroids(buf, n_segs);

    int  seg  = -1;
    bool prev = true;
    for (auto& pt : buf) {
        if (pt.blanked) { prev = true; continue; }
        if (prev)       { ++seg; prev = false; }

        float intensity = gl.global_dim;
        for (int gfi = 0; gfi < static_cast<int>(gl.fx.size()); ++gfi) {
            const auto& e = gl.fx[static_cast<size_t>(gfi)];
            if (!e.enabled) continue;
            float dir_off = direction_phase(e.direction, seg, centroids, e.dir_width, e.parts, e.segs);
            if (e.width < 0.999f) {
                float gate_val = (e.dir_width > 0.f)
                    ? std::fmod(dir_off / e.dir_width, 1.f)
                    : std::fmod(t * e.rate + e.offset + dir_off, 1.f);  // duty-cycle for Sync
                if (gate_val < 0.f) gate_val += 1.f;
                if (gate_val >= e.width) { intensity = 0.f; break; }
            }
            float* sptr = (gfx_smooth && gfi < 32) ? &gfx_smooth[gfi] : nullptr;
            float v = eval_global_fx(e, t, dir_off, seg, sptr);
            switch (e.blend) {
            case FxBlendMode::Add:      intensity = std::clamp(intensity + v, 0.f, 1.f); break;
            case FxBlendMode::Subtract: intensity = std::clamp(intensity - v, 0.f, 1.f); break;
            case FxBlendMode::Absolute: intensity = std::clamp(v,             0.f, 1.f); break;
            case FxBlendMode::Multiply: intensity = std::clamp(intensity * v, 0.f, 1.f); break;
            }
        }
        float c = intensity * extra_mul;
        pt.r = static_cast<uint8_t>(pt.r * c);
        pt.g = static_cast<uint8_t>(pt.g * c);
        pt.b = static_cast<uint8_t>(pt.b * c);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  process_commands — drain the MPSC command queue
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::process_commands()
{
    // Hold project_access_mtx_ for the entire duration of command processing.
    // This prevents snapshot_project_for_save() (UI thread) from copying
    // project_->full_cue_list or project_->playbacks[n].cuelist while a command
    // handler is writing them (e.g. RecordToPlayback, sync_cuelist_to_project).
    std::lock_guard<std::mutex> proj_lk(project_access_mtx_);

    EngineCommand cmd;
    while (cmd_queue_.try_pop(cmd))
    {
        std::visit([this](auto&& c) {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, cmd::Play>) {
                playing_ = true;
                log::debug("Engine: Play");
            }
            else if constexpr (std::is_same_v<T, cmd::Stop>) {
                playing_    = false;
                time_       = 0.0;
                active_cue_ = -1;
                fade_level_ = 0.f;
                // Stop all active playbacks so their output goes dark too.
                for (auto& rs : playback_states_)
                    rs.active = false;
                if (project_) {
                    for (auto& pb : project_->playbacks)
                        pb.active = false;
                }
                log::debug("Engine: Stop");
            }
            else if constexpr (std::is_same_v<T, cmd::Pause>) {
                playing_ = !playing_;
                log::debug("Engine: Pause -> %s", playing_ ? "playing" : "paused");
            }
            else if constexpr (std::is_same_v<T, cmd::SetMasterIntensity>) {
                master_ = std::clamp(c.value, 0.f, 1.f);
            }
            else if constexpr (std::is_same_v<T, cmd::SetPointRate>) {
                point_rate_ = std::clamp(c.pps, kMinPointRate, kMaxPointRate);
            }
            else if constexpr (std::is_same_v<T, cmd::SetBPM>) {
                bpm_ = std::clamp(c.bpm, 20.f, 2000.f);
            }
            else if constexpr (std::is_same_v<T, cmd::ActivateCue>) {
                if (project_ && c.slot >= 0
                    && c.slot < static_cast<int>(project_->cues.size()))
                {
                    active_cue_ = c.slot;
                    time_       = 0.0;
                    fade_level_ = (c.fade_time <= 0.f) ? 1.f : 0.f;
                    log::debug("Engine: ActivateCue slot=%d", c.slot);
                    const std::string& cue_name = project_->cues[static_cast<size_t>(c.slot)].name;
                    record_event_to_armed_timelines(TimelineEventType::CueGo,
                                                    std::to_string(c.slot), {},
                                                    cue_name.empty()
                                                        ? "Cue " + std::to_string(c.slot + 1)
                                                        : cue_name);
                }
            }
            else if constexpr (std::is_same_v<T, cmd::DeactivateCue>) {
                if (active_cue_ == c.slot) {
                    active_cue_ = -1;
                    fade_level_ = 0.f;
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetGeneratorParam>) {
                if (!project_) return;
                if (c.cue_slot < 0
                    || c.cue_slot >= static_cast<int>(project_->cues.size())) return;
                GeneratorParams& p = project_->cues[c.cue_slot].params;
                const std::string& n = c.param_name;
                if      (n == "speed")     p.speed     = c.value;
                else if (n == "scale")     p.scale     = c.value;
                else if (n == "density")   p.density   = c.value;
                else if (n == "param_a")   p.param_a   = c.value;
                else if (n == "param_b")   p.param_b   = c.value;
                else if (n == "param_c")   p.param_c   = c.value;
                else if (n == "rotation")  p.rotation  = c.value;
                else if (n == "pan")       p.pan       = c.value;
                else if (n == "tilt")      p.tilt      = c.value;
                else if (n == "zoom")      p.zoom      = c.value;
                else if (n == "intensity") p.intensity = c.value;
                else                       p.extra_params[n] = c.value;
            }
            else if constexpr (std::is_same_v<T, cmd::LoadProject>) {
                project_ = c.project;
                active_cue_ = -1;
                time_       = 0.0;
                playing_    = false;
                log::info("Engine: project loaded '%s'",
                          project_ ? project_->name.c_str() : "<null>");

                // Build CueList — prefer full_cue_list when present.
                cue_list_ = CueList{};
                if (project_) {
                    if (!project_->full_cue_list.empty()) {
                        // Prefer full cue list if available.
                        for (auto& fe : project_->full_cue_list)
                            cue_list_.add_entry(fe);
                    } else {
                        // Fall back to legacy CueListEntry conversion.
                        for (int ei = 0; ei < static_cast<int>(project_->cue_list.size()); ++ei) {
                            const auto& entry = project_->cue_list[static_cast<size_t>(ei)];
                            FullCueEntry fe;
                            fe.cue_id = entry.cue_id;
                            int ci = project_->find_cue(entry.cue_id);
                            fe.name   = (ci >= 0) ? project_->cues[static_cast<size_t>(ci)].name : "Cue";
                            fe.timing.fade_in  = entry.fade_in;
                            fe.timing.fade_out = entry.fade_out;
                            fe.timing.hold     = (float)(entry.out_time - entry.in_time);
                            fe.trigger.type    = entry.auto_next ? TriggerType::Follow : TriggerType::Halt;
                            fe.trigger.time_s  = (float)entry.auto_next_delay;
                            fe.number.major    = ei + 1;
                            fe.number.minor    = 0;
                            cue_list_.add_entry(fe);
                        }
                    }

                    // Initialize one FxEngine per cue slot
                    cue_fx_engines_.resize(project_->cues.size());

                    // Sync full_cue_list to reflect what was actually loaded.
                    sync_cuelist_to_project();

                    // Pre-create 40 playback slots so UI cards always have valid IDs
                    if (project_->playbacks.empty()) {
                        for (int i = 1; i <= 40; ++i) {
                            PlaybackDef pb;
                            pb.id   = i;
                            pb.name = "PB " + std::to_string(i);
                            project_->playbacks.push_back(std::move(pb));
                        }
                    }
                    // Rebuild run-states from the playback defs.
                    // Safety: active is forced to false regardless of what was
                    // deserialized — a laser show must never auto-start on load.
                    playback_states_.clear();
                    next_playback_id_ = 1;
                    for (auto& pb_def : project_->playbacks) {
                        pb_def.active = false;  // belt-and-suspenders: clear in the def too
                        PlaybackRunState rs;
                        rs.id        = pb_def.id;
                        rs.intensity = pb_def.intensity;
                        rs.active    = false;
                        playback_states_.push_back(rs);
                        if (pb_def.id >= next_playback_id_)
                            next_playback_id_ = pb_def.id + 1;
                    }

                    // Reset engine-mode flags — programmer and record mode must
                    // not carry over from the previous session.
                    programmer_active_ = false;
                    record_mode_       = false;
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetBeamThickness>) {
                beam_thickness_.store(c.px, std::memory_order_relaxed);
            }
            else if constexpr (std::is_same_v<T, cmd::SetBloomRadius>) {
                bloom_radius_.store(c.px, std::memory_order_relaxed);
            }
            else if constexpr (std::is_same_v<T, cmd::SetHazeDensity>) {
                haze_density_.store(c.d, std::memory_order_relaxed);
            }
            else if constexpr (std::is_same_v<T, cmd::SetExposure>) {
                exposure_.store(c.e, std::memory_order_relaxed);
            }
            else if constexpr (std::is_same_v<T, cmd::Seek>) {
                time_ = std::max(0.0, c.time_s);
                log::debug("Engine: Seek -> %.3f s", time_);
            }
            else if constexpr (std::is_same_v<T, cmd::GoNext>) {
                // H-3: use cue_list_ (the live state machine) instead of
                // project_->cue_list (the legacy array that may be out of sync
                // after add/delete/move operations).
                int cnt = cue_list_.entry_count();
                if (cnt > 0) {
                    int next = cue_list_idx_ + 1;
                    if (next < cnt) {
                        cue_list_idx_ = next;
                        const auto& fe = cue_list_.entry_at(next);
                        int ci = project_ ? project_->find_cue(fe.cue_id) : -1;
                        if (ci >= 0) { active_cue_ = ci; time_ = 0.0; fade_level_ = 0.0f; }
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::GoPrev>) {
                // H-3: same fix — use cue_list_ not project_->cue_list
                int cnt = cue_list_.entry_count();
                if (cnt > 0 && cue_list_idx_ > 0) {
                    int prev = cue_list_idx_ - 1;
                    cue_list_idx_ = prev;
                    const auto& fe = cue_list_.entry_at(prev);
                    int ci = project_ ? project_->find_cue(fe.cue_id) : -1;
                    if (ci >= 0) { active_cue_ = ci; time_ = 0.0; fade_level_ = 0.0f; }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetLoopEnd>) {
                loop_end_ = std::max(1.0, c.time_s);
            }
            else if constexpr (std::is_same_v<T, cmd::CueListGo>) {
                cue_list_.go();
                record_event_to_armed_timelines(TimelineEventType::CueGo, "", {}, "Go");
            }
            else if constexpr (std::is_same_v<T, cmd::CueListBack>) {
                cue_list_.back();
                record_event_to_armed_timelines(TimelineEventType::CueGo, "back", {}, "Back");
            }
            else if constexpr (std::is_same_v<T, cmd::CueListJump>) {
                CueNumber target{ c.major, c.minor };
                cue_list_.jump(target);
                record_event_to_armed_timelines(TimelineEventType::CueGo,
                    std::to_string(c.major) + "." + std::to_string(c.minor), {},
                    "Jump " + std::to_string(c.major));
            }
            else if constexpr (std::is_same_v<T, cmd::SetFxParam>) {
                if (c.cue_idx >= 0 && c.cue_idx < static_cast<int>(cue_fx_engines_.size())) {
                    auto* blk = cue_fx_engines_[c.cue_idx].block_at(c.fx_slot);
                    if (blk) {
                        auto* p = blk->param(c.param);
                        if (p) p->base_value = c.value;
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetFxEnabled>) {
                if (c.cue_idx >= 0 && c.cue_idx < static_cast<int>(cue_fx_engines_.size())) {
                    auto* blk = cue_fx_engines_[c.cue_idx].block_at(c.fx_slot);
                    if (blk) blk->enabled = c.enabled;
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetFxBypassed>) {
                if (c.cue_idx >= 0 && c.cue_idx < static_cast<int>(cue_fx_engines_.size())) {
                    auto* blk = cue_fx_engines_[c.cue_idx].block_at(c.fx_slot);
                    if (blk) blk->bypassed = c.bypassed;
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetFxWet>) {
                if (c.cue_idx >= 0 && c.cue_idx < static_cast<int>(cue_fx_engines_.size())) {
                    auto* blk = cue_fx_engines_[c.cue_idx].block_at(c.fx_slot);
                    if (blk) blk->wet = c.wet;
                }
            }
            else if constexpr (std::is_same_v<T, cmd::TriggerMacro>) {
                // Stub: macro slots are storage-only in this version.
                log::debug("Engine: TriggerMacro slot=%d (stub)", c.slot);
            }
            else if constexpr (std::is_same_v<T, cmd::SetRecordMode>) {
                record_mode_ = c.active;
                log::debug("Engine: SetRecordMode -> %s", record_mode_ ? "on" : "off");
            }
            else if constexpr (std::is_same_v<T, cmd::RecordCue>) {
                // Snapshot current active params into a new Project::Cue + FullCueEntry.
                FullCueEntry fe;
                int n = cue_list_.entry_count();
                fe.number.major = n + 1;
                fe.number.minor = 0;
                fe.name         = c.name;
                fe.timing.fade_in  = c.fade_in;
                fe.timing.fade_out = c.fade_out;
                fe.trigger.type = TriggerType::Halt;

                // Create a matching Project::Cue so the generator path can render it.
                // Clone from the active cue if one is live; otherwise create a blank.
                if (project_) {
                    Cue new_cue;
                    new_cue.id   = Project::new_id();
                    new_cue.name = c.name;
                    if (active_cue_ >= 0
                        && active_cue_ < static_cast<int>(project_->cues.size()))
                    {
                        // Inherit the generator type and current param state.
                        const Cue& src = project_->cues[static_cast<size_t>(active_cue_)];
                        new_cue.generator = src.generator;
                        new_cue.params    = src.params;
                        new_cue.ilda_path = src.ilda_path;
                        new_cue.color_tag = src.color_tag;
                        // Apply any live evaluated params (DMX/automation overrides).
                        if (evaluated_params_valid_)
                            new_cue.params = evaluated_params_;
                        // Capture params as overrides so CueList LTP tracking works.
                        const GeneratorParams& p = new_cue.params;
                        fe.param_overrides = {
                            {"speed",     p.speed},
                            {"scale",     p.scale},
                            {"density",   p.density},
                            {"param_a",   p.param_a},
                            {"param_b",   p.param_b},
                            {"param_c",   p.param_c},
                            {"rotation",  p.rotation},
                            {"pan",       p.pan},
                            {"tilt",      p.tilt},
                            {"zoom",      p.zoom},
                            {"intensity", p.intensity},
                        };
                    }
                    fe.cue_id = new_cue.id;
                    int new_idx = static_cast<int>(project_->cues.size());
                    project_->cues.push_back(std::move(new_cue));
                    // Expand the per-cue FX engine vector to stay in sync.
                    cue_fx_engines_.resize(project_->cues.size());
                    log::debug("Engine: RecordCue created Project::Cue idx=%d id='%s'",
                               new_idx, fe.cue_id.c_str());
                }

                cue_list_.add_entry(std::move(fe));
                sync_cuelist_to_project();
                log::debug("Engine: RecordCue '%s' (total %d)",
                           c.name.c_str(), cue_list_.entry_count());
            }
            else if constexpr (std::is_same_v<T, cmd::UpdateCue>) {
                if (c.idx >= 0 && c.idx < cue_list_.entry_count()) {
                    FullCueEntry& fe = cue_list_.entry_at(c.idx);
                    // Update the named param in param_overrides.
                    bool found = false;
                    for (auto& kv : fe.param_overrides) {
                        if (kv.first == c.param) { kv.second = c.value; found = true; break; }
                    }
                    if (!found)
                        fe.param_overrides.emplace_back(c.param, c.value);
                }
            }
            else if constexpr (std::is_same_v<T, cmd::DeleteCue>) {
                if (c.idx >= 0 && c.idx < cue_list_.entry_count()) {
                    // Save cue_id before removing so we can remove from project_->cues too.
                    // project_->cues and cue_list_ must stay positionally in sync — the UI
                    // uses project_->cues[i].index as a direct index into full_cue_list.
                    std::string cue_id = cue_list_.entry_at(c.idx).cue_id;
                    cue_list_.remove_entry(c.idx);
                    if (project_ && !cue_id.empty()) {
                        auto& cv = project_->cues;
                        // H-4: record the position of the cue BEFORE erasing so
                        // we erase the matching FxEngine slot, not the last one.
                        // The old resize(cv.size()) only truncated from the end,
                        // leaving a stale engine in the middle of the vector.
                        int erase_pos = -1;
                        for (int i = 0; i < static_cast<int>(cv.size()); ++i) {
                            if (cv[i].id == cue_id) { erase_pos = i; break; }
                        }
                        cv.erase(std::remove_if(cv.begin(), cv.end(),
                            [&](const Cue& cue){ return cue.id == cue_id; }),
                            cv.end());
                        // Erase the corresponding FX engine slot at the same position.
                        if (erase_pos >= 0 && erase_pos < static_cast<int>(cue_fx_engines_.size()))
                            cue_fx_engines_.erase(cue_fx_engines_.begin() + erase_pos);
                        else if (cue_fx_engines_.size() > cv.size())
                            cue_fx_engines_.resize(cv.size());
                    }
                    cue_list_idx_ = cue_list_.current_idx();
                    sync_cuelist_to_project();
                    log::debug("Engine: DeleteCue idx=%d", c.idx);
                }
            }
            else if constexpr (std::is_same_v<T, cmd::InsertCue>) {
                // Insert after after_idx by manipulating add then move.
                // Simplest: append then move to desired position.
                cue_list_.add_entry(c.entry); // adds sorted by number; we rely on caller setting number
                // If caller set number correctly, add_entry will sort it into place.
                // No further move needed for MagicQ-number-sorted lists.
                sync_cuelist_to_project();
                log::debug("Engine: InsertCue after=%d", c.after_idx);
            }
            else if constexpr (std::is_same_v<T, cmd::RenameCue>) {
                if (c.idx >= 0 && c.idx < cue_list_.entry_count()) {
                    cue_list_.entry_at(c.idx).name = c.name;
                    sync_cuelist_to_project();
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetCueTiming>) {
                if (c.idx >= 0 && c.idx < cue_list_.entry_count()) {
                    CueTimingBlock& t = cue_list_.entry_at(c.idx).timing;
                    t.fade_in  = c.fade_in;
                    t.fade_out = c.fade_out;
                    t.delay_in = c.delay_in;
                    t.hold     = c.hold;
                    sync_cuelist_to_project();
                }
            }
            else if constexpr (std::is_same_v<T, cmd::MoveCue>) {
                if (c.from_idx >= 0 && c.from_idx < cue_list_.entry_count() &&
                    c.to_idx   >= 0 && c.to_idx   < cue_list_.entry_count()) {
                    cue_list_.move_entry(c.from_idx, c.to_idx);
                    sync_cuelist_to_project();
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetZone>) {
                Zone* existing = zone_manager_.get_zone(c.zone.id);
                if (existing)
                    *existing = c.zone;
                else
                {
                    // Add as a brand-new zone (id already set by caller).
                    zone_manager_.zones().push_back(c.zone);
                }
                log::debug("Engine: SetZone id=%d name='%s'",
                           c.zone.id, c.zone.name.c_str());
            }
            else if constexpr (std::is_same_v<T, cmd::RemoveZone>) {
                zone_manager_.remove_zone(c.id);
                log::debug("Engine: RemoveZone id=%d", c.id);
            }
            else if constexpr (std::is_same_v<T, cmd::BamPaint>) {
                auto& grid = safety_.bam.grid();
                int half = c.brush_size / 2;
                for (int dr = -half; dr <= half; ++dr)
                    for (int dc = -half; dc <= half; ++dc) {
                        int r = c.row + dr, col2 = c.col + dc;
                        if (r >= 0 && r < 64 && col2 >= 0 && col2 < 64)
                            grid.cells[r][col2] = c.val;
                    }
                safety_.bam.rebuild_cache();
            }
            else if constexpr (std::is_same_v<T, cmd::BamClear>) {
                safety_.bam.grid().clear();
                safety_.bam.rebuild_cache();
            }
            else if constexpr (std::is_same_v<T, cmd::BamSetEnabled>) {
                safety_.bam.enabled = c.enabled;
            }
            else if constexpr (std::is_same_v<T, cmd::ResetScanFail>) {
                safety_.scan_fail.reset();
                log::info("Engine: scan-fail latch cleared by operator");
            }
            else if constexpr (std::is_same_v<T, cmd::SetScanFailEnabled>) {
                ScanFailMonitor::Config cfg = safety_.scan_fail.config();
                cfg.enabled = c.enabled;
                safety_.scan_fail.configure(cfg);
                // Do NOT reset() the latch here — a triggered scan-fail must be
                // acknowledged by the operator via cmd::ResetScanFail, not silently
                // cleared by toggling the enable switch (IEC 60825-1 requirement).
                log::info("Engine: scan-fail protection %s", c.enabled ? "enabled" : "disabled");
            }
            // ── Quick Show commands ───────────────────────────────────────────
            else if constexpr (std::is_same_v<T, cmd::QuickShowTrigger>) {
                if (c.page >= 0 && c.page < 32 &&
                    c.row  >= 0 && c.row  < 6  &&
                    c.col  >= 0 && c.col  < 10)
                {
                    int ci = quickshow_[c.page][c.row][c.col].cue_idx;
                    if (ci >= 0) {
                        // Jump to cue in cue list if we can find it by index
                        if (ci < cue_list_.entry_count()) {
                            const FullCueEntry& fe = cue_list_.entry_at(ci);
                            cue_list_.jump(fe.number);
                        } else {
                            cue_list_.go();
                        }
                        log::debug("Engine: QuickShowTrigger page=%d row=%d col=%d cue_idx=%d",
                                   c.page, c.row, c.col, ci);
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::QuickShowAssign>) {
                if (c.page >= 0 && c.page < 32 &&
                    c.row  >= 0 && c.row  < 6  &&
                    c.col  >= 0 && c.col  < 10)
                {
                    quickshow_[c.page][c.row][c.col].cue_idx = c.cue_idx;
                    log::debug("Engine: QuickShowAssign page=%d row=%d col=%d cue_idx=%d",
                               c.page, c.row, c.col, c.cue_idx);
                }
            }
            else if constexpr (std::is_same_v<T, cmd::QuickShowClear>) {
                if (c.page >= 0 && c.page < 32 &&
                    c.row  >= 0 && c.row  < 6  &&
                    c.col  >= 0 && c.col  < 10)
                {
                    quickshow_[c.page][c.row][c.col].cue_idx = -1;
                }
            }
            // ── LivePRO commands ─────────────────────────────────────────────
            else if constexpr (std::is_same_v<T, cmd::LiveProXY>) {
                // Map x/y (-1..1) to pan/tilt on the active cue
                if (project_ && active_cue_ >= 0
                    && active_cue_ < static_cast<int>(project_->cues.size()))
                {
                    GeneratorParams& p = project_->cues[active_cue_].params;
                    p.pan  = std::clamp(c.x, -1.f, 1.f);
                    p.tilt = std::clamp(c.y, -1.f, 1.f);
                }
            }
            else if constexpr (std::is_same_v<T, cmd::LiveProScale>) {
                if (project_ && active_cue_ >= 0
                    && active_cue_ < static_cast<int>(project_->cues.size()))
                {
                    project_->cues[active_cue_].params.scale =
                        std::clamp(c.scale, 0.f, 4.f);
                }
            }
            else if constexpr (std::is_same_v<T, cmd::LiveProSpeed>) {
                if (project_ && active_cue_ >= 0
                    && active_cue_ < static_cast<int>(project_->cues.size()))
                {
                    project_->cues[active_cue_].params.speed =
                        std::clamp(c.speed, 0.f, 8.f);
                }
            }
            else if constexpr (std::is_same_v<T, cmd::LiveProBeat>) {
                // Beat trigger: advance cue list on each beat hit
                cue_list_.go();
                log::debug("Engine: LiveProBeat slot=%d", c.slot);
            }
            // ── MIDI learn commands ───────────────────────────────────────────
            else if constexpr (std::is_same_v<T, cmd::SetMidiBinding>) {
                midi_map_.add_binding(c.binding);
            }
            else if constexpr (std::is_same_v<T, cmd::RemoveMidiBinding>) {
                midi_map_.remove_binding(c.target);
            }
            else if constexpr (std::is_same_v<T, cmd::ClearMidiBindings>) {
                midi_map_.clear();
            }
            // ── Palette selection commands ────────────────────────────────────
            else if constexpr (std::is_same_v<T, cmd::SetActiveColor>) {
                if (c.slot >= -1 && c.slot < ColorPalette::kSlots)
                    active_color_slot_ = c.slot;
            }
            else if constexpr (std::is_same_v<T, cmd::SetActivePosition>) {
                if (c.slot >= -1 && c.slot < PositionPalette::kSlots)
                    active_position_slot_ = c.slot;
            }
            else if constexpr (std::is_same_v<T, cmd::PatchDmxChannel>) {
                // c.channel is 1-based (1..512) matching the UI DragInt range.
                // Convert to 0-based array index; validate as 1-based.
                if (c.universe >= 0 && c.universe < kMaxUniverses &&
                    c.channel >= 1 && c.channel <= 512)
                    dmx_[c.universe][c.channel - 1] = c.value;
            }
            else if constexpr (std::is_same_v<T, cmd::SetArtNetOutput>) {
                artnet_out_.set_target(c.ip);
                artnet_out_universe_offset_ = c.universe_offset;
                if (c.enabled && !artnet_out_.is_open()) {
                    artnet_out_.open();
                } else if (!c.enabled) {
                    artnet_out_.close();
                }
                artnet_out_enabled_ = c.enabled;
                log::info("Engine: ArtNet output %s target=%s offset=%d",
                          c.enabled ? "enabled" : "disabled",
                          c.ip.c_str(), c.universe_offset);
            }
            else if constexpr (std::is_same_v<T, cmd::UpdateCueListEntry>) {
                if (c.idx >= 0 && c.idx < cue_list_.entry_count()) {
                    cue_list_.entry_at(c.idx) = c.entry;
                    // Reset chaser state if we're editing the active chaser
                    if (c.idx == active_cue_ && chaser_run_.has_value())
                        chaser_run_.reset();
                    sync_cuelist_to_project();
                }
            }
            else if constexpr (std::is_same_v<T, cmd::NewChaserCue>) {
                if (!project_) return;
                // Create a new FullCueEntry that is a chaser
                FullCueEntry fce;
                fce.name       = c.name;
                fce.is_chaser  = true;
                fce.chaser_global_hold  = 0.5f;
                fce.chaser_global_xfade = 0.0f;
                fce.chaser_beat_sync    = false;
                fce.chaser_beat_div     = 4;
                // Also create a matching project Cue entry (using beams as placeholder)
                Cue pe;
                pe.id        = Project::new_id();
                pe.name      = c.name;
                pe.generator = GeneratorType::Beams;
                pe.color_tag = 0xFF00FF80u;
                int new_idx = static_cast<int>(project_->cues.size());
                project_->cues.push_back(pe);
                fce.cue_id = project_->cues[static_cast<size_t>(new_idx)].id;
                fce.number.major = cue_list_.entry_count() + 1;
                fce.number.minor = 0;
                cue_list_.add_entry(fce);
                // Also resize fx engines
                cue_fx_engines_.resize(project_->cues.size());
                sync_cuelist_to_project();
            }
            // ── Playback management commands ──────────────────────────────────
            else if constexpr (std::is_same_v<T, cmd::NewPlayback>) {
                if (!project_) return;
                PlaybackDef pb;
                pb.id   = next_playback_id_++;
                pb.name = c.name;
                project_->playbacks.push_back(pb);
                PlaybackRunState rs;
                rs.id = pb.id;
                playback_states_.push_back(rs);
                sync_cuelist_to_project();
            }
            else if constexpr (std::is_same_v<T, cmd::DeletePlayback>) {
                auto it = std::find_if(project_->playbacks.begin(), project_->playbacks.end(),
                    [&](const PlaybackDef& p){ return p.id == c.id; });
                if (it != project_->playbacks.end()) project_->playbacks.erase(it);
                auto it2 = std::find_if(playback_states_.begin(), playback_states_.end(),
                    [&](const PlaybackRunState& s){ return s.id == c.id; });
                if (it2 != playback_states_.end()) playback_states_.erase(it2);
            }
            else if constexpr (std::is_same_v<T, cmd::RenamePlayback>) {
                auto* pb = find_playback_def(c.id);
                if (pb) pb->name = c.name;
            }
            else if constexpr (std::is_same_v<T, cmd::SetPlaybackGo>) {
                auto* rs = find_playback_state(c.id);
                auto* pb = find_playback_def(c.id);
                if (rs && pb) {
                    const bool was_active = rs->active;
                    rs->active = true;
                    pb->active = true;
                    // Auto-fire the first cue when activating a previously-idle playback
                    // that has a non-empty cue list and no cue currently selected.
                    if (!was_active && rs->current_idx < 0 && !pb->cuelist.empty()) {
                        rs->current_idx = 0;
                        rs->prev_idx    = -1;
                        // Always respect cue's fade_in on first trigger; first_trigger_fade_s overrides
                        const float configured_fade = pb->cuelist[0].timing.fade_in;
                        float first_fade = configured_fade;
                        if (pb->config.first_trigger_fade_s > 0.f)
                            first_fade = pb->config.first_trigger_fade_s;
                        rs->fade_in_dur  = first_fade;
                        rs->fade_elapsed = 0.f;
                        rs->fade_alpha   = 0.f;
                        rs->prev_alpha   = 0.f;
                        rs->fade_start   = time_;
                        rs->hold_start   = -1.0;
                        rs->first_trigger_done = true;
                    }
                } else {
                    if (rs) rs->active = true;
                    if (pb) pb->active = true;
                }
                if (rs && pb)
                    record_event_to_armed_timelines(TimelineEventType::PlaybackGo,
                                                    std::to_string(c.id), {},
                                                    "PB" + std::to_string(c.id) + " Go");
            }
            else if constexpr (std::is_same_v<T, cmd::SetPlaybackStop>) {
                auto* rs = find_playback_state(c.id);
                auto* pb = find_playback_def(c.id);
                if (rs) {
                    rs->active        = false;
                    rs->manual_active = false;  // manual Stop clears manual override
                    rs->first_trigger_done = false;
                    // Reset cuelist position unless "remember position" is enabled
                    if (pb && !pb->config.remember_cuelist_position)
                        rs->current_idx = -1;
                }
                if (pb) pb->active = false;
                record_event_to_armed_timelines(TimelineEventType::PlaybackRelease,
                                                std::to_string(c.id), {},
                                                "PB" + std::to_string(c.id) + " Release");
            }
            else if constexpr (std::is_same_v<T, cmd::SetPlaybackClear>) {
                auto* rs = find_playback_state(c.id);
                if (rs) { rs->current_idx = -1; rs->active = false; rs->manual_active = false; rs->first_trigger_done = false; }
                auto* pb = find_playback_def(c.id);
                if (pb) pb->active = false;
            }
            else if constexpr (std::is_same_v<T, cmd::SetPlaybackBlind>) {
                auto* rs = find_playback_state(c.id);
                if (rs) rs->blind = c.blind;
                auto* pb = find_playback_def(c.id);
                if (pb) pb->blind = c.blind;
            }
            else if constexpr (std::is_same_v<T, cmd::SetPlaybackIntensity>) {
                auto* rs = find_playback_state(c.id);
                if (rs) rs->intensity = c.intensity;
                auto* pb = find_playback_def(c.id);
                if (pb) pb->intensity = c.intensity;
                record_event_to_armed_timelines(TimelineEventType::SetLevel,
                                                std::to_string(c.id),
                                                {{"intensity", c.intensity}},
                                                "PB" + std::to_string(c.id) + " Level");
            }
            else if constexpr (std::is_same_v<T, cmd::SetPlaybackCueGo>) {
                auto* rs = find_playback_state(c.id);
                auto* pb = find_playback_def(c.id);
                if (rs && pb && !pb->cuelist.empty()) {
                    // Manual GO press — mark as manually activated so DMX cannot
                    // deactivate this playback while the operator wants it running.
                    rs->manual_active = true;
                    if (!rs->active) {
                        // Not active: activate and jump to cue 0 (or resume at current position)
                        if (rs->current_idx < 0)
                            rs->current_idx = 0;
                        {
                            // Always respect cue's fade_in on first trigger; first_trigger_fade_s overrides
                            const float configured_fade =
                                pb->cuelist[static_cast<size_t>(rs->current_idx)].timing.fade_in;
                            float first_fade = configured_fade;
                            if (pb->config.first_trigger_fade_s > 0.f)
                                first_fade = pb->config.first_trigger_fade_s;
                            rs->fade_in_dur = first_fade;
                        }
                        rs->prev_idx     = -1;
                        rs->fade_elapsed = 0.f;
                        rs->fade_alpha   = 0.f;
                        rs->prev_alpha   = 0.f;
                        rs->fade_start   = time_;
                        rs->hold_start   = -1.0;
                        rs->first_trigger_done = true;
                        rs->active       = true;
                        pb->active       = true;
                    } else {
                        // Already active: advance to the next cue (MagicQ/Titan GO behavior).
                        // STOP is the correct way to deactivate; GO always advances.
                        int old_idx  = rs->current_idx;
                        int next_idx = old_idx + 1;
                        if (next_idx >= static_cast<int>(pb->cuelist.size())) {
                            if (pb->config.end_behavior == PlaybackConfig::EndBehavior::Loop)
                                next_idx = 0;
                            else
                                next_idx = old_idx; // stay on last cue
                        }
                        if (next_idx != old_idx) {
                            float old_fo = pb->cuelist[static_cast<size_t>(old_idx)].timing.fade_out;
                            rs->prev_idx    = old_idx;
                            rs->current_idx = next_idx;
                            rs->fade_in_dur = std::max(pb->cuelist[static_cast<size_t>(next_idx)].timing.fade_in, old_fo);
                            rs->fade_elapsed = 0.f;
                            rs->fade_alpha   = 0.f;
                            rs->prev_alpha   = 1.f;
                            rs->fade_start   = time_;
                            rs->hold_start   = -1.0;
                        }
                        // If already on the last cue and not looping, GO is a no-op
                        // (consistent with MagicQ halting at end of list)
                    }
                }
                record_event_to_armed_timelines(TimelineEventType::PlaybackGo,
                                                std::to_string(c.id), {},
                                                "PB" + std::to_string(c.id) + " Go");
            }
            else if constexpr (std::is_same_v<T, cmd::SetPlaybackCueBack>) {
                auto* rs = find_playback_state(c.id);
                auto* pb = find_playback_def(c.id);
                if (rs && pb && !pb->cuelist.empty()) {
                    int old_idx  = rs->current_idx;
                    int prev_idx = old_idx - 1;
                    if (prev_idx < 0)
                        prev_idx = 0; // clamp at first cue (MagicQ behaviour: stop at cue 1)
                    if (prev_idx != old_idx) {
                        // Set up crossfade from old cue to previous cue
                        float old_fade_out = (old_idx >= 0 && old_idx < (int)pb->cuelist.size())
                                             ? pb->cuelist[old_idx].timing.fade_out : 0.f;
                        rs->prev_idx     = old_idx;
                        rs->current_idx  = prev_idx;
                        rs->fade_in_dur  = std::max(pb->cuelist[static_cast<size_t>(prev_idx)].timing.fade_in, old_fade_out);
                        rs->fade_elapsed = 0.f;
                        rs->fade_alpha   = 0.f;
                        rs->prev_alpha   = 1.f;
                        rs->fade_start   = time_;
                        rs->hold_start   = -1.0;
                        rs->active = true;
                        pb->active = true;
                    }
                }
                record_event_to_armed_timelines(TimelineEventType::PlaybackGo,
                                                std::to_string(c.id), {},
                                                "PB" + std::to_string(c.id) + " Back");
            }
            else if constexpr (std::is_same_v<T, cmd::SetPlaybackJump>) {
                auto* rs = find_playback_state(c.id);
                auto* pb = find_playback_def(c.id);
                if (rs && pb) {
                    CueNumber target{ c.cue_major, c.cue_minor };
                    for (int i = 0; i < static_cast<int>(pb->cuelist.size()); ++i) {
                        if (pb->cuelist[static_cast<size_t>(i)].number == target) {
                            rs->current_idx  = i;
                            rs->prev_idx     = -1;
                            rs->fade_in_dur  = pb->cuelist[static_cast<size_t>(i)].timing.fade_in;
                            rs->fade_elapsed = 0.f;
                            rs->fade_alpha   = 0.f;
                            rs->hold_start   = -1.0;
                            break;
                        }
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::RecordToPlayback>) {
                auto* pb = find_playback_def(c.playback_id);
                if (pb) {
                    bool first_cue = pb->cuelist.empty();
                    FullCueEntry fce = c.entry;
                    fce.number.major = static_cast<int>(pb->cuelist.size()) + 1;
                    fce.number.minor = 0;
                    // Merge latched per-stream KF from deselected streams into the cue.
                    // UI-programmed content (already in fce.per_stream_kf) takes priority.
                    for (const auto& [sid, sp] : stream_prog_) {
                        if (!sp.objects.objects.empty())
                            fce.per_stream_kf.try_emplace(sid, sp.objects);
                    }
                    pb->cuelist.push_back(fce);
                    // Pre-select first cue so it shows in the UI without requiring a GO
                    if (first_cue) {
                        auto* rs = find_playback_state(c.playback_id);
                        if (rs) rs->current_idx = 0;
                    }
                    sync_cuelist_to_project();
                }
            }
            else if constexpr (std::is_same_v<T, cmd::UpdatePlaybackCue>) {
                auto* pb = find_playback_def(c.playback_id);
                if (pb && c.cue_idx >= 0 && c.cue_idx < static_cast<int>(pb->cuelist.size())) {
                    pb->cuelist[static_cast<size_t>(c.cue_idx)] = c.entry;
                    sync_cuelist_to_project();
                }
            }
            else if constexpr (std::is_same_v<T, cmd::DeletePlaybackCue>) {
                auto* pb = find_playback_def(c.playback_id);
                if (pb && c.cue_idx >= 0 && c.cue_idx < static_cast<int>(pb->cuelist.size())) {
                    pb->cuelist.erase(pb->cuelist.begin() + c.cue_idx);
                    auto* rs = find_playback_state(c.playback_id);
                    if (rs) {
                        const int del = c.cue_idx;
                        if (pb->cuelist.empty()) {
                            rs->current_idx  = -1;
                            rs->prev_idx     = -1;
                            rs->fade_in_dur  = 0.f;
                            rs->fade_elapsed = 0.f;
                            rs->active       = false;  // no cues left — stop playback
                            pb->active       = false;  // mirrors to UI snapshot (FIRE button)
                        } else {
                            if (rs->current_idx > del) --rs->current_idx;
                            else if (rs->current_idx == del)
                                rs->current_idx = std::min(del, (int)pb->cuelist.size() - 1);
                            if (rs->prev_idx > del) --rs->prev_idx;
                            else if (rs->prev_idx == del) {
                                rs->prev_idx     = -1;
                                rs->fade_in_dur  = 0.f;
                                rs->fade_elapsed = 0.f;
                            }
                        }
                    }
                    sync_cuelist_to_project();
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetPlaybackDmxTrigger>) {
                auto* pb = find_playback_def(c.id);
                if (pb) {
                    pb->dmx_universe  = c.universe;
                    pb->dmx_channel   = c.channel;
                    pb->dmx_threshold = c.threshold;
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetPlaybackConfig>) {
                auto* def = find_playback_def(c.playback_id);
                if (def) def->config = c.config;
            }
            else if constexpr (std::is_same_v<T, cmd::SetProgrammerFrame>) {
                programmer_objects_      = c.objects;
                programmer_global_layer_ = c.global_layer;
                programmer_fx_layer_     = c.fx_layer;
                programmer_blind_        = c.blind;
                programmer_active_       = c.active && !c.objects.objects.empty();
            }
            else if constexpr (std::is_same_v<T, cmd::ClearProgrammer>) {
                programmer_active_       = false;
                programmer_objects_      = {};
                programmer_global_layer_ = {};
                programmer_fx_layer_     = {};
                stream_prog_.clear(); // CLR wipes all per-stream latches too
            }
            else if constexpr (std::is_same_v<T, cmd::LatchProgrammer>) {
                for (int sid : c.stream_ids) {
                    auto& sp        = stream_prog_[sid];
                    sp.objects      = c.objects;
                    sp.global_layer = c.global_layer;
                    sp.fx_layer     = c.fx_layer;
                    sp.has_content  = !c.objects.objects.empty();
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetDmxUniverseOffset>) {
                dmx_universe_offset_ = c.offset;
            }
            else if constexpr (std::is_same_v<T, cmd::SetNdiConfig>) {
                // If the patch system already owns an NDI stream, the legacy sender
                // must stay off — re-enabling it would create a duplicate source.
                bool patch_owns_ndi = false;
                for (const auto& s : output_streams_)
                    if (s.config.type == OutputStreamType::NDI && s.config.enabled)
                        { patch_owns_ndi = true; break; }

                if (patch_owns_ndi) {
                    // Store settings for later (in case patch mode is removed),
                    // but do not create / re-create the legacy sender.
                    ndi_width_  = c.width;
                    ndi_height_ = c.height;
                    ndi_fps_N_  = std::max(1, c.fps_N);
                    ndi_fps_D_  = std::max(1, c.fps_D);
                    ndi_enabled_.store(false, std::memory_order_relaxed);
                } else {
                    ndi_enabled_.store(c.enabled, std::memory_order_relaxed);
                    if (c.enabled) {
                        if (c.width <= 0 || c.width > 8192 || c.height <= 0 || c.height > 8192) {
                            log::error("NDI: invalid dimensions {}x{}, ignoring", c.width, c.height);
                            return;
                        }
                        ndi_width_  = c.width;
                        ndi_height_ = c.height;
                        ndi_fps_N_  = std::max(1, c.fps_N);
                        ndi_fps_D_  = std::max(1, c.fps_D);
                        ndi_pixel_buf_.resize(
                            static_cast<size_t>(c.width) * static_cast<size_t>(c.height) * 4u);
                        ndi_sender_.init(c.name, c.width, c.height, c.fps_N, c.fps_D,
                                         /*bgra=*/true, c.clock_video);
                    } else {
                        ndi_sender_.shutdown();
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetRasterConfig>) {
                raster_cfg_.beam_radius              = c.beam_radius;
                raster_cfg_.glow_radius              = c.glow_radius;
                raster_cfg_.glow_alpha               = c.glow_alpha;
                raster_cfg_.otaniemi_enabled         = c.otaniemi_enabled;
                raster_cfg_.otaniemi_line_thickness  = c.otaniemi_line_thickness;
                raster_cfg_.otaniemi_brightness_boost= c.otaniemi_brightness_boost;
                raster_cfg_.otaniemi_glow_radius     = c.otaniemi_glow_radius;
                raster_cfg_.otaniemi_auto_fill       = c.otaniemi_auto_fill;
            }
            else if constexpr (std::is_same_v<T, cmd::SetOptimizerConfig>) {
                point_optimizer_.set_config(c.cfg);
                log::info("Engine: OptimizerConfig applied (pps=%d blank=%.0f corner=%.0f thresh=%.3f reorder=%d)",
                          c.cfg.target_pps, static_cast<double>(c.cfg.blank_dwell),
                          static_cast<double>(c.cfg.corner_dwell),
                          static_cast<double>(c.cfg.corner_angle_threshold),
                          (int)c.cfg.enable_reorder);
            }
            else if constexpr (std::is_same_v<T, cmd::SetOutputEnable>) {
                output_enabled_ = c.enabled;
                log::info("Engine: output %s", c.enabled ? "enabled" : "DISABLED (kill switch)");
            }
            else if constexpr (std::is_same_v<T, cmd::SetStreamTypeEnabled>) {
                using Kind = cmd::SetStreamTypeEnabled::Kind;
                if (c.kind == Kind::DAC)    { stream_type_dac_enabled_    = c.enabled; log::info("Engine: DAC stream type %s",    c.enabled ? "enabled" : "disabled"); }
                if (c.kind == Kind::NDI)    { stream_type_ndi_enabled_    = c.enabled; log::info("Engine: NDI stream type %s",    c.enabled ? "enabled" : "disabled"); }
                if (c.kind == Kind::ArtNet) { stream_type_artnet_enabled_ = c.enabled; log::info("Engine: ArtNet stream type %s", c.enabled ? "enabled" : "disabled"); }
                if (c.kind == Kind::IDN)    { log::info("Engine: IDN stream type %s",    c.enabled ? "enabled" : "disabled"); }
            }
            else if constexpr (std::is_same_v<T, cmd::SetEmergencyShutoff>) {
                emergency_shutoff_active_ = c.active;
                if (!c.active) {
                    // Re-enable output when emergency shutoff is cleared so that
                    // output is restored even if a separate SetOutputEnable command
                    // is lost or arrives out of order.
                    output_enabled_ = true;
                }
                log::info("Engine: emergency shutoff %s", c.active ? "ACTIVATED" : "deactivated");
            }
            else if constexpr (std::is_same_v<T, cmd::SetSafetyBlackout>) {
                safety_blackout_enabled_ = c.enabled;
                safety_blackout_borders_ = c.borders;
                safety_blackout_zones_   = c.zones;
            }
            else if constexpr (std::is_same_v<T, cmd::SetOutputPatch>) {
                // Reconcile the output_streams_ list against the new config.
                // Preserve existing NDI senders for unchanged streams; create
                // new ones for added streams; destroy removed ones.
                std::vector<OutputStreamRuntime> new_streams;
                new_streams.reserve(c.streams.size());

                for (const auto& cfg : c.streams) {
                    OutputStreamRuntime rt;
                    rt.config = cfg;

                    // Try to migrate existing stream (same id → reuse NDI sender)
                    for (auto& old : output_streams_) {
                        if (old.config.id == cfg.id) {
                            rt.ndi_sender    = std::move(old.ndi_sender);
                            rt.ndi_pixel_buf = std::move(old.ndi_pixel_buf);
                            rt.laser_bus     = old.laser_bus;
                            break;
                        }
                    }

                    // Create NDI sender if needed
                    if (cfg.type == OutputStreamType::NDI && cfg.enabled) {
                        if (!rt.ndi_sender) {
                            rt.ndi_sender = std::make_unique<NdiSender>();
                        }
                        // (Re)init if config changed
                        rt.ndi_sender->init(cfg.ndi_name,
                                            cfg.ndi_width, cfg.ndi_height,
                                            cfg.ndi_fps_N, cfg.ndi_fps_D,
                                            cfg.ndi_clock_video);
                        size_t px = static_cast<size_t>(cfg.ndi_width) *
                                    static_cast<size_t>(cfg.ndi_height) * 4u;
                        rt.ndi_pixel_buf.resize(px, 0u);
                    } else if (cfg.type != OutputStreamType::NDI && rt.ndi_sender) {
                        rt.ndi_sender->shutdown();
                        rt.ndi_sender.reset();
                    }

                    // Wire extra laser buses (slot 0 always uses bus_, slots 1+ use extra_laser_buses_)
                    if (cfg.type == OutputStreamType::Laser) {
                        int laser_idx = 0;
                        for (const auto& s2 : c.streams) {
                            if (s2.id == cfg.id) break;
                            if (s2.type == OutputStreamType::Laser) ++laser_idx;
                        }
                        if (laser_idx == 0) {
                            rt.laser_bus = &bus_;
                        } else {
                            // Ensure we have enough extra buses.
                            // extra_buses_mtx_ guards against the UI thread calling extra_laser_bus().
                            int needed = laser_idx;
                            std::lock_guard<std::mutex> lk(extra_buses_mtx_);
                            while (static_cast<int>(extra_laser_buses_.size()) < needed)
                                extra_laser_buses_.push_back(std::make_unique<RenderBus>());
                            rt.laser_bus = extra_laser_buses_[static_cast<size_t>(needed - 1)].get();
                        }
                    }

                    new_streams.push_back(std::move(rt));
                }

                // Shutdown NDI for removed streams
                for (auto& old : output_streams_) {
                    bool found = false;
                    for (const auto& cfg : c.streams)
                        if (cfg.id == old.config.id) { found = true; break; }
                    if (!found && old.ndi_sender)
                        old.ndi_sender->shutdown();
                }

                output_streams_ = std::move(new_streams);

                // Remove latched programmer state for streams that no longer exist in the patch
                {
                    std::vector<int> current_ids;
                    for (const auto& s : output_streams_)
                        current_ids.push_back(s.config.id);
                    for (auto it = stream_prog_.begin(); it != stream_prog_.end(); ) {
                        if (std::find(current_ids.begin(), current_ids.end(), it->first) == current_ids.end())
                            it = stream_prog_.erase(it);
                        else
                            ++it;
                    }
                }

                // If patch mode owns any NDI stream, shut down the legacy ndi_sender_
                // so it doesn't appear as a duplicate source in the NDI registry.
                // If no patch NDI streams exist, restore the legacy path.
                bool patch_owns_ndi = false;
                for (const auto& s : output_streams_)
                    if (s.config.type == OutputStreamType::NDI && s.config.enabled) { patch_owns_ndi = true; break; }
                if (patch_owns_ndi) {
                    if (ndi_enabled_.load()) {
                        ndi_sender_.shutdown();
                        ndi_enabled_.store(false);
                        log::info("Engine: legacy NDI disabled — patch mode owns NDI output");
                    }
                }

                log::info("Engine: output patch updated, %d streams",
                          static_cast<int>(output_streams_.size()));

                // Populate stream_defs_ from the command.
                // Prefer streams_def (new typed path) when provided; otherwise
                // synthesise OutputStreamDef entries from the legacy streams list
                // so stream_defs_ always reflects the current patch state.
                if (!c.streams_def.empty()) {
                    stream_defs_ = c.streams_def;
                } else {
                    stream_defs_.clear();
                    stream_defs_.reserve(c.streams.size());
                    for (const auto& cfg : c.streams) {
                        OutputStreamDef def;
                        def.id          = cfg.id;
                        def.name        = cfg.name;
                        def.type        = cfg.type;
                        def.enabled     = cfg.enabled;
                        def.dac_type    = cfg.dac_type;
                        def.dac_address = cfg.dac_address;
                        def.point_rate  = cfg.point_rate;
                        def.ndi_source_name = cfg.ndi_name;
                        // hdmi_title has no legacy equivalent — leave empty
                        def.transform   = cfg.transform;
                        def.safety      = cfg.safety;
                        stream_defs_.push_back(std::move(def));
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetActiveStreams>) {
                // Auto-latch programmer content for any stream being removed from
                // the active set.  This is the authoritative latch path: it runs
                // inside the engine thread so programmer_objects_ is guaranteed to
                // be the most recent non-empty content (SetProgrammerFrame(empty)
                // arrives AFTER SetActiveStreams in the queue, so programmer_active_
                // is still true and programmer_objects_ is still populated here).
                if (programmer_active_ && !programmer_objects_.objects.empty()) {
                    for (int old_id : active_stream_ids_) {
                        bool still_active = std::find(c.stream_ids.begin(),
                                                      c.stream_ids.end(),
                                                      old_id) != c.stream_ids.end();
                        if (!still_active) {
                            auto& sp        = stream_prog_[old_id];
                            sp.objects      = programmer_objects_;
                            sp.global_layer = programmer_global_layer_;
                            sp.fx_layer     = programmer_fx_layer_;
                            sp.has_content  = true;
                        }
                    }
                }
                active_stream_ids_ = c.stream_ids;
            }
            else if constexpr (std::is_same_v<T, cmd::SetMirroredStreams>) {
                mirrored_stream_ids_ = c.stream_ids;
            }
            // ── Timeline system commands ──────────────────────────────────────
            else if constexpr (std::is_same_v<T, cmd::CreateTimeline>) {
                // audio_peaks are precomputed by the UI thread (do_load_project or
                // on_timeline_create callbacks) — just forward the def as-is.
                timeline_engine_.create(c.def);
                if (c.def.audio_track.has_value()) {
                    timeline_audio_player_.load(c.def.audio_track->file_path);
                    timeline_audio_player_.set_volume(c.def.audio_track->volume);
                }
            }
            else if constexpr (std::is_same_v<T, cmd::DeleteTimeline>) {
                timeline_engine_.remove(c.id);
            }
            else if constexpr (std::is_same_v<T, cmd::RenameTimeline>) {
                for (const auto& snap : timeline_engine_.defs()) {
                    if (snap.id == c.id) {
                        TimelineDef d = snap;
                        d.name = c.name;
                        timeline_engine_.update_def(std::move(d));
                        break;
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetTimelineArmed>) {
                timeline_engine_.set_armed(c.id, c.armed);
            }
            else if constexpr (std::is_same_v<T, cmd::SetTimelineRecordArmed>) {
                for (const auto& snap : timeline_engine_.defs()) {
                    if (snap.id == c.id) {
                        TimelineDef d = snap;
                        d.record_armed = c.armed;
                        bool had_no_tracks = d.tracks.empty();
                        timeline_engine_.update_def(std::move(d));
                        // Auto-create a default recording track when arming
                        // with no existing tracks so record_event always has
                        // somewhere to write events.
                        if (c.armed && had_no_tracks)
                            timeline_engine_.add_track(c.id, "Record");
                        break;
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::TimelinePlay>) {
                timeline_engine_.play(c.id);
                // Start audio from the current playhead position
                for (const auto& def : timeline_engine_.defs()) {
                    if (def.id == c.id && def.audio_track.has_value()) {
                        const auto& at = def.audio_track.value();
                        const auto  rsnaps = timeline_engine_.runtime_snaps();
                        double pos_s = 0.0;
                        for (const auto& rs : rsnaps) {
                            if (rs.id == c.id) {
                                int fps_int = smpte_max_frames(def.fps);
                                if (fps_int <= 0) fps_int = 25;
                                double offset_s = static_cast<double>(at.offset_frames)
                                                  / static_cast<double>(fps_int);
                                pos_s = static_cast<double>(rs.position_frames)
                                        / static_cast<double>(fps_int)
                                        - offset_s;
                                if (pos_s < 0.0) pos_s = 0.0;
                                break;
                            }
                        }
                        timeline_audio_player_.play_from(pos_s);
                        break;
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::TimelinePause>) {
                timeline_engine_.pause(c.id);
                timeline_audio_player_.pause();
            }
            else if constexpr (std::is_same_v<T, cmd::TimelineStop>) {
                timeline_engine_.stop(c.id);
                timeline_engine_.clear_record_armed(c.id);
                timeline_audio_player_.stop();
            }
            else if constexpr (std::is_same_v<T, cmd::TimelineRewind>) {
                timeline_engine_.rewind(c.id);
                timeline_audio_player_.stop();
            }
            else if constexpr (std::is_same_v<T, cmd::TimelineSeek>) {
                timeline_engine_.seek(c.id, c.frame);
                // If the timeline is currently playing, restart audio from the new position
                bool is_playing = false;
                for (const auto& rs : timeline_engine_.runtime_snaps()) {
                    if (rs.id == c.id) {
                        is_playing = (rs.state == TimelineState::Playing);
                        break;
                    }
                }
                for (const auto& def : timeline_engine_.defs()) {
                    if (def.id == c.id && def.audio_track.has_value()) {
                        if (is_playing) {
                            const auto& at = def.audio_track.value();
                            int fps_int = smpte_max_frames(def.fps);
                            if (fps_int <= 0) fps_int = 25;
                            double offset_s = static_cast<double>(at.offset_frames)
                                              / static_cast<double>(fps_int);
                            double pos_s = static_cast<double>(c.frame)
                                           / static_cast<double>(fps_int)
                                           - offset_s;
                            if (pos_s < 0.0) pos_s = 0.0;
                            timeline_audio_player_.play_from(pos_s);
                        } else {
                            timeline_audio_player_.pause();
                        }
                        break;
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetTimelineSource>) {
                for (const auto& snap : timeline_engine_.defs()) {
                    if (snap.id == c.id) {
                        TimelineDef d = snap;
                        d.tc_slot = c.slot;
                        timeline_engine_.update_def(std::move(d));
                        break;
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetTimelineLink>) {
                for (const auto& snap : timeline_engine_.defs()) {
                    if (snap.id == c.id) {
                        TimelineDef d = snap;
                        d.link_mode = c.link_mode;
                        timeline_engine_.update_def(std::move(d));
                        break;
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::SetTimelineOffset>) {
                for (const auto& snap : timeline_engine_.defs()) {
                    if (snap.id == c.id) {
                        TimelineDef d = snap;
                        d.time_offset_frames = c.offset_frames;
                        timeline_engine_.update_def(std::move(d));
                        break;
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::AddTimelineTrack>) {
                timeline_engine_.add_track(c.timeline_id, c.name);
            }
            else if constexpr (std::is_same_v<T, cmd::RemoveTimelineTrack>) {
                timeline_engine_.remove_track(c.timeline_id, c.track_id);
            }
            else if constexpr (std::is_same_v<T, cmd::UpdateTimelineTrack>) {
                timeline_engine_.update_track(c.timeline_id, c.track_id,
                                              c.name, c.muted, c.locked, c.collapsed);
            }
            else if constexpr (std::is_same_v<T, cmd::AddTimelineEvent>) {
                timeline_engine_.add_event(c.timeline_id, c.track_id, c.event);
            }
            else if constexpr (std::is_same_v<T, cmd::RemoveTimelineEvent>) {
                timeline_engine_.remove_event(c.timeline_id, c.track_id, c.event_id);
            }
            else if constexpr (std::is_same_v<T, cmd::UpdateTimelineEvent>) {
                timeline_engine_.update_event(c.timeline_id, c.track_id, c.event);
            }
            else if constexpr (std::is_same_v<T, cmd::SetTimecodeSettings>) {
                tc_config_ = c.config;
                timeline_engine_.set_tc_config(c.config);
            }
            else if constexpr (std::is_same_v<T, cmd::TimelineWaitForGoAdvance>) {
                timeline_engine_.wait_for_go_advance(c.id);
            }
            else if constexpr (std::is_same_v<T, cmd::SetTimelineAudio>) {
                for (const auto& snap : timeline_engine_.defs()) {
                    if (snap.id == c.id) {
                        TimelineDef d = snap;
                        d.audio_track = c.track;
                        d.audio_peaks = std::move(c.peaks); // precomputed by UI thread
                        timeline_engine_.update_def(std::move(d));
                        // Load the sound so it's ready when play is triggered
                        timeline_audio_player_.load(c.track.file_path);
                        timeline_audio_player_.set_volume(c.track.volume);
                        break;
                    }
                }
            }
            else if constexpr (std::is_same_v<T, cmd::ClearTimelineAudio>) {
                for (const auto& snap : timeline_engine_.defs()) {
                    if (snap.id == c.id) {
                        TimelineDef d = snap;
                        d.audio_track.reset();
                        d.audio_peaks.clear();
                        timeline_engine_.update_def(std::move(d));
                        timeline_audio_player_.stop();
                        timeline_audio_player_.unload();
                        break;
                    }
                }
            }
        }, cmd);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  record_event_to_armed_timelines
//  Called after ActivateCue / SetPlaybackGo so live-playing+record_armed
//  timelines automatically capture a matching event at the current playhead.
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::record_event_to_armed_timelines(TimelineEventType type,
                                                  const std::string& target_id,
                                                  std::map<std::string, float> params,
                                                  std::string label)
{
    const auto rsnaps = timeline_engine_.runtime_snaps();
    // Take a snapshot of def IDs so we can re-fetch safely after add_track().
    std::vector<std::string> def_ids;
    {
        const auto& defs = timeline_engine_.defs();
        if (rsnaps.size() != defs.size()) return; // defensive: sizes must match
        def_ids.reserve(defs.size());
        for (const auto& d : defs) def_ids.push_back(d.id);
    }

    for (size_t i = 0; i < def_ids.size(); ++i) {
        // Re-fetch the def each iteration — add_track() can reallocate defs_.
        const auto& defs = timeline_engine_.defs();
        const TimelineDef* defp = nullptr;
        for (const auto& d : defs) {
            if (d.id == def_ids[i]) { defp = &d; break; }
        }
        if (!defp) continue;
        if (!defp->record_armed) continue;

        // Auto-create a "Record" track when no tracks exist yet.
        if (defp->record_armed && defp->tracks.empty()) {
            timeline_engine_.add_track(def_ids[i], "Record");
            // add_track() may have reallocated defs_ — re-fetch the pointer.
            const auto& defs2 = timeline_engine_.defs();
            defp = nullptr;
            for (const auto& d : defs2) {
                if (d.id == def_ids[i]) { defp = &d; break; }
            }
            if (!defp) continue;
        }

        if (defp->tracks.empty()) continue; // still empty after auto-create: skip

        // Auto-start playback on the first recorded event so position advances.
        if (i >= rsnaps.size()) break;
        if (rsnaps[i].state == TimelineState::Idle)
            timeline_engine_.play(def_ids[i]);

        const int64_t pos = rsnaps[i].position_frames;
        for (const auto& track : defp->tracks) {
            if (!track.locked && !track.muted) {
                TimelineEvent ev;
                ev.tc_position = pos;
                ev.type        = type;
                ev.target_id   = target_id;
                ev.params      = params;
                ev.label       = label;
                timeline_engine_.add_event(def_ids[i], track.id, std::move(ev));
                break;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  dispatch_timeline_event — convert a fired TimelineEvent → EngineCommand
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::dispatch_timeline_event(const TimelineEngine::FiredEvent& fe)
{
    const TimelineEvent& ev = fe.event;
    switch (ev.type) {
    case TimelineEventType::CueGo:
        // target_id holds the cue slot index recorded at capture time, or the
        // special value "back" (CueListBack was recorded) or empty (generic Go).
        if (ev.target_id == "back") {
            send(cmd::CueListBack{});
        } else if (!ev.target_id.empty()) {
            try {
                int slot = std::stoi(ev.target_id);
                send(cmd::ActivateCue{ slot, 0.f });
            } catch (...) {
                send(cmd::CueListGo{});
            }
        } else {
            send(cmd::CueListGo{});
        }
        break;
    case TimelineEventType::PlaybackGo:
    case TimelineEventType::PlaybackActivate:
        try {
            int pb_id = std::stoi(ev.target_id);
            send(cmd::SetPlaybackGo{ pb_id });
        } catch (...) {
            log::warn("dispatch_timeline_event: failed to parse target_id '%s' for event type %d",
                ev.target_id.c_str(), static_cast<int>(ev.type));
        }
        break;
    case TimelineEventType::PlaybackRelease:
        try {
            int pb_id = std::stoi(ev.target_id);
            send(cmd::SetPlaybackStop{ pb_id });
        } catch (...) {
            log::warn("dispatch_timeline_event: failed to parse target_id '%s' for event type %d",
                ev.target_id.c_str(), static_cast<int>(ev.type));
        }
        break;
    case TimelineEventType::SetLevel: {
        auto it = ev.params.find("intensity");
        float lvl = (it != ev.params.end()) ? it->second : 1.f;
        try {
            int pb_id = std::stoi(ev.target_id);
            send(cmd::SetPlaybackIntensity{ pb_id, lvl });
        } catch (...) {
            log::warn("dispatch_timeline_event: failed to parse target_id '%s' for event type %d",
                ev.target_id.c_str(), static_cast<int>(ev.type));
        }
        break;
    }
    case TimelineEventType::Flash:
        try {
            int pb_id = std::stoi(ev.target_id);
            send(cmd::SetPlaybackGo{ pb_id });
        } catch (...) {
            log::warn("dispatch_timeline_event: failed to parse target_id '%s' for event type %d",
                ev.target_id.c_str(), static_cast<int>(ev.type));
        }
        break;
    case TimelineEventType::Command:
        if (ev.target_id == "Blackout" || ev.target_id == "blackout")
            send(cmd::SetOutputEnable{ false });
        // "OutputOn" is intentionally not supported from timeline events:
        // a timeline must never be able to override an operator's kill switch.
        break;
    case TimelineEventType::Marker:
        // No action — marker is informational only
        break;
    case TimelineEventType::WaitForGo:
        // wait_for_go flag is already set inside collect_events() when the event
        // fires; this dispatch call is a no-op for internal-clock timelines.
        // For external-TC timelines the flag stops internal clock advancement.
        (void)fe;
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  process_dmx_input
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::process_dmx_input()
{
    std::pair<int, DmxUniverse> entry;
    while (dmx_queue_.try_pop(entry))
    {
        int u = entry.first;
        if (u >= 0 && u < kMaxUniverses)
            dmx_[u] = entry.second;
    }

    // ── DMX-triggered playbacks ───────────────────────────────────────────────
    if (project_) {
        for (auto& def : project_->playbacks) {
            if (def.config.dmx_mode == PlaybackConfig::DmxMode::Off) continue;

            int univ   = def.config.dmx_universe + dmx_universe_offset_;  // offset shifts universe only
            int ch     = def.config.dmx_channel;                          // 1-based (1..512)
            int ch_idx = ch - 1;                                          // convert to 0-based array index
            if (univ < 0 || univ >= kMaxUniverses || ch < 1 || ch > 512) continue;
            if (ch_idx < 0 || ch_idx >= 512) continue;

            uint8_t ch1_val = dmx_[univ].ch[static_cast<size_t>(ch_idx)];

            auto* state = find_playback_state(def.id);
            if (!state) continue;

            if (def.config.dmx_mode == PlaybackConfig::DmxMode::OneChannel) {
                // ch1 > threshold = active (intensity from ch1 value)
                // manual_active keeps the playback alive even when DMX is low —
                // only a manual Stop (SetPlaybackStop) clears it.
                bool should_be_active = (ch1_val > def.config.dmx_threshold) || state->manual_active;
                if (should_be_active && !state->active) {
                    state->active    = true;
                    state->intensity = static_cast<float>(ch1_val) / 255.f;
                    if (state->current_idx < 0) {
                        auto* def_m = find_playback_def(def.id);
                        if (def_m && !def_m->cuelist.empty())
                            state->current_idx = 0;
                    }
                } else if (!should_be_active && state->active) {
                    state->active = false;
                } else if (should_be_active && ch1_val > def.config.dmx_threshold) {
                    // Only update intensity from DMX when DMX is actually sending value
                    state->intensity = static_cast<float>(ch1_val) / 255.f;
                }
                def.active = state->active;  // mirror to PlaybackDef for UI snapshot
            } else { // TwoChannel
                // ch1 = on/off intensity, ch2 = GO trigger (rising edge)
                // manual_active keeps the playback alive even when DMX ch1 is low.
                uint8_t ch2_val = (ch_idx + 1 < 512) ? dmx_[univ].ch[static_cast<size_t>(ch_idx + 1)] : 0;
                bool ch1_active = (ch1_val > def.config.dmx_threshold) || state->manual_active;
                if (ch1_active) {
                    if (!state->active && state->current_idx < 0) {
                        auto* def_m = find_playback_def(def.id);
                        if (def_m && !def_m->cuelist.empty())
                            state->current_idx = 0;
                    }
                    state->active    = true;
                    state->intensity = static_cast<float>(ch1_val) / 255.f;
                } else {
                    state->active = false;
                }
                def.active = state->active;  // mirror to PlaybackDef for UI snapshot
                // Rising edge detection for ch2 GO trigger
                bool ch2_trigger = (ch2_val > 127 && state->dmx_ch2_prev <= 127);
                state->dmx_ch2_prev = ch2_val;
                if (ch2_trigger && state->active) {
                    // Advance cue with end-behavior check
                    auto* def_mut = find_playback_def(def.id);
                    if (def_mut && !def_mut->cuelist.empty()) {
                        int next_idx = state->current_idx + 1;
                        if (next_idx >= static_cast<int>(def_mut->cuelist.size())) {
                            if (def_mut->config.end_behavior == PlaybackConfig::EndBehavior::Loop) {
                                state->current_idx = 0;
                            }
                            // else: stay on last cue (Stop behavior)
                        } else {
                            state->current_idx = next_idx;
                        }
                    }
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  evaluate_cues — apply DMX and automation to the active cue's params
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::evaluate_cues(double t, double /*dt*/)
{
    evaluated_params_valid_ = false;

    if (!project_ || active_cue_ < 0
        || active_cue_ >= static_cast<int>(project_->cues.size()))
        return;

    Cue& cue = project_->cues[active_cue_];
    // Use a copy so fade/master scaling doesn't permanently corrupt the stored params
    GeneratorParams p = cue.params;

    // ── Apply DMX patches ─────────────────────────────────────────────────
    for (const DmxPatch& patch : project_->dmx_patches)
    {
        if (patch.universe < 0 || patch.universe >= kMaxUniverses) continue;
        const DmxUniverse& uni = dmx_[patch.universe];
        for (const auto& ch : patch.profile.channels)
        {
            int addr = patch.start_addr + ch.offset - 1;
            if (addr < 0 || addr > 511) continue;
            float norm = uni.norm(addr); // 0..1
            float val  = ch.scale_min + norm * (ch.scale_max - ch.scale_min);
            const std::string& n = ch.param_name;
            if      (n == "speed")     p.speed     = val;
            else if (n == "scale")     p.scale     = val;
            else if (n == "density")   p.density   = val;
            else if (n == "param_a")   p.param_a   = val;
            else if (n == "param_b")   p.param_b   = val;
            else if (n == "param_c")   p.param_c   = val;
            else if (n == "rotation")  p.rotation  = val;
            else if (n == "pan")       p.pan       = val;
            else if (n == "tilt")      p.tilt      = val;
            else if (n == "zoom")      p.zoom      = val;
            else if (n == "intensity") p.intensity = val;
            else if (n == "r")         p.color_a.r = val;
            else if (n == "g")         p.color_a.g = val;
            else if (n == "b")         p.color_a.b = val;
        }
    }

    // ── Apply automation keyframe tracks ─────────────────────────────────
    AudioSnapshot audio_snap = audio_.snapshot();
    for (const ParamTrack& track : cue.automation)
    {
        float val = 0.f;

        // Automation priority: audio > DMX > keyframe
        if (track.audio_mapped)
        {
            const std::string& src = track.audio_source;
            if      (src == "rms")  val = audio_snap.rms;
            else if (src == "peak") val = audio_snap.peak;
            else if (src == "bpm")  val = audio_snap.bpm / 300.f;
            else if (src == "sub")  val = audio_snap.sub_band;
            else if (src == "mid")  val = audio_snap.mid_band;
            else if (src == "high") val = audio_snap.high_band;
            else if (src.rfind("fft:", 0) == 0)
            {
                try {
                    int bin = std::stoi(src.substr(4));
                    bin = std::clamp(bin, 0, AudioSnapshot::kFFTBins - 1);
                    val = audio_snap.fft[bin];
                } catch (...) {}
            }
        }
        else if (track.dmx_mapped)
        {
            if (track.dmx_universe < 0 || track.dmx_universe >= kMaxUniverses) continue;
            const DmxUniverse& uni = dmx_[track.dmx_universe];
            val = uni.norm(track.dmx_channel);
        }
        else if (!track.keyframes.empty())
        {
            val = track.evaluate(t);
        }

        // Write evaluated value back to cue params
        const std::string& n = track.param_name;
        if      (n == "speed")     p.speed     = val;
        else if (n == "scale")     p.scale     = val;
        else if (n == "density")   p.density   = val;
        else if (n == "param_a")   p.param_a   = val;
        else if (n == "param_b")   p.param_b   = val;
        else if (n == "param_c")   p.param_c   = val;
        else if (n == "rotation")  p.rotation  = val;
        else if (n == "pan")       p.pan       = val;
        else if (n == "tilt")      p.tilt      = val;
        else if (n == "zoom")      p.zoom      = val;
        else if (n == "intensity") p.intensity = val;
    }

    // ── Apply fade level ──────────────────────────────────────────────────
    p.intensity *= fade_level_ * master_;

    // Store the fully-evaluated params so build_frame() can use them.
    evaluated_params_       = p;
    evaluated_params_valid_ = true;

    // ── Chaser step advance ──────────────────────────────────────────────────
    // Find the FullCueEntry by cue_id using cue_list_ (engine-thread-owned) instead
    // of project_->full_cue_list to avoid a data race with the UI save path.
    const FullCueEntry* ace_ptr = nullptr;
    if (active_cue_ >= 0 && active_cue_ < static_cast<int>(project_->cues.size())) {
        const std::string& target_id = project_->cues[static_cast<size_t>(active_cue_)].id;
        for (int ei = 0; ei < cue_list_.entry_count(); ++ei) {
            const auto& fce = cue_list_.entry_at(ei);
            if (fce.cue_id == target_id) { ace_ptr = &fce; break; }
        }
    }
    if (ace_ptr) {
        const auto& ace = *ace_ptr;
        if (ace.is_chaser && !ace.chaser_steps.empty()) {
            if (!chaser_run_.has_value()) {
                // Just activated a chaser cue — initialise
                ChaserRunState rs;
                rs.step = 0;
                rs.step_enter_s = time_;
                rs.effective_cue_idx = ace.chaser_steps[0].cue_ref_idx;
                chaser_run_ = rs;
            } else {
                auto& rs = *chaser_run_;
                int n_steps = static_cast<int>(ace.chaser_steps.size());
                const auto& step = ace.chaser_steps[rs.step % n_steps];
                float hold = (step.hold_s >= 0.f) ? step.hold_s : ace.chaser_global_hold;
                if (step.beat_sync && bpm_ > 0.f) {
                    hold = (4.f / static_cast<float>(std::max(1, step.beat_div))) * (60.f / bpm_);
                }
                if ((time_ - rs.step_enter_s) >= static_cast<double>(hold)) {
                    rs.step = (rs.step + 1) % n_steps;
                    rs.step_enter_s = time_;
                    rs.effective_cue_idx = ace.chaser_steps[rs.step].cue_ref_idx;
                }
            }
        } else if (!ace.is_chaser && chaser_run_.has_value()) {
            chaser_run_.reset();
        }
    } else {
        chaser_run_.reset();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_frame — call generator, apply transforms, post to bus
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::build_frame()
{
    PointBuffer points;

    // Generator-based cue rendering (only if a main cue is active)
    if (project_ && active_cue_ >= 0
        && active_cue_ < static_cast<int>(project_->cues.size()))
    {
    int gen_cue_idx = active_cue_;
    if (chaser_run_.has_value()) {
        int eff = chaser_run_->effective_cue_idx;
        if (eff >= 0 && eff < static_cast<int>(project_->cues.size()))
            gen_cue_idx = eff;
    }

    const Cue& cue = project_->cues[static_cast<size_t>(gen_cue_idx)];
    // Use evaluated_params_ (set by evaluate_cues) which includes DMX patches,
    // automation, fade level, and master intensity.  Fall back to raw cue.params
    // only if evaluate_cues did not run for this cue (e.g. chaser effective slot
    // differs from active_cue_), to avoid rendering with stale modulated values.
    const GeneratorParams& p = (evaluated_params_valid_ && gen_cue_idx == active_cue_)
                               ? evaluated_params_
                               : cue.params;

    if (!p.blanked)
    {
    // ── Update audio bridge for audio-reactive generators ─────────────────
    { std::lock_guard<std::mutex> lk(g_audio_snap_mtx); g_last_audio_snap = audio_.snapshot(); }

    // ── Update ILDA path if this is an ILDA cue ───────────────────────────
    if (cue.generator == GeneratorType::ILDASequence && !cue.ilda_path.empty())
    {
        set_ilda_path(cue.ilda_path);
    }

    // ── Find and call the generator ───────────────────────────────────────
    // Map GeneratorType enum to registry name
    static const char* generator_names[] = {
        "beams",       // Beams
        "waves",       // Waves
        "lissajous",   // Lissajous
        "tunnel",      // Tunnel
        "text",        // TextScroller
        "oscilloscope",// Oscilloscope
        "fft_bars",    // FFTBars
        "spirograph",  // Spirograph
        "particles",   // ParticleField
        "geomorph",    // GeometricMorph
        "ribbon",      // Ribbon
        "grid",        // Grid
        "starburst",   // Starburst
        "fan_sweep",   // FanSweep
        "cone_sweep",  // ConeSweep
        "ilda",        // ILDASequence
        "beams",       // Custom (fallback)
    };

    int gtype = static_cast<int>(cue.generator);
    if (gtype < 0 || gtype >= static_cast<int>(std::size(generator_names)))
        gtype = 0;

    const char* gname = generator_names[gtype];
    IGenerator* gen = find_generator(gname);

    if (gen)
        points = gen->generate(p, time_);
    else
    {
        // Fallback: blank frame
        log::warn("Generator '%s' not found", gname);
    }

    // ── Apply global 2D affine transform (pan, tilt, zoom, rotation) ──────
    float cos_r = std::cos(p.rotation);
    float sin_r = std::sin(p.rotation);
    float zoom  = std::max(p.zoom, 0.01f);
    float pan   = p.pan;
    float tilt  = p.tilt;

    for (LaserPoint& pt : points)
    {
        // 1. Scale
        float fx = pt.nx() * zoom;
        float fy = pt.ny() * zoom;

        // 2. Rotate
        float rx = fx * cos_r - fy * sin_r;
        float ry = fx * sin_r + fy * cos_r;

        // 3. Pan / tilt
        rx += pan;
        ry += tilt;

        pt.x = static_cast<int16_t>(
            std::clamp(rx, -1.f, 1.f) * 32767.f);
        pt.y = static_cast<int16_t>(
            std::clamp(ry, -1.f, 1.f) * 32767.f);
    }

    // ── Run per-cue FX engine (applied before intensity scaling) ─────────
    if (active_cue_ >= 0 && active_cue_ < static_cast<int>(cue_fx_engines_.size())) {
        cue_fx_engines_[static_cast<size_t>(active_cue_)].process(points, last_dt_, expr_ctx_);
    }

    // ── Apply master intensity to all lit (non-blanked) points ────────────
    float intensity = std::clamp(p.intensity, 0.f, 1.f);
    for (LaserPoint& pt : points)
    {
        if (!pt.blanked)
        {
            pt.r = static_cast<uint8_t>(pt.r * intensity);
            pt.g = static_cast<uint8_t>(pt.g * intensity);
            pt.b = static_cast<uint8_t>(pt.b * intensity);
        }
    }

    } // end !p.blanked
    } // end generator cue block

    // ── Per-stream playback buffers (playbacks with explicit output_stream_ids) ─
    std::map<int, PointBuffer> per_stream_extras;

    // ── Multi-playback compositing (additive merge) ───────────────────────────
    for (auto& rs : playback_states_) {
        if (!rs.active || rs.blind) continue;
        const PlaybackDef* pb = find_playback_def(rs.id);
        if (!pb || rs.current_idx < 0 || rs.current_idx >= static_cast<int>(pb->cuelist.size()))
            continue;

        float pb_intensity = std::clamp(rs.intensity * master_, 0.f, 1.f);

        // Advance the per-playback fade elapsed counter using last_dt_ (monotonic,
        // not tied to time_ which resets on cue activation).
        if (rs.fade_in_dur > 0.f)
            rs.fade_elapsed += last_dt_;

        // Compute fade-in alpha from per-playback elapsed time (immune to time_ resets).
        float alpha = (rs.fade_in_dur > 0.f)
                      ? std::clamp(rs.fade_elapsed / rs.fade_in_dur, 0.f, 1.f)
                      : 1.f;
        rs.fade_level = alpha;

        // A fade is in progress when fade_in_dur > 0 and not yet complete
        bool fading = (rs.fade_in_dur > 0.f && alpha < 1.f);

        // BPM-scaled FX time (applied to both fading and non-fading paths)
        float fx_t = pb->config.fx_at_bpm && bpm_ > 0.f
                     ? static_cast<float>(fx_time_) * bpm_ / 60.f
                     : static_cast<float>(fx_time_);

        if (fading) {
            // Per-object morphing: objects matched by ID morph position/color;
            // new objects fade in, removed objects fade out.
            const FullCueEntry& fce     = pb->cuelist[static_cast<size_t>(rs.current_idx)];
            const FullCueEntry* prev_fce = (rs.prev_idx >= 0 && rs.prev_idx < static_cast<int>(pb->cuelist.size()))
                                           ? &pb->cuelist[static_cast<size_t>(rs.prev_idx)] : nullptr;
            const KeyframeLayer& cur_kf = fce.keyframe_layer;
            const KeyframeLayer* prev_kf = prev_fce ? &prev_fce->keyframe_layer : nullptr;

            // Match objects by ID, with fallback to text for symmetry copies (__sym prefix)
            auto find_matching = [](const std::vector<LaserObject>& objs, const LaserObject& target) -> const LaserObject* {
                for (const auto& o : objs) if (o.id == target.id) return &o;
                if (target.text.rfind("__sym", 0) == 0)
                    for (const auto& o : objs)
                        if (o.text == target.text) return &o;
                return nullptr;
            };

            // Helper: build a morphed KeyframeLayer from a current and previous KeyframeLayer
            auto build_morphed = [&](const KeyframeLayer& ckf, const KeyframeLayer* pkf) -> KeyframeLayer {
                KeyframeLayer morphed;
                morphed.symmetry_mode = ckf.symmetry_mode;
                morphed.sym_cx        = ckf.sym_cx;
                morphed.sym_cy        = ckf.sym_cy;
                for (const auto& co : ckf.objects) {
                    if (co.text.rfind("__sym", 0) == 0) continue;
                    const LaserObject* po = pkf ? find_matching(pkf->objects, co) : nullptr;
                    if (po && po->type == co.type && po->pts.size() == co.pts.size()) {
                        LaserObject lerped = co;
                        for (size_t pi = 0; pi < co.pts.size(); ++pi) {
                            lerped.pts[pi].x = po->pts[pi].x + (co.pts[pi].x - po->pts[pi].x) * alpha;
                            lerped.pts[pi].y = po->pts[pi].y + (co.pts[pi].y - po->pts[pi].y) * alpha;
                        }
                        lerped.r = po->r + (co.r - po->r) * alpha;
                        lerped.g = po->g + (co.g - po->g) * alpha;
                        lerped.b = po->b + (co.b - po->b) * alpha;
                        morphed.objects.push_back(std::move(lerped));
                    } else {
                        LaserObject obj = co;
                        obj.r *= alpha; obj.g *= alpha; obj.b *= alpha;
                        morphed.objects.push_back(std::move(obj));
                    }
                }
                if (pkf) {
                    float inv = 1.f - alpha;
                    for (const auto& po : pkf->objects) {
                        if (po.text.rfind("__sym", 0) == 0) continue;
                        if (!find_matching(ckf.objects, po)) {
                            LaserObject obj = po;
                            obj.r *= inv; obj.g *= inv; obj.b *= inv;
                            morphed.objects.push_back(std::move(obj));
                        }
                    }
                }
                return morphed;
            };

            const auto& assigned = pb->config.output_stream_ids;
            // Effective stream list: explicit assignment wins; otherwise all configured streams.
            std::vector<int> effective_ids;
            if (!assigned.empty()) {
                effective_ids = assigned;
            } else {
                for (const auto& s : output_streams_)
                    effective_ids.push_back(s.config.id);
            }
            if ((!fce.per_stream_fx.empty() || !fce.per_stream_kf.empty()) && !effective_ids.empty()) {
                // Per-stream path during fade: each stream may have different geometry and/or FX.
                // Only render streams that were explicitly programmed (have per-stream KF or FX).
                // Streams with neither entry are skipped — they were not part of this cue.
                for (int sid : effective_ids) {
                    auto cur_kf_it = fce.per_stream_kf.find(sid);
                    auto fx_it     = fce.per_stream_fx.find(sid);
                    if (cur_kf_it == fce.per_stream_kf.end() && fx_it == fce.per_stream_fx.end())
                        continue;
                    const KeyframeLayer& stream_cur_kf = (cur_kf_it != fce.per_stream_kf.end())
                                                         ? cur_kf_it->second : cur_kf;
                    const KeyframeLayer* stream_prev_kf = prev_kf;
                    if (prev_fce && !prev_fce->per_stream_kf.empty()) {
                        auto prev_kf_it = prev_fce->per_stream_kf.find(sid);
                        if (prev_kf_it != prev_fce->per_stream_kf.end())
                            stream_prev_kf = &prev_kf_it->second;
                    }
                    KeyframeLayer stream_morphed = build_morphed(stream_cur_kf, stream_prev_kf);
                    if (stream_morphed.objects.empty()) continue;
                    PointBuffer stream_pts = render_keyframe_layer(stream_morphed, programmer_point_budget());
                    const FxLayer& stream_fx = (fx_it != fce.per_stream_fx.end())
                                               ? fx_it->second : fce.fx_layer;
                    apply_frame_fx(stream_pts, stream_fx, fx_t);
                    apply_global_geometry(stream_pts, fce.global_layer);
                    apply_global_layer(stream_pts, fce.global_layer, fx_t, pb_intensity, rs.gfx_smooth);
                    // Apply per-cue mirroring only when live group mirroring is NOT
                    // already active for this stream.  If both applied simultaneously
                    // they double-invert (net no flip), which is the mirror-playback bug.
                    {
                        bool cue_mir  = !fce.mirrored_ids.empty() &&
                            std::find(fce.mirrored_ids.begin(), fce.mirrored_ids.end(), sid) != fce.mirrored_ids.end();
                        bool live_mir = !mirrored_stream_ids_.empty() &&
                            std::find(mirrored_stream_ids_.begin(), mirrored_stream_ids_.end(), sid) != mirrored_stream_ids_.end();
                        if (cue_mir && !live_mir)
                            for (auto& pt : stream_pts) pt.x = -pt.x;
                    }
                    for (const auto& pt : stream_pts)
                        per_stream_extras[sid].push_back(pt);
                }
            } else {
                // Global path during fade: all streams get the same morphed geometry and FX
                KeyframeLayer morphed = build_morphed(cur_kf, prev_kf);
                if (!morphed.objects.empty()) {
                    PointBuffer morph_pts = render_keyframe_layer(morphed, programmer_point_budget());
                    apply_frame_fx(morph_pts, fce.fx_layer, fx_t);
                    apply_global_geometry(morph_pts, fce.global_layer);
                    apply_global_layer(morph_pts, fce.global_layer, fx_t, pb_intensity, rs.gfx_smooth);
                    if (assigned.empty() && fce.mirrored_ids.empty()) {
                        // No per-stream routing needed: push to global composite buffer.
                        for (auto& pt : morph_pts)
                            points.push_back(pt);
                    } else if (assigned.empty()) {
                        // No explicit assignment but per-cue mirroring is active: fan out
                        // per-stream so each mirrored stream receives a flipped copy.
                        // Guard: skip pre-flip for streams where live group mirror is active
                        // (the fanout will flip via mirrored_stream_ids_; pre-flipping here
                        // would double-invert to no flip — the mirror-playback bug).
                        for (int sid : effective_ids) {
                            bool is_mir  = std::find(fce.mirrored_ids.begin(),
                                                      fce.mirrored_ids.end(), sid)
                                           != fce.mirrored_ids.end();
                            bool live_mir = !mirrored_stream_ids_.empty() &&
                                std::find(mirrored_stream_ids_.begin(), mirrored_stream_ids_.end(), sid) != mirrored_stream_ids_.end();
                            if (!is_mir || live_mir) {
                                for (const auto& pt : morph_pts)
                                    per_stream_extras[sid].push_back(pt);
                            } else {
                                for (auto pt : morph_pts) {
                                    pt.x = static_cast<int16_t>(-pt.x);
                                    per_stream_extras[sid].push_back(pt);
                                }
                            }
                        }
                    } else {
                        for (int sid : assigned) {
                            bool is_mir  = !fce.mirrored_ids.empty() &&
                                           std::find(fce.mirrored_ids.begin(), fce.mirrored_ids.end(), sid) != fce.mirrored_ids.end();
                            bool live_mir = !mirrored_stream_ids_.empty() &&
                                std::find(mirrored_stream_ids_.begin(), mirrored_stream_ids_.end(), sid) != mirrored_stream_ids_.end();
                            for (auto pt : morph_pts) {   // copy by value
                                if (is_mir && !live_mir) pt.x = static_cast<int16_t>(-pt.x);
                                per_stream_extras[sid].push_back(pt);
                            }
                        }
                    }
                }
            }
        } else {
            // Fade complete (or no fade): clear crossfade state and render current cue at full intensity
            rs.prev_idx    = -1;
            rs.fade_in_dur = 0.f;
            rs.fade_elapsed = 0.f;

            // Start hold timer on the first tick after fade completes
            if (rs.hold_start < 0.0) rs.hold_start = 0.0;
            // H-2: only advance the hold timer while the main transport is
            // playing.  When paused (!playing_) the accumulator freezes so
            // Follow/Wait auto-advances do not fire while the show is paused.
            if (playing_)
                rs.hold_start += static_cast<double>(last_dt_);

            // Auto-advance when cue trigger type is Follow and hold has elapsed.
            // Respect timing.hold (the user-editable hold column); trigger.time_s
            // is a secondary override kept for legacy compatibility.
            {
                const FullCueEntry& cur_fce = pb->cuelist[static_cast<size_t>(rs.current_idx)];
                if (cur_fce.trigger.type == TriggerType::Follow) {
                    double follow_t = std::max(static_cast<double>(cur_fce.timing.hold),
                                               static_cast<double>(cur_fce.trigger.time_s));
                    if (rs.hold_start >= follow_t) {
                        int old_idx  = rs.current_idx;
                        int next_idx = old_idx + 1;
                        if (next_idx >= static_cast<int>(pb->cuelist.size())) {
                            if (pb->config.end_behavior == PlaybackConfig::EndBehavior::Loop)
                                next_idx = 0;
                            else
                                next_idx = old_idx; // stay on last cue
                        }
                        if (next_idx != old_idx) {
                            float old_fo = pb->cuelist[static_cast<size_t>(old_idx)].timing.fade_out;
                            rs.prev_idx    = old_idx;
                            rs.current_idx = next_idx;
                            rs.fade_in_dur = std::max(pb->cuelist[static_cast<size_t>(next_idx)].timing.fade_in, old_fo);
                            rs.fade_elapsed = 0.f;
                            rs.fade_alpha  = 0.f;
                            rs.prev_alpha  = 1.f;
                            rs.fade_start  = time_;
                            rs.hold_start  = -1.0;
                        }
                    }
                }
            }

            // ── go_at_bpm: advance cue on each BPM beat ──────────────────────
            if (pb->config.go_at_bpm && bpm_ > 0.f && rs.active && rs.current_idx >= 0) {
                rs.beat_phase += last_dt_ * bpm_ / 60.f;
                if (rs.beat_phase >= 1.f) {
                    rs.beat_phase -= 1.f;
                    // Fire GO (same logic as manual advance)
                    int old_idx  = rs.current_idx;
                    int next_idx = old_idx + 1;
                    if (next_idx >= static_cast<int>(pb->cuelist.size())) {
                        next_idx = (pb->config.end_behavior == PlaybackConfig::EndBehavior::Loop)
                                   ? 0 : old_idx;
                    }
                    if (next_idx != old_idx && old_idx >= 0 && old_idx < static_cast<int>(pb->cuelist.size())) {
                        float old_fo = pb->cuelist[static_cast<size_t>(old_idx)].timing.fade_out;
                        rs.prev_idx    = old_idx;
                        rs.current_idx = next_idx;
                        rs.fade_in_dur = std::max(pb->cuelist[static_cast<size_t>(next_idx)].timing.fade_in, old_fo);
                        rs.fade_elapsed = 0.f;
                        rs.hold_start  = -1.0;
                    }
                }
            }

            const FullCueEntry& fce = pb->cuelist[static_cast<size_t>(rs.current_idx)];
            if (fce.keyframe_layer.objects.empty() && fce.per_stream_kf.empty()) continue;
            const auto& assigned = pb->config.output_stream_ids;
            // Effective stream list: explicit assignment wins; otherwise all configured streams.
            std::vector<int> effective_ids;
            if (!assigned.empty()) {
                effective_ids = assigned;
            } else {
                for (const auto& s : output_streams_)
                    effective_ids.push_back(s.config.id);
            }
            if ((!fce.per_stream_fx.empty() || !fce.per_stream_kf.empty()) && !effective_ids.empty()) {
                // Per-stream path: only render streams that were explicitly programmed.
                // Streams absent from both per_stream_kf and per_stream_fx are skipped.
                for (int sid : effective_ids) {
                    auto kf_it = fce.per_stream_kf.find(sid);
                    auto fx_it = fce.per_stream_fx.find(sid);
                    if (kf_it == fce.per_stream_kf.end() && fx_it == fce.per_stream_fx.end())
                        continue;
                    const KeyframeLayer& stream_kf = (kf_it != fce.per_stream_kf.end())
                                                     ? kf_it->second : fce.keyframe_layer;
                    if (stream_kf.objects.empty()) continue;
                    PointBuffer stream_pts = render_keyframe_layer(stream_kf, programmer_point_budget());
                    const FxLayer& stream_fx = (fx_it != fce.per_stream_fx.end())
                                               ? fx_it->second : fce.fx_layer;
                    apply_frame_fx(stream_pts, stream_fx, fx_t);
                    apply_global_geometry(stream_pts, fce.global_layer);
                    apply_global_layer(stream_pts, fce.global_layer, fx_t, pb_intensity, rs.gfx_smooth);
                    // Apply per-cue mirroring only when live group mirroring is NOT
                    // already active for this stream.  If both applied simultaneously
                    // they double-invert (net no flip), which is the mirror-playback bug.
                    {
                        bool cue_mir  = !fce.mirrored_ids.empty() &&
                            std::find(fce.mirrored_ids.begin(), fce.mirrored_ids.end(), sid) != fce.mirrored_ids.end();
                        bool live_mir = !mirrored_stream_ids_.empty() &&
                            std::find(mirrored_stream_ids_.begin(), mirrored_stream_ids_.end(), sid) != mirrored_stream_ids_.end();
                        if (cue_mir && !live_mir)
                            for (auto& pt : stream_pts) pt.x = -pt.x;
                    }
                    for (const auto& pt : stream_pts)
                        per_stream_extras[sid].push_back(pt);
                }
            } else {
                // Global path: all streams get the same geometry and FX
                if (fce.keyframe_layer.objects.empty()) continue;
                PointBuffer pb_pts = render_keyframe_layer(fce.keyframe_layer, programmer_point_budget());
                apply_frame_fx(pb_pts, fce.fx_layer, fx_t);
                apply_global_geometry(pb_pts, fce.global_layer);
                apply_global_layer(pb_pts, fce.global_layer, fx_t, pb_intensity, rs.gfx_smooth);
                if (assigned.empty() && fce.mirrored_ids.empty()) {
                    // No per-stream routing needed: push to global composite buffer.
                    for (auto& pt : pb_pts)
                        points.push_back(pt);
                } else if (assigned.empty()) {
                    // No explicit assignment but per-cue mirroring is active: fan out
                    // per-stream so each mirrored stream receives a flipped copy.
                    // Guard: skip pre-flip for streams where live group mirror is active
                    // (the fanout will flip via mirrored_stream_ids_; pre-flipping here
                    // would double-invert to no flip — the mirror-playback bug).
                    for (int sid : effective_ids) {
                        bool is_mir  = std::find(fce.mirrored_ids.begin(),
                                                  fce.mirrored_ids.end(), sid)
                                       != fce.mirrored_ids.end();
                        bool live_mir = !mirrored_stream_ids_.empty() &&
                            std::find(mirrored_stream_ids_.begin(), mirrored_stream_ids_.end(), sid) != mirrored_stream_ids_.end();
                        if (!is_mir || live_mir) {
                            for (const auto& pt : pb_pts)
                                per_stream_extras[sid].push_back(pt);
                        } else {
                            for (auto pt : pb_pts) {
                                pt.x = static_cast<int16_t>(-pt.x);
                                per_stream_extras[sid].push_back(pt);
                            }
                        }
                    }
                } else {
                    for (int sid : assigned) {
                        bool is_mir  = !fce.mirrored_ids.empty() &&
                                       std::find(fce.mirrored_ids.begin(), fce.mirrored_ids.end(), sid) != fce.mirrored_ids.end();
                        bool live_mir = !mirrored_stream_ids_.empty() &&
                            std::find(mirrored_stream_ids_.begin(), mirrored_stream_ids_.end(), sid) != mirrored_stream_ids_.end();
                        for (auto pt : pb_pts) {   // copy by value
                            if (is_mir && !live_mir) pt.x = static_cast<int16_t>(-pt.x);
                            per_stream_extras[sid].push_back(pt);
                        }
                    }
                }
            }
        }
    }

    // ── Programmer output — highest priority, added last ─────────────────────
    // Programmer only runs when at least one stream is explicitly selected.
    // No streams selected → programmer outputs to NONE (per-head latch model).
    const bool programmer_will_run = programmer_active_ && !programmer_blind_ && !active_stream_ids_.empty();
    const bool has_stream_filter   = !active_stream_ids_.empty();
    // points_base (cue-only) is needed when the programmer goes to a subset of streams,
    // OR when any stream has latched programmer content that composites on top of the cue.
    const bool has_any_latch = std::any_of(stream_prog_.begin(), stream_prog_.end(),
                                            [](const auto& kv){ return kv.second.has_content; });
    const bool need_base = (programmer_will_run && has_stream_filter) || has_any_latch;
    PointBuffer points_base;
    if (need_base)
        points_base = points;

    // Programmer content is kept in a separate buffer so active streams receive
    // programmer-only frames (programmer always trumps playbacks). The same pts
    // are appended to `points` so the optimizer, scan-fail sampling, and preview
    // still see the full composite.
    PointBuffer programmer_pts_live;
    if (programmer_will_run) {
        programmer_pts_live = render_keyframe_layer(programmer_objects_,
                                                    programmer_point_budget());
        apply_frame_fx(programmer_pts_live, programmer_fx_layer_, static_cast<float>(fx_time_));
        apply_global_geometry(programmer_pts_live, programmer_global_layer_);
        apply_global_layer(programmer_pts_live, programmer_global_layer_,
                           static_cast<float>(fx_time_));
        for (auto& pt : programmer_pts_live)
            points.push_back(pt);
    }

    // ── Point optimizer on full composite (main cue + playbacks + programmer) ──
    points = point_optimizer_.optimize(points);

    // zone_bufs is computed after safety (below) so zone transforms see
    // safety-cleared master points.  Active streams in the fanout use this map
    // directly; non-active/latched streams have their zone transform applied
    // per-stream using zone_manager_.apply_transform() inside the fanout loop.
    std::unordered_map<int, PointBuffer> zone_bufs;

    // ── ArtNet DMX output: send all universes when enabled ────────────────
    if (artnet_out_enabled_ && stream_type_artnet_enabled_) {
        for (int u = 0; u < kMaxUniverses; ++u) {
            artnet_out_.send_universe(u + artnet_out_universe_offset_,
                                      dmx_[u].ch.data());
        }
    }

    // ── NDI software rasterization (legacy path) ─────────────────────────
    // Skipped when the multi-output patch contains at least one enabled NDI
    // stream — in that case each stream's own ndi_sender handles rasterization
    // inside the fanout loop below.  Running both paths simultaneously would
    // produce a duplicate NDI source with the same source name.
    {
        bool patch_has_ndi = false;
        for (const auto& s : output_streams_)
            if (s.config.type == OutputStreamType::NDI && s.config.enabled) { patch_has_ndi = true; break; }
        if (ndi_enabled_.load(std::memory_order_relaxed) && stream_type_ndi_enabled_ && !ndi_pixel_buf_.empty() && !patch_has_ndi) {
        // Gate sends to the configured NDI frame rate. Sending every engine tick
        // (1ms) with clock_video=true causes NDI to block the engine thread for
        // ~16ms per frame, dropping 1kHz to 60Hz and making GO feel sluggish.
        double ndi_fps_d = static_cast<double>(std::max(1, ndi_fps_N_))
                         / static_cast<double>(std::max(1, ndi_fps_D_));
        int ndi_period = std::max(1, static_cast<int>(1000.0 / ndi_fps_d + 0.5));
        if (frame_count_ % static_cast<uint64_t>(ndi_period) == 0) {
            raster_cfg_.width  = ndi_width_;
            raster_cfg_.height = ndi_height_;
            rasterize_frame(points, ndi_pixel_buf_.data(), raster_cfg_);
            // rasterize_frame outputs RGBA; NdiSender::send_bgra expects BGRA — swap R and B
            // Use int64 multiplication to avoid signed int32 overflow for large resolutions.
            int64_t px_count = static_cast<int64_t>(ndi_width_) * static_cast<int64_t>(ndi_height_);
            for (int64_t i = 0; i < px_count; ++i) {
                std::swap(ndi_pixel_buf_[static_cast<size_t>(i) * 4u + 0],
                          ndi_pixel_buf_[static_cast<size_t>(i) * 4u + 2]);
            }
            // Timecode: wall-clock 100ns units (1 engine tick = 1ms = 10000 × 100ns)
            int64_t tc = static_cast<int64_t>(frame_count_ & 0x7FFFFFFFFFFFFFFFULL) * 10000LL;
            ndi_sender_.send_bgra(ndi_pixel_buf_.data(), ndi_width_, ndi_height_, tc);
        }
        }  // end legacy NDI if (!patch_has_ndi)
    }  // end legacy NDI scope

    // ── Output kill switch ────────────────────────────────────────────────
    // Checked before safety so the UI kill switch yields a clean blank frame
    // (no scan-fail accumulation while intentionally dark).
    if (!output_enabled_) {
        for (LaserPoint& pt : points)
            pt.blanked = true;
    }

    // ── Safety (BAM + scan-fail + interlock) ─────────────────────────────
    // Applied LAST — after zone routing and playback compositing — so that
    // BAM checks and scan-fail sampling operate in the final output coordinate
    // space that the DAC will actually output.  This is safety-critical: the
    // BAM grid maps to physical output coordinates, not pre-zone-transform ones.
    safety_.apply(points, last_dt_ * 1000.f);

    // ── Safety Blackout Zones (HARD enforcement — last in chain) ──────────────
    // Must run AFTER safety_.apply() and BEFORE preview store/bus submission.
    // Points in safety blackout zones are ALWAYS blanked regardless of other settings.
    apply_safety_blackout(points);

    // ── Emergency Shutoff (absolute last — overrides everything) ─────────────
    if (emergency_shutoff_active_) {
        for (auto& pt : points)
            pt.blanked = true;
    }

    // Apply safety enforcement to the cue-only base frame (if any).
    if (need_base) {
        apply_safety_blackout(points_base);
        if (!output_enabled_)
            for (auto& pt : points_base) pt.blanked = true;
        if (emergency_shutoff_active_)
            for (auto& pt : points_base) pt.blanked = true;
    }

    // Apply same blanking rules to the live programmer frame used by active streams.
    // scan-fail sampling is covered by safety_.apply(points) above (points includes
    // programmer content), so we only need the discrete blanking gates here.
    if (programmer_will_run) {
        if (!output_enabled_)
            for (auto& pt : programmer_pts_live) pt.blanked = true;
        apply_safety_blackout(programmer_pts_live);
        if (emergency_shutoff_active_)
            for (auto& pt : programmer_pts_live) pt.blanked = true;
    }

    // Pre-compute per-stream latched programmer frames (programmer-only, frozen).
    // These are used in the fanout for streams that were deselected while the programmer
    // had content — their last programmer state is frozen on them until CLR.
    // The latched frame MUST reproduce exactly what the head showed while the stream was
    // still in active_stream_ids_, i.e. the programmer_pts_live frame built above: the
    // programmer content ONLY (no cue/playback base — programmer trumps), and NOT run
    // through point_optimizer_ (the live path optimizes only the separate `points`
    // buffer, never programmer_pts_live).  Prepending points_base or optimizing here was
    // the deselect "morph": it changed geometry, drawing order and point budget the
    // instant the stream lost selection.
    std::unordered_map<int, PointBuffer> latched_frames;
    if (has_any_latch) {
        for (const auto& [sid, sp] : stream_prog_) {
            if (!sp.has_content) continue;
            bool is_active = std::find(active_stream_ids_.begin(),
                                       active_stream_ids_.end(), sid) != active_stream_ids_.end();
            if (is_active) continue; // active stream gets live programmer instead
            PointBuffer lf = render_keyframe_layer(sp.objects, programmer_point_budget());
            apply_frame_fx(lf, sp.fx_layer, static_cast<float>(fx_time_));
            apply_global_geometry(lf, sp.global_layer);
            apply_global_layer(lf, sp.global_layer, static_cast<float>(fx_time_));
            // Same blanking/safety gates the active path applies to programmer_pts_live.
            if (!output_enabled_) for (auto& pt : lf) pt.blanked = true;
            apply_safety_blackout(lf);
            if (emergency_shutoff_active_) for (auto& pt : lf) pt.blanked = true;
            latched_frames[sid] = std::move(lf);
        }
    }

    // Apply global blanking rules to per-stream extras
    for (auto& [sid, buf] : per_stream_extras) {
        apply_safety_blackout(buf);
        if (!output_enabled_)      for (auto& pt : buf) pt.blanked = true;
        if (emergency_shutoff_active_) for (auto& pt : buf) pt.blanked = true;
    }

    // ── Zone routing (after safety, so all zones see safety-blanked content) ─
    // Each enabled zone gets a copy of `points` with its geometric/colour
    // transform applied.  The fanout loop below distributes these per-zone
    // copies to matching laser streams (zone N → laser stream N by list order).
    if (!zone_manager_.zones().empty())
        zone_bufs = zone_manager_.route(points);

    // ── Submit to render bus(es) ──────────────────────────────────────────
    // stream_defs_ is the authoritative list of output buses.  When non-empty
    // each enabled entry drives one output bus using its own per-stream
    // OutputTransform and OutputSafetyConfig.  When empty (no outputs patched),
    // no output is produced and nothing is submitted to any bus.
    //
    // preview_pts_ is populated from the full composited frame (programmer +
    // all playbacks + FX) before per-stream transforms so the 2D/3D preview
    // shows everything being output regardless of how many streams are patched.
    preview_pts_.clear();
    if (output_enabled_ && !emergency_shutoff_active_ && !points.empty()) {
        int step = std::max(1, static_cast<int>(points.size()) / 512);
        for (int i = 0; i < static_cast<int>(points.size()); i += step)
            preview_pts_.push_back(points[static_cast<size_t>(i)]);
    }

    if (!stream_defs_.empty()) {
        // Multi-output path: iterate stream_defs_ (authoritative bus list).
        // active_stream_ids_ controls which streams receive programmer content.
        // Empty = programmer off (no streams receive programmer); non-empty = only those IDs.
        bool submitted_to_legacy_bus = false;
        // True when at least one laser stream is assigned to the legacy bus (laser_idx==0).
        // Only submit a keepalive blank to bus_ when this is set; otherwise there is no
        // DacManager draining bus_ and the blank is wasted / could cause a ghost output.
        bool has_legacy_bus_laser = false;
        for (const auto& d : stream_defs_)
            if (d.type == OutputStreamType::Laser) { has_legacy_bus_laser = true; break; }

        // De-synchronize NDI sends across the 1000 Hz ticks. Without this every
        // NDI stream sharing the same fps shares the same ndi_period and fires on
        // the SAME ticks (frame_count_ % period == 0), so on those ticks ALL NDI
        // senders rasterize (1080p+) and call the clock_video send back-to-back on
        // the engine thread — a periodic multi-ms stall that scales with stream
        // count and starves the laser buses while average CPU stays low. Giving
        // each NDI stream a distinct phase offset spreads at most ceil(N/period)
        // sends onto any single tick instead of all N, keeping the 1 kHz cadence.
        int ndi_phase_seq = 0;

        for (size_t si = 0; si < stream_defs_.size(); ++si) {
            const OutputStreamDef& def = stream_defs_[si];
            if (!def.enabled) continue;

            // Find matching runtime entry (same id) for NDI sender / laser_bus ptr.
            OutputStreamRuntime* rt = nullptr;
            for (auto& os : output_streams_) {
                if (os.config.id == def.id) { rt = &os; break; }
            }

            // Active streams: programmer trumps — receive programmer-only frame when programmer is running.
            // Non-active streams get their latched frame (if any) or plain cue+playback composite.
            // No active streams (has_stream_filter=false) → programmer off, all get plain cue.
            const bool in_active_set = has_stream_filter &&
                std::find(active_stream_ids_.begin(), active_stream_ids_.end(),
                          def.id) != active_stream_ids_.end();

            const PointBuffer* src_ptr;
            if (in_active_set) {
                // Programmer trumps: active streams receive programmer-only frame,
                // not the cue+playbacks composite. Falls back to full composite when
                // programmer is not running (no content / no streams selected).
                src_ptr = programmer_will_run ? &programmer_pts_live : &points;
            } else if (need_base) {
                auto latch_it = latched_frames.find(def.id);
                src_ptr = (latch_it != latched_frames.end()) ? &latch_it->second : &points_base;
            } else {
                src_ptr = &points; // no filter, no latch — plain cue frame
            }
            const PointBuffer& src = *src_ptr;

            if (def.type == OutputStreamType::Laser) {
                // Master DAC enable gate — skip entirely when DAC stream type is disabled
                if (!stream_type_dac_enabled_) continue;
                // Resolve the laser bus: slot 0 → bus_, slot N → extra_laser_buses_[N-1]
                RenderBus* laser_bus = nullptr;
                if (rt && rt->laser_bus) {
                    laser_bus = rt->laser_bus;
                } else {
                    // Fallback: count laser streams before this one in stream_defs_
                    int laser_idx = 0;
                    for (size_t k = 0; k < si; ++k)
                        if (stream_defs_[k].type == OutputStreamType::Laser) ++laser_idx;
                    if (laser_idx == 0) {
                        laser_bus = &bus_;
                    } else {
                        // Guard bounds — extra_laser_buses_ is grown lazily
                        std::lock_guard<std::mutex> lk(extra_buses_mtx_);
                        int ei = laser_idx - 1;
                        if (ei < static_cast<int>(extra_laser_buses_.size()))
                            laser_bus = extra_laser_buses_[static_cast<size_t>(ei)].get();
                    }
                }
                if (!laser_bus) continue;

                // Zone-aware source: laser stream N always gets its zone N geometry,
                // regardless of whether it is currently active in the programmer.
                // Active streams use pre-routed zone_bufs (cue + programmer already baked in).
                // Non-active/latched streams have zone N's transform applied to their src
                // (latched frame or cue-only base) so each head keeps its own projection geometry.
                PointBuffer out;
                if (!zone_bufs.empty()) {
                    int lsi = 0;
                    for (size_t k = 0; k < si; ++k)
                        if (stream_defs_[k].type == OutputStreamType::Laser) ++lsi;
                    const std::vector<Zone>& zlist = zone_manager_.zones();
                    bool used_zone = false;
                    if (lsi < static_cast<int>(zlist.size())) {
                        if (in_active_set && programmer_will_run) {
                            // Programmer trumps: apply zone transform directly to
                            // programmer-only content so cue/playback is not visible.
                            const Zone& z = zlist[static_cast<size_t>(lsi)];
                            if (z.enabled && !z.blind) {
                                out = zone_manager_.apply_transform(programmer_pts_live, z.transform);
                                zone_manager_.apply_color(out, z.color_r, z.color_g, z.color_b, z.intensity);
                                used_zone = true;
                            }
                        } else if (in_active_set) {
                            // Active but programmer not running: use pre-routed zone buffer.
                            auto zit = zone_bufs.find(zlist[static_cast<size_t>(lsi)].id);
                            if (zit != zone_bufs.end()) {
                                out = zit->second;
                                used_zone = true;
                            }
                        } else {
                            // Non-active/latched: apply this zone's geometry+colour to src
                            // (latched content or cue-only base) so the head stays in its zone.
                            const Zone& z = zlist[static_cast<size_t>(lsi)];
                            if (z.enabled && !z.blind) {
                                out = zone_manager_.apply_transform(src, z.transform);
                                zone_manager_.apply_color(out, z.color_r, z.color_g, z.color_b, z.intensity);
                                used_zone = true;
                            }
                        }
                    }
                    if (!used_zone) out = src;
                } else {
                    out = src;
                }
                // Append playbacks assigned exclusively to this stream.
                // Suppressed when the programmer is active on this stream — programmer trumps.
                if (!(in_active_set && programmer_will_run)) {
                    auto eit = per_stream_extras.find(def.id);
                    if (eit != per_stream_extras.end())
                        for (const auto& pt : eit->second)
                            out.push_back(pt);
                }
                // Apply group mirror (X-flip) if this stream is in the mirrored set
                if (!mirrored_stream_ids_.empty()) {
                    bool is_mirrored = std::find(mirrored_stream_ids_.begin(),
                                                 mirrored_stream_ids_.end(),
                                                 def.id) != mirrored_stream_ids_.end();
                    if (is_mirrored) {
                        for (auto& pt : out)
                            pt.x = static_cast<int16_t>(-pt.x);
                    }
                }
                // Use per-stream transform and safety from OutputStreamDef
                apply_per_stream_transform(out, def.transform);

                // Downsample for 3D/UI preview BEFORE safety transforms so the
                // 3D preview always shows what the laser is drawing (unmasked).
                // Safety zones blank the hardware output but should not blank the
                // preview — the operator needs to see the content even when safety
                // zones are active.
                {
                    PointBuffer& pv = per_stream_preview_pts_[def.id];
                    pv.clear();
                    int step = std::max(1, static_cast<int>(out.size()) / 512);
                    for (int pi = 0; pi < static_cast<int>(out.size()); pi += step)
                        pv.push_back(out[static_cast<size_t>(pi)]);
                }

                apply_per_stream_safety(out, def.safety);

                RenderFrame frame;
                frame.points     = std::move(out);
                frame.sequence   = frame_count_;
                frame.timestamp  = time_;
                frame.point_rate = static_cast<uint32_t>(
                    def.point_rate > 0 ? def.point_rate : point_rate_);
                laser_bus->submit(std::move(frame));

                if (laser_bus == &bus_)
                    submitted_to_legacy_bus = true;

            } else if (def.type == OutputStreamType::NDI) {
                // NDI path: use runtime entry for sender and pixel buffer
                if (!stream_type_ndi_enabled_) continue;
                if (!rt || !rt->ndi_sender || rt->ndi_pixel_buf.empty()) continue;
                const auto& cfg = rt->config;
                double ndi_fps_d = static_cast<double>(std::max(1, cfg.ndi_fps_N))
                                 / static_cast<double>(std::max(1, cfg.ndi_fps_D));
                int ndi_period = std::max(1, static_cast<int>(1000.0 / ndi_fps_d + 0.5));
                // Distinct phase per NDI stream (stable across ticks since the
                // guards above are config-invariant) so same-fps streams no longer
                // all fire on the same tick. Rate is unchanged — each stream still
                // sends once every ndi_period ticks, just at a staggered offset.
                int ndi_phase = (ndi_phase_seq++) % ndi_period;
                if (frame_count_ % static_cast<uint64_t>(ndi_period) ==
                    static_cast<uint64_t>(ndi_phase)) {
                    PointBuffer out = src;
                    // Append playbacks assigned exclusively to this stream
                    {
                        auto eit = per_stream_extras.find(def.id);
                        if (eit != per_stream_extras.end())
                            for (const auto& pt : eit->second)
                                out.push_back(pt);
                    }
                    // Apply group mirror (X-flip) if this stream is in the mirrored set
                    if (!mirrored_stream_ids_.empty()) {
                        bool is_mirrored = std::find(mirrored_stream_ids_.begin(),
                                                     mirrored_stream_ids_.end(),
                                                     def.id) != mirrored_stream_ids_.end();
                        if (is_mirrored) {
                            for (auto& pt : out)
                                pt.x = static_cast<int16_t>(-pt.x);
                        }
                    }
                    // Use per-stream transform and safety from OutputStreamDef
                    apply_per_stream_transform(out, def.transform);
                    apply_per_stream_safety(out, def.safety);
                    RasterConfig rc = raster_cfg_;
                    rc.width  = cfg.ndi_width;
                    rc.height = cfg.ndi_height;
                    rasterize_frame(out, rt->ndi_pixel_buf.data(), rc);
                    int64_t px = static_cast<int64_t>(cfg.ndi_width) *
                                 static_cast<int64_t>(cfg.ndi_height);
                    for (int64_t pi = 0; pi < px; ++pi)
                        std::swap(rt->ndi_pixel_buf[static_cast<size_t>(pi)*4],
                                  rt->ndi_pixel_buf[static_cast<size_t>(pi)*4+2]);
                    int64_t tc = static_cast<int64_t>(frame_count_ & 0x7FFFFFFFFFFFFFFFULL) * 10000LL;
                    rt->ndi_sender->send_bgra(rt->ndi_pixel_buf.data(),
                                              cfg.ndi_width, cfg.ndi_height, tc);
                }
            }
            // Hdmi streams: points are available via preview_pts_ for the SDL window
        }

        // If a laser stream is wired to the legacy bus but didn't submit this tick
        // (e.g. it is disabled), send a blank keepalive so the DacManager doesn't stall.
        if (!submitted_to_legacy_bus && has_legacy_bus_laser) {
            RenderFrame blank;
            blank.points.push_back(LaserPoint{});
            blank.points.back().blanked = true;
            blank.sequence   = frame_count_;
            blank.timestamp  = time_;
            blank.point_rate = static_cast<uint32_t>(point_rate_);
            bus_.submit(std::move(blank));
        }
    }
    // When stream_defs_ is empty (no outputs patched) produce no output.
    // Nothing is submitted to any bus — this is intentional and safe.
}

// ─────────────────────────────────────────────────────────────────────────────
//  apply_safety_blackout — HARD blanking of safety zones (last in chain)
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::apply_safety_blackout(PointBuffer& pts)
{
    if (!safety_blackout_enabled_)
        return;

    const auto& bdr = safety_blackout_borders_;
    const bool  has_border = (bdr.left > 0.f || bdr.right > 0.f ||
                               bdr.top  > 0.f || bdr.bottom > 0.f);
    const bool  has_zones  = !safety_blackout_zones_.empty();

    if (!has_border && !has_zones)
        return;

    for (auto& pt : pts) {
        if (pt.blanked)
            continue; // already blanked — skip further processing

        const float nx = pt.nx();
        const float ny = pt.ny();

        // ── Border crop ───────────────────────────────────────────────────
        if (has_border) {
            // Convert pt from -1..1 to 0..1
            float u = (nx + 1.f) * 0.5f;
            float v = (ny + 1.f) * 0.5f;

            // Apply tilt: rotate (u,v) around (0.5,0.5) by -tilt_deg
            const float tilt_deg = bdr.tilt_deg;
            if (tilt_deg != 0.f) {
                const float tilt_rad = -tilt_deg * 3.14159265358979323846f / 180.f;
                const float du = u - 0.5f;
                const float dv = v - 0.5f;
                const float cs = std::cos(tilt_rad);
                const float sn = std::sin(tilt_rad);
                u =  du * cs - dv * sn + 0.5f;
                v =  du * sn + dv * cs + 0.5f;
            }

            if (u < bdr.left || u > 1.f - bdr.right ||
                v < bdr.top  || v > 1.f - bdr.bottom)
            {
                pt.blanked = true;
                continue;
            }
        }

        // ── Block zones ───────────────────────────────────────────────────
        if (has_zones) {
            for (const auto& zone : safety_blackout_zones_) {
                if (!zone.enabled)
                    continue;

                const float dx = nx - zone.cx;
                const float dy = ny - zone.cy;
                const float angle_rad = -zone.angle_deg * 3.14159265358979323846f / 180.f;
                const float cs = std::cos(angle_rad);
                const float sn = std::sin(angle_rad);
                const float lx =  dx * cs + dy * sn;
                const float ly = -dx * sn + dy * cs;

                if (std::abs(lx) <= zone.hw && std::abs(ly) <= zone.hh) {
                    pt.blanked = true;
                    break;
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  apply_per_stream_safety — per-output safety blackout (same logic as global)
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::apply_per_stream_safety(PointBuffer& pts,
                                          const OutputSafetyConfig& cfg) const
{
    if (!cfg.enabled) return;

    const bool has_border = (cfg.border_left > 0.f || cfg.border_right > 0.f ||
                              cfg.border_top  > 0.f || cfg.border_bottom > 0.f);
    const bool has_zones  = !cfg.zones.empty();
    if (!has_border && !has_zones) return;

    static constexpr float kPi = 3.14159265358979323846f;

    for (auto& pt : pts) {
        if (pt.blanked) continue;

        const float nx = pt.nx();
        const float ny = pt.ny();

        if (has_border) {
            float u = (nx + 1.f) * 0.5f;
            float v = (ny + 1.f) * 0.5f;
            if (cfg.border_tilt != 0.f) {
                const float rad = -cfg.border_tilt * kPi / 180.f;
                const float du = u - 0.5f, dv = v - 0.5f;
                const float cs = std::cos(rad), sn = std::sin(rad);
                u = du * cs - dv * sn + 0.5f;
                v = du * sn + dv * cs + 0.5f;
            }
            if (u < cfg.border_left || u > 1.f - cfg.border_right ||
                v < cfg.border_top  || v > 1.f - cfg.border_bottom) {
                pt.blanked = true;
                continue;
            }
        }

        if (has_zones) {
            for (const auto& z : cfg.zones) {
                if (!z.enabled) continue;
                const float dx = nx - z.cx, dy = ny - z.cy;
                const float ar  = -z.angle_deg * kPi / 180.f;
                const float cs  = std::cos(ar), sn = std::sin(ar);
                const float lx  =  dx * cs + dy * sn;
                const float ly  = -dx * sn + dy * cs;
                if (std::abs(lx) <= z.hw && std::abs(ly) <= z.hh) {
                    pt.blanked = true;
                    break;
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  apply_per_stream_transform — translate, scale, rotate, flip
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::apply_per_stream_transform(PointBuffer& pts,
                                             const OutputTransform& xf) const
{
    // If transform is identity, skip
    if (xf.offset_x == 0.f && xf.offset_y == 0.f &&
        xf.scale_x  == 1.f && xf.scale_y  == 1.f &&
        xf.rotation == 0.f && !xf.flip_x  && !xf.flip_y)
        return;

    static constexpr float kPi = 3.14159265358979323846f;
    const float rad = xf.rotation * kPi / 180.f;
    const float cs  = std::cos(rad);
    const float sn  = std::sin(rad);

    for (auto& pt : pts) {
        float fx = pt.nx() * xf.scale_x;
        float fy = pt.ny() * xf.scale_y;
        if (xf.flip_x) fx = -fx;
        if (xf.flip_y) fy = -fy;
        // Rotate
        float rx = fx * cs - fy * sn;
        float ry = fx * sn + fy * cs;
        // Translate
        rx += xf.offset_x;
        ry += xf.offset_y;
        pt.x = static_cast<int16_t>(std::clamp(rx, -1.f, 1.f) * 32767.f);
        pt.y = static_cast<int16_t>(std::clamp(ry, -1.f, 1.f) * 32767.f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  extra_laser_bus — return a pointer to extra bus slot i (1-based for extras)
// ─────────────────────────────────────────────────────────────────────────────
RenderBus* ShowEngine::extra_laser_bus(int stream_index)
{
    if (stream_index <= 0) return &bus_;
    int ei = stream_index - 1;
    std::lock_guard<std::mutex> lk(extra_buses_mtx_);
    while (static_cast<int>(extra_laser_buses_.size()) <= ei)
        extra_laser_buses_.push_back(std::make_unique<RenderBus>());
    return extra_laser_buses_[static_cast<size_t>(ei)].get();
}

// ─────────────────────────────────────────────────────────────────────────────
//  output_stream_configs — snapshot for UI
// ─────────────────────────────────────────────────────────────────────────────
std::vector<OutputStreamConfig> ShowEngine::output_stream_configs() const
{
    // C-3: output_streams_ is written in process_commands() which holds
    // project_access_mtx_.  snap_mtx_ is only held during snapshot flips and
    // is NOT acquired when writing output_streams_, so using snap_mtx_ here
    // left a data race.  Use project_access_mtx_ to match the write side.
    std::lock_guard<std::mutex> lk(project_access_mtx_);
    std::vector<OutputStreamConfig> out;
    out.reserve(output_streams_.size());
    for (const auto& s : output_streams_)
        out.push_back(s.config);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  update_snapshot — write the double-buffered UI snapshot
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::update_snapshot(double fps)
{
    int wi = snap_write_idx_.load(std::memory_order_relaxed);

    EngineSnapshot& s = snap_[wi];
    s.time             = time_;
    s.loop_end         = loop_end_;
    s.playing          = playing_;
    s.master_intensity = master_;
    s.bpm              = bpm_;
    s.point_rate       = point_rate_;
    s.active_cue       = active_cue_;
    s.cue_list_idx     = cue_list_idx_;
    s.engine_fps       = static_cast<float>(fps);
    s.frame_count      = frame_count_;
    s.audio            = audio_.snapshot();
    s.dmx[0]           = dmx_[0];
    if constexpr (kMaxUniverses > 1) s.dmx[1] = dmx_[1];

    // Use the final composited frame (programmer + playbacks) for the preview
    s.preview_points = preview_pts_;

    // ── Playback snapshots ────────────────────────────────────────────────
    s.playback_count = 0;
    if (project_) {
        for (const auto& pb : project_->playbacks) {
            if (s.playback_count >= EngineSnapshot::kMaxPlaybacks) break;
            auto& snap = s.playbacks[s.playback_count++];
            snap.id        = pb.id;
            snap.name      = pb.name;
            snap.active    = pb.active;
            snap.blind     = pb.blind;
            snap.intensity = pb.intensity;
            snap.cue_count = static_cast<int>(pb.cuelist.size());
            snap.current_cue      = -1;
            snap.fade_level       = 0.f;
            snap.current_cue_name.clear();
            for (const auto& rs : playback_states_) {
                if (rs.id == pb.id) {
                    snap.current_cue  = rs.current_idx;
                    snap.fade_level   = rs.fade_level;
                    snap.fade_in_dur  = rs.fade_in_dur;
                    snap.fade_elapsed = rs.fade_elapsed;
                    if (rs.current_idx >= 0 && rs.current_idx < snap.cue_count)
                        snap.current_cue_name = pb.cuelist[static_cast<size_t>(rs.current_idx)].name;
                    break;
                }
            }
        }
    }

    // ── FX block snapshot ─────────────────────────────────────────────────
    {
        int active = active_cue_;
        if (active >= 0 && active < static_cast<int>(cue_fx_engines_.size())) {
            FxEngine& eng = cue_fx_engines_[static_cast<size_t>(active)];
            int bc = eng.block_count();
            s.fx_block_count = (bc < EngineSnapshot::kMaxFxBlocks) ? bc : EngineSnapshot::kMaxFxBlocks;
            for (int bi = 0; bi < s.fx_block_count; ++bi) {
                IFxBlock* blk = eng.block_at(bi);
                if (!blk) { s.fx_blocks[bi] = {}; continue; }
                auto& snap = s.fx_blocks[bi];
                std::strncpy(snap.name,     blk->name(),     63); snap.name[63]     = '\0';
                std::strncpy(snap.category, blk->category(), 31); snap.category[31] = '\0';
                snap.enabled  = blk->enabled;
                snap.bypassed = blk->bypassed;
                snap.wet      = blk->wet;
                auto plist = blk->params();
                int pc = static_cast<int>(plist.size());
                snap.param_count = (pc < 16) ? pc : 16;
                for (int pi = 0; pi < snap.param_count; ++pi) {
                    std::strncpy(snap.params[pi].name, plist[static_cast<size_t>(pi)]->name.c_str(), 31);
                    snap.params[pi].name[31]        = '\0';
                    snap.params[pi].effective_val   = plist[static_cast<size_t>(pi)]->effective();
                    snap.params[pi].base_val        = plist[static_cast<size_t>(pi)]->base_value;
                }
            }
        } else {
            s.fx_block_count = 0;
        }
    }

    // ── Cue list state ────────────────────────────────────────────────────
    s.cuelist_entry_count  = cue_list_.entry_count();
    s.cuelist_current_idx  = cue_list_.current_idx();
    s.cuelist_fade_level   = cue_list_.fade_level();
    s.record_mode          = record_mode_;

    // ── MIDI learn state ──────────────────────────────────────────────────
    s.midi_learn_armed     = midi_learn_.is_armed();
    s.midi_learn_target    = midi_learn_.armed_target();
    s.active_color_slot    = active_color_slot_;
    s.active_position_slot = active_position_slot_;

    // ── Next cue name (for GO button tooltip) ─────────────────────────────
    {
        int ni = cue_list_.next_idx();
        const FullCueEntry* nfe = cue_list_.entry_at_safe(ni);
        if (nfe)
            s.next_cue_name = nfe->name;
        else
            s.next_cue_name.clear();
    }

    // ── DAC hot-plug snapshot ─────────────────────────────────────────────
    s.available_dacs   = dac_registry_.list();
    s.dac_hot_plugged  = dac_changed_.exchange(false, std::memory_order_acq_rel);

    // ── Zone snapshot ─────────────────────────────────────────────────────
    s.zones = zone_manager_.zones();

    // ── Quick Show snapshot ───────────────────────────────────────────────
    for (int pg = 0; pg < 32; ++pg)
        for (int r = 0; r < 6; ++r)
            for (int col = 0; col < 10; ++col)
                s.quickshow_slots[pg * 60 + r * 10 + col] =
                    quickshow_[pg][r][col].cue_idx;

    // Safety snapshot
    s.safety_ok     = safety_.is_safe();
    s.safety_status = safety_.status_string();
    s.bam_enabled          = safety_.bam.enabled;
    s.artnet_out_enabled   = artnet_out_enabled_;
    s.ndi_active           = ndi_enabled_.load(std::memory_order_relaxed) && ndi_sender_.is_initialized();
    // Poll connection count here so it stays fresh even when no frames are being sent.
    ndi_sender_.poll_connections();
    s.ndi_connections      = ndi_sender_.num_connections();

    // Multi-output stream snapshot
    s.output_streams.clear();
    s.output_streams.reserve(output_streams_.size());
    for (auto& stream : output_streams_) {
        EngineSnapshot::OutputStreamSnap snap;
        snap.id          = stream.config.id;
        snap.name        = stream.config.name;
        snap.type        = stream.config.type;
        snap.enabled     = stream.config.enabled;
        snap.dac_type    = stream.config.dac_type;
        snap.dac_address = stream.config.dac_address;
        if (stream.ndi_sender) {
            stream.ndi_sender->poll_connections();
            snap.ndi_active = stream.ndi_sender->is_initialized();
            snap.ndi_conns  = stream.ndi_sender->num_connections();
        }
        s.output_streams.push_back(std::move(snap));
    }
    s.active_stream_ids = active_stream_ids_;

    // Expose stream_defs_ to UI (current patch state)
    s.stream_defs = stream_defs_;

    // Per-output frame snapshot: one entry per enabled stream.
    // Each stream uses its own post-transform downsampled preview if available,
    // falling back to the global composite for streams not yet captured.
    {
        s.output_frames.clear();
        s.output_frames.reserve(output_streams_.size());
        for (const auto& stream : output_streams_) {
            if (!stream.config.enabled) continue;
            EngineSnapshot::OutputFrame of;
            of.stream_id = stream.config.id;
            auto it = per_stream_preview_pts_.find(stream.config.id);
            of.points = (it != per_stream_preview_pts_.end())
                      ? it->second
                      : preview_pts_;
            s.output_frames.push_back(std::move(of));
        }
    }

    // Programmer state
    s.programmer_active = programmer_active_;
    s.programmer_blind  = programmer_blind_;

    // Chaser state
    s.cuelist_version   = cuelist_version_.load(std::memory_order_relaxed);
    s.chaser_active     = chaser_run_.has_value();
    s.chaser_step       = chaser_run_ ? chaser_run_->step : 0;
    s.chaser_step_count = 0;
    if (project_ && active_cue_ >= 0 && active_cue_ < static_cast<int>(project_->cues.size())) {
        const std::string& target_id = project_->cues[static_cast<size_t>(active_cue_)].id;
        // Use cue_list_ (engine-thread-owned) instead of project_->full_cue_list to
        // avoid racing with the UI save path that copies project_->full_cue_list.
        for (int ei = 0; ei < cue_list_.entry_count(); ++ei) {
            const auto& fce = cue_list_.entry_at(ei);
            if (fce.cue_id == target_id && fce.is_chaser) {
                s.chaser_step_count = static_cast<int>(fce.chaser_steps.size());
                break;
            }
        }
    }

    const auto& grid = safety_.bam.grid();
    static_assert(sizeof(s.bam_cells) == sizeof(grid.cells), "BAM grid size mismatch");
    std::memcpy(s.bam_cells, grid.cells, sizeof(grid.cells));

    // ── Timecode snapshot ─────────────────────────────────────────────────────
    s.timecode = timecode_router_.active();

    // ── Timeline snapshots ────────────────────────────────────────────────────
    {
        s.timelines.clear();
        const TimecodeState tc_now = timecode_router_.active();
        const auto& defs = timeline_engine_.defs();
        const auto  rsnaps = timeline_engine_.runtime_snaps();
        // Keep the read cache current so read_timelines() returns live data
        {
            std::lock_guard<std::mutex> lk(timeline_read_mtx_);
            timeline_read_cache_ = defs;
        }
        for (size_t ti = 0; ti < defs.size(); ++ti) {
            const TimelineDef& def = defs[ti];
            EngineSnapshot::TimelineSnap ts;
            ts.id             = def.id;
            ts.name           = def.name;
            ts.fps            = def.fps;
            ts.length_frames  = def.length_frames;
            ts.tc_slot        = def.tc_slot;
            ts.link_mode      = def.link_mode;
            ts.record_armed   = def.record_armed;
            ts.track_count    = static_cast<int>(def.tracks.size());
            ts.tracks         = def.tracks;
            ts.audio_track    = def.audio_track;
            ts.audio_peaks    = def.audio_peaks;
            if (ti < rsnaps.size()) {
                ts.state           = rsnaps[ti].state;
                ts.position_frames = rsnaps[ti].position_frames;
            }
            ts.source_status  = timeline_engine_.source_status(tc_now, def.tc_slot);
            s.timelines.push_back(std::move(ts));
        }
        s.tc_config = tc_config_;
    }

    // Flip write index — reader always gets 1-snap_write_idx_
    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        snap_write_idx_.store(1 - wi, std::memory_order_release);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  sync_cuelist_to_project — write live CueList back to project->full_cue_list
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::sync_cuelist_to_project()
{
    if (!project_) return;

    // Build the updated list before assigning so project_->full_cue_list is
    // never visible in a partially-populated state to a concurrent UI-thread read.
    std::vector<FullCueEntry> updated;
    updated.reserve(static_cast<size_t>(cue_list_.entry_count()));
    for (int i = 0; i < cue_list_.entry_count(); ++i)
        updated.push_back(cue_list_.entry_at(i));
    project_->full_cue_list = std::move(updated);

    {
        std::lock_guard<std::mutex> lock(cuelist_read_mtx_);
        cuelist_read_cache_ = project_->full_cue_list;
        pb_cuelist_cache_.clear();
        for (const auto& pb : project_->playbacks)
            pb_cuelist_cache_[pb.id] = pb.cuelist;
    }
    cuelist_version_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<FullCueEntry> ShowEngine::read_full_cue_list() const {
    std::lock_guard<std::mutex> lock(cuelist_read_mtx_);
    return cuelist_read_cache_;
}

std::vector<FullCueEntry> ShowEngine::read_playback_cuelist(int pb_id) const {
    std::lock_guard<std::mutex> lock(cuelist_read_mtx_);
    auto it = pb_cuelist_cache_.find(pb_id);
    if (it != pb_cuelist_cache_.end()) return it->second;
    return {};
}

std::vector<TimelineDef> ShowEngine::read_timelines() const {
    std::lock_guard<std::mutex> lock(timeline_read_mtx_);
    return timeline_read_cache_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  snapshot_project_for_save — thread-safe project copy for UI save / auto-save
//
//  Holds project_access_mtx_ while copying *project_ so that process_commands()
//  (the only writer of project_->full_cue_list and project_->playbacks[n].cuelist)
//  cannot run concurrently.  After the copy the UI can freely call copy.save()
//  without any shared state remaining in play.
// ─────────────────────────────────────────────────────────────────────────────
Project ShowEngine::snapshot_project_for_save() const
{
    std::lock_guard<std::mutex> lk(project_access_mtx_);
    if (!project_) return {};
    return *project_;          // deep copy — safe while command handlers are blocked
}

// ─────────────────────────────────────────────────────────────────────────────
//  tick — (used internally; public interface is engine_loop)
// ─────────────────────────────────────────────────────────────────────────────
void ShowEngine::tick(double dt)
{
    // This is the broken-out tick path called from engine_loop.
    // All logic already inlined in engine_loop for performance; kept for
    // possible external test harness use.
    (void)dt;
}

// ─────────────────────────────────────────────────────────────────────────────
//  EngineWatchdog
// ─────────────────────────────────────────────────────────────────────────────
EngineWatchdog::EngineWatchdog(ShowEngine& engine)
    : engine_(engine)
{}

EngineWatchdog::~EngineWatchdog()
{
    stop();
}

void EngineWatchdog::start()
{
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&EngineWatchdog::watch_loop, this);
    log::info("EngineWatchdog started");
}

void EngineWatchdog::stop()
{
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    log::info("EngineWatchdog stopped");
}

void EngineWatchdog::watch_loop()
{
    set_thread_name("idhmfis-watchdog");

    uint64_t last_heartbeat = engine_.watchdog_heartbeat();

    while (running_.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kStallThresholdMs));

        uint64_t current = engine_.watchdog_heartbeat();
        if (current == last_heartbeat && engine_.is_running())
        {
            log::warn("EngineWatchdog: engine thread stalled (heartbeat=%llu) "
                      "— attempting restart",
                      static_cast<unsigned long long>(current));
            // Attempt restart with a time-limited join to avoid blocking
            // forever if the engine thread is truly frozen.
            engine_.restart();
        }
        last_heartbeat = current;
    }
}

} // namespace idhmfis
