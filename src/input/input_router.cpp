// input_router.cpp — Input aggregation and translation to engine commands
//
// tick() is designed to be called at 1000 Hz from the engine thread.
// All three queues are drained completely each tick.
// Engine commands are dispatched via ShowEngine::send() and ShowEngine::push_dmx().

#include "input_router.h"
#include "../core/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <optional>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
InputRouter::InputRouter(ShowEngine& engine)
    : engine_(engine)
    , dmx_queue_(1024)
    , midi_queue_(512)
    , osc_queue_(512)
    , artnet_(dmx_queue_)
    , sacn_(dmx_queue_)
    , midi_input_(midi_queue_)
    , osc_server_(osc_queue_, 8000)
{}

InputRouter::~InputRouter() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  start / stop
// ─────────────────────────────────────────────────────────────────────────────
void InputRouter::start(const ArtNetConfig& artnet_cfg, int osc_port) {
    // Start ArtNet listener
    if (!artnet_.start(artnet_cfg)) {
        log::warn("InputRouter: ArtNet listener failed to start");
    }

    // Start sACN listener (unicast only by default)
    if (!sacn_.start("", {})) {
        log::warn("InputRouter: sACN listener failed to start");
    }

    // OSC port is set at construction; reinitialise with requested port
    // Note: OscServer port is fixed at construction; caller should pass
    // the correct port to InputRouter if non-default is needed.
    (void)osc_port;  // osc_server_ was constructed with port from caller
    if (!osc_server_.start()) {
        log::warn("InputRouter: OSC server failed to start");
    }

    // MIDI: don't auto-open — caller calls midi().open(portName) explicitly
    log::info("InputRouter: started. Use midi().open() to connect MIDI device.");

    // Start the self-tick thread.  tick() drains all input queues and forwards
    // DMX/MIDI/OSC data to the engine via thread-safe MPSC queues.
    // This thread MUST be started after the listeners so that it can safely
    // forward to an already-running engine; it is stopped before the listeners
    // in stop() so it never touches a half-torn-down listener.
    tick_running_.store(true, std::memory_order_release);
    tick_thread_ = std::thread([this]() {
        using namespace std::chrono_literals;
        while (tick_running_.load(std::memory_order_relaxed)) {
            tick();
            std::this_thread::sleep_for(1ms);   // ~1 kHz drain rate
        }
    });
    log::info("InputRouter: tick thread started");
}

void InputRouter::stop() {
    // Stop the self-tick thread FIRST — it pushes into the engine's queues,
    // so it must be quiesced before any listener or engine teardown begins.
    tick_running_.store(false, std::memory_order_release);
    if (tick_thread_.joinable())
        tick_thread_.join();

    artnet_.stop();
    sacn_.stop();
    midi_input_.close();
    osc_server_.stop();
    log::info("InputRouter: stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
//  tick() — drain all queues
// ─────────────────────────────────────────────────────────────────────────────
void InputRouter::tick() {
    process_dmx();
    process_midi();
    process_osc();
}

// ─────────────────────────────────────────────────────────────────────────────
//  DMX processing — forward universe data directly to engine
// ─────────────────────────────────────────────────────────────────────────────
void InputRouter::process_dmx() {
    std::pair<int, DmxUniverse> item;
    while (dmx_queue_.try_pop(item)) {
        engine_.push_dmx(item.first, item.second);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  MIDI processing
// ─────────────────────────────────────────────────────────────────────────────
void InputRouter::process_midi() {
    MidiMessage msg;
    while (midi_queue_.try_pop(msg)) {
        EngineCommand cmd = cmd::Stop{};  // default; overwritten by midi_to_command
        if (midi_to_command(msg, cmd)) {
            engine_.send(cmd);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  OSC processing
// ─────────────────────────────────────────────────────────────────────────────
void InputRouter::process_osc() {
    OscServer::Message msg;
    while (osc_queue_.try_pop(msg)) {
        EngineCommand cmd = cmd::Stop{};  // default; overwritten by osc_to_command
        if (osc_to_command(msg, cmd)) {
            engine_.send(cmd);
        } else {
            log::debug("InputRouter: unrecognized OSC path '%s'", msg.path.c_str());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  OSC → EngineCommand translation table
//
//  Paths are matched case-sensitively against the spec in input_router.h.
//  The function returns false for any unrecognized path.
// ─────────────────────────────────────────────────────────────────────────────
bool InputRouter::osc_to_command(const OscServer::Message& msg,
                                  EngineCommand& out_cmd) {
    const std::string& p = msg.path;

    if (p == "/idhmfis/play") {
        out_cmd = cmd::Play{};
        return true;
    }

    if (p == "/idhmfis/stop") {
        out_cmd = cmd::Stop{};
        return true;
    }

    if (p == "/idhmfis/pause") {
        out_cmd = cmd::Pause{};
        return true;
    }

    if (p == "/idhmfis/intensity") {
        float v = msg.args_f.empty() ? 1.f : msg.args_f[0];
        v = std::clamp(v, 0.f, 1.f);
        out_cmd = cmd::SetMasterIntensity{v};
        return true;
    }

    if (p == "/idhmfis/bpm") {
        float bpm = msg.args_f.empty()
                    ? (msg.args_i.empty() ? 120.f : static_cast<float>(msg.args_i[0]))
                    : msg.args_f[0];
        bpm = std::clamp(bpm, 20.f, 2000.f);
        out_cmd = cmd::SetBPM{bpm};
        return true;
    }

    if (p == "/idhmfis/pointrate") {
        int pps = msg.args_i.empty()
                  ? (msg.args_f.empty() ? kDefaultPointRate
                                        : static_cast<int>(msg.args_f[0]))
                  : msg.args_i[0];
        pps = std::clamp(pps, kMinPointRate, kMaxPointRate);
        out_cmd = cmd::SetPointRate{pps};
        return true;
    }

    if (p == "/idhmfis/cue/activate") {
        int   slot = msg.args_i.empty()
                     ? (msg.args_f.empty() ? 0 : static_cast<int>(msg.args_f[0]))
                     : msg.args_i[0];
        float fade = msg.args_f.size() >= 2 ? msg.args_f[1]
                   : (msg.args_f.size() >= 1 ? 0.5f : 0.5f);
        out_cmd = cmd::ActivateCue{slot, fade};
        return true;
    }

    if (p == "/idhmfis/cue/deactivate") {
        int slot = msg.args_i.empty()
                   ? (msg.args_f.empty() ? 0 : static_cast<int>(msg.args_f[0]))
                   : msg.args_i[0];
        out_cmd = cmd::DeactivateCue{slot, 0.5f};
        return true;
    }

    if (p == "/idhmfis/param") {
        // /idhmfis/param <string:name> <float:value>
        if (!msg.args_s.empty() && !msg.args_f.empty()) {
            out_cmd = cmd::SetGeneratorParam{0, msg.args_s[0], msg.args_f[0]};
            return true;
        }
        return false;
    }

    if (p == "/idhmfis/beam_thickness") {
        float v = msg.args_f.empty() ? 2.0f : msg.args_f[0];
        out_cmd = cmd::SetBeamThickness{v};
        return true;
    }

    if (p == "/idhmfis/bloom_radius") {
        float v = msg.args_f.empty() ? 8.0f : msg.args_f[0];
        out_cmd = cmd::SetBloomRadius{v};
        return true;
    }

    if (p == "/idhmfis/haze_density") {
        float v = msg.args_f.empty() ? 0.4f : msg.args_f[0];
        out_cmd = cmd::SetHazeDensity{v};
        return true;
    }

    if (p == "/idhmfis/exposure") {
        float v = msg.args_f.empty() ? 1.0f : msg.args_f[0];
        out_cmd = cmd::SetExposure{v};
        return true;
    }

    // ── Cue list transport shortcuts ─────────────────────────────────────────
    if (p == "/idhmfis/go" || p == "/idhmfis/cue/go") {
        out_cmd = cmd::CueListGo{};
        return true;
    }

    if (p == "/idhmfis/back" || p == "/idhmfis/cue/back") {
        out_cmd = cmd::CueListBack{};
        return true;
    }

    if (p == "/idhmfis/jump" || p == "/idhmfis/cue/jump") {
        int major = msg.args_i.size() >= 1 ? msg.args_i[0] : 0;
        int minor = msg.args_i.size() >= 2 ? msg.args_i[1] : 0;
        out_cmd = cmd::CueListJump{major, minor};
        return true;
    }

    // ── Master intensity alias ────────────────────────────────────────────────
    if (p == "/idhmfis/master") {
        float v = msg.args_f.empty() ? 1.f : msg.args_f[0];
        v = std::clamp(v, 0.f, 1.f);
        out_cmd = cmd::SetMasterIntensity{v};
        return true;
    }

    // ── Beam / bloom visual params ────────────────────────────────────────────
    if (p == "/idhmfis/beam/thickness") {
        float v = msg.args_f.empty() ? 2.0f : msg.args_f[0];
        out_cmd = cmd::SetBeamThickness{v};
        return true;
    }

    if (p == "/idhmfis/beam/bloom") {
        float v = msg.args_f.empty() ? 8.0f : msg.args_f[0];
        out_cmd = cmd::SetBloomRadius{v};
        return true;
    }

    // ── FX engine routes ─────────────────────────────────────────────────────
    if (p == "/idhmfis/fx/set") {
        // /idhmfis/fx/set i i s f
        int  ci  = msg.args_i.size() >= 1 ? msg.args_i[0] : 0;
        int  fi  = msg.args_i.size() >= 2 ? msg.args_i[1] : 0;
        if (!msg.args_s.empty() && !msg.args_f.empty()) {
            out_cmd = cmd::SetFxParam{ci, fi, msg.args_s[0], msg.args_f[0]};
            return true;
        }
        return false;
    }

    if (p == "/idhmfis/fx/enable") {
        // /idhmfis/fx/enable i i i
        int  ci = msg.args_i.size() >= 1 ? msg.args_i[0] : 0;
        int  fi = msg.args_i.size() >= 2 ? msg.args_i[1] : 0;
        bool en = msg.args_i.size() >= 3 ? (msg.args_i[2] != 0) : true;
        out_cmd = cmd::SetFxEnabled{ci, fi, en};
        return true;
    }

    if (p == "/idhmfis/fx/bypass") {
        // /idhmfis/fx/bypass i i i
        int  ci = msg.args_i.size() >= 1 ? msg.args_i[0] : 0;
        int  fi = msg.args_i.size() >= 2 ? msg.args_i[1] : 0;
        bool bp = msg.args_i.size() >= 3 ? (msg.args_i[2] != 0) : true;
        out_cmd = cmd::SetFxBypassed{ci, fi, bp};
        return true;
    }

    if (p == "/idhmfis/fx/wet") {
        // /idhmfis/fx/wet i i f
        int   ci  = msg.args_i.size() >= 1 ? msg.args_i[0] : 0;
        int   fi  = msg.args_i.size() >= 2 ? msg.args_i[1] : 0;
        float wet = msg.args_f.empty() ? 1.f : msg.args_f[0];
        out_cmd = cmd::SetFxWet{ci, fi, wet};
        return true;
    }

    // ── Quick Show ───────────────────────────────────────────────────────────
    if (p == "/idhmfis/quickshow/trigger") {
        // /idhmfis/quickshow/trigger i i i  (page, row, col)
        int page = msg.args_i.size() >= 1 ? msg.args_i[0] : 0;
        int row  = msg.args_i.size() >= 2 ? msg.args_i[1] : 0;
        int col  = msg.args_i.size() >= 3 ? msg.args_i[2] : 0;
        out_cmd = cmd::QuickShowTrigger{page, row, col};
        return true;
    }

    if (p == "/idhmfis/quickshow/assign") {
        // /idhmfis/quickshow/assign i i i i  (page, row, col, cue_idx)
        int page    = msg.args_i.size() >= 1 ? msg.args_i[0] : 0;
        int row     = msg.args_i.size() >= 2 ? msg.args_i[1] : 0;
        int col     = msg.args_i.size() >= 3 ? msg.args_i[2] : 0;
        int cue_idx = msg.args_i.size() >= 4 ? msg.args_i[3] : 0;
        out_cmd = cmd::QuickShowAssign{page, row, col, cue_idx};
        return true;
    }

    // ── LivePRO ──────────────────────────────────────────────────────────────
    if (p == "/idhmfis/livepro/xy") {
        float x = msg.args_f.size() >= 1 ? msg.args_f[0] : 0.f;
        float y = msg.args_f.size() >= 2 ? msg.args_f[1] : 0.f;
        out_cmd = cmd::LiveProXY{x, y};
        return true;
    }

    if (p == "/idhmfis/livepro/scale") {
        float s = msg.args_f.empty() ? 1.f : msg.args_f[0];
        out_cmd = cmd::LiveProScale{s};
        return true;
    }

    if (p == "/idhmfis/livepro/speed") {
        float s = msg.args_f.empty() ? 1.f : msg.args_f[0];
        out_cmd = cmd::LiveProSpeed{s};
        return true;
    }

    if (p == "/idhmfis/livepro/beat") {
        int slot = msg.args_i.empty() ? 0 : msg.args_i[0];
        out_cmd = cmd::LiveProBeat{slot};
        return true;
    }

    // ── Macro ────────────────────────────────────────────────────────────────
    if (p == "/idhmfis/macro") {
        int slot = msg.args_i.empty() ? 0 : msg.args_i[0];
        out_cmd = cmd::TriggerMacro{slot};
        return true;
    }

    // ── Output shortcut (blackout / full) ────────────────────────────────────
    if (p == "/idhmfis/output") {
        // 0 = blackout, non-zero = full
        float v = 0.f;
        if (!msg.args_i.empty())
            v = (msg.args_i[0] != 0) ? 1.f : 0.f;
        else if (!msg.args_f.empty())
            v = std::clamp(msg.args_f[0], 0.f, 1.f);
        out_cmd = cmd::SetMasterIntensity{v};
        return true;
    }

    // ── BAM ──────────────────────────────────────────────────────────────────
    if (p == "/idhmfis/bam/clear") {
        out_cmd = cmd::BamClear{};
        return true;
    }

    if (p == "/idhmfis/bam/enable") {
        bool en = msg.args_i.empty() ? true : (msg.args_i[0] != 0);
        out_cmd = cmd::BamSetEnabled{en};
        return true;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  target_to_engine_command — maps a MidiBinding target string + scaled value
//  to an EngineCommand.  Returns std::nullopt for unrecognised targets.
// ─────────────────────────────────────────────────────────────────────────────
static std::optional<EngineCommand>
target_to_engine_command(const std::string& target, float value) {
    if (target == "master_intensity") return cmd::SetMasterIntensity{value};
    if (target == "bpm")              return cmd::SetBPM{value};
    if (target == "param_a")          return cmd::SetGeneratorParam{0, "param_a", value};
    if (target == "param_b")          return cmd::SetGeneratorParam{0, "param_b", value};
    if (target == "param_c")          return cmd::SetGeneratorParam{0, "param_c", value};
    if (target == "speed")            return cmd::SetGeneratorParam{0, "speed", value};
    if (target == "scale")            return cmd::SetGeneratorParam{0, "scale", value};
    if (target == "pan")              return cmd::SetGeneratorParam{0, "pan", value};
    if (target == "tilt")             return cmd::SetGeneratorParam{0, "tilt", value};
    if (target == "rotation")         return cmd::SetGeneratorParam{0, "rotation", value};
    if (target == "density")          return cmd::SetGeneratorParam{0, "density", value};
    if (target == "livepro_x")        return cmd::LiveProXY{value * 2.f - 1.f, 0.f};
    if (target == "livepro_y")        return cmd::LiveProXY{0.f, value * 2.f - 1.f};
    if (target == "cue_go"   && value > 0.5f) return cmd::CueListGo{};
    if (target == "cue_back" && value > 0.5f) return cmd::CueListBack{};
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
//  MIDI → EngineCommand translation
//
//  CC mapping (channel 0):
//    CC  7 = Volume / Master Intensity
//    CC 11 = Expression (also master intensity)
//    CC 64 = Sustain pedal → Play/Stop toggle
//
//  Note mapping:
//    Note-on  → ActivateCue (note = cue slot, velocity 1-127 → fade 0-2s)
//    Note-off → DeactivateCue
// ─────────────────────────────────────────────────────────────────────────────
bool InputRouter::midi_to_command(const MidiMessage& msg,
                                   EngineCommand& out_cmd) {
    // ── Dynamic MIDI learn lookup ─────────────────────────────────────────────
    {
        MidiLearnMsg lm;

        // Derive MidiMsgType from the raw status byte
        const uint8_t t = msg.type();
        if (t == MidiMessage::kControlChange) {
            lm.type = MidiMsgType::CC;
        } else if (t == MidiMessage::kNoteOn) {
            lm.type = MidiMsgType::Note;
        } else if (t == MidiMessage::kNoteOff) {
            lm.type = MidiMsgType::Note;
        } else if (t == MidiMessage::kPitchBend) {
            lm.type = MidiMsgType::PitchBend;
        } else if (t == MidiMessage::kChanAfterTouch || t == MidiMessage::kPolyAfterTouch) {
            lm.type = MidiMsgType::Aftertouch;
        } else {
            goto hardcoded_fallback;
        }

        lm.channel = static_cast<int>(msg.channel());
        lm.number  = static_cast<int>(msg.data1);   // CC number or note number
        lm.value   = static_cast<float>(msg.data2) / 127.f;  // normalise 0..127 → 0..1

        // Pitch bend uses a 14-bit signed value; normalise -8192..+8191 → 0..1.
        // Dividing by 8191 can produce a value slightly below 0 at the -8192
        // extreme, so clamp to [0, 1] after the conversion.
        if (t == MidiMessage::kPitchBend) {
            lm.value = std::clamp(
                (static_cast<float>(msg.pitch_bend()) / 8191.f + 1.f) * 0.5f,
                0.f, 1.f);
        }

        {
            const auto bindings = engine_.midi_learn_map().resolve(lm);
            for (const auto& b : bindings) {
                float mapped = b.min_val + lm.value * (b.max_val - b.min_val);
                if (auto cmd_opt = target_to_engine_command(b.target, mapped)) {
                    out_cmd = *cmd_opt;
                    return true;
                }
            }
        }
    }
    hardcoded_fallback:

    switch (msg.type()) {
        case MidiMessage::kControlChange: {
            switch (msg.data1) {
                case 7:   // Volume
                {
                    float v = static_cast<float>(msg.data2) / 127.f;
                    out_cmd = cmd::SetMasterIntensity{v};
                    return true;
                }
                case 11:  // Expression — master intensity
                {
                    float v = static_cast<float>(msg.data2) / 127.f;
                    out_cmd = cmd::SetMasterIntensity{v};
                    return true;
                }
                case 1:   // Modwheel — generator param_a
                {
                    float v = static_cast<float>(msg.data2) / 127.f;
                    out_cmd = cmd::SetGeneratorParam{0, "param_a", v};
                    return true;
                }
                case 2:   // CC2 — generator param_b
                {
                    float v = static_cast<float>(msg.data2) / 127.f;
                    out_cmd = cmd::SetGeneratorParam{0, "param_b", v};
                    return true;
                }
                case 3:   // CC3 — generator param_c
                {
                    float v = static_cast<float>(msg.data2) / 127.f;
                    out_cmd = cmd::SetGeneratorParam{0, "param_c", v};
                    return true;
                }
                case 10:  // Pan — LivePro X (no Y; only X component updated)
                {
                    // Map 0-127 to -1..+1
                    float x = (static_cast<float>(msg.data2) / 63.5f) - 1.f;
                    x = std::clamp(x, -1.f, 1.f);
                    out_cmd = cmd::LiveProXY{x, 0.f};
                    return true;
                }
                case 64:  // Sustain pedal
                {
                    bool pressed = msg.data2 >= 64;
                    if (pressed && !sustain_held_) {
                        // Toggle play/stop on press edge
                        // The engine itself tracks whether it's playing;
                        // we can't query that here without introducing a data
                        // dependency.  We use Play as the first-press action;
                        // the engine handles stop via the same command sequence.
                        // A richer implementation would read engine_.snapshot().playing
                        // but that adds a mutex/copy overhead on every tick.
                        out_cmd = cmd::Play{};
                    }
                    sustain_held_ = pressed;
                    return pressed;  // only dispatch on press
                }
                default:
                    return false;
            }
        }

        case MidiMessage::kNoteOn: {
            if (msg.data2 == 0) break;  // velocity=0 = note-off
            float fade_s = static_cast<float>(msg.data2) / 127.f * 2.f;
            out_cmd = cmd::ActivateCue{msg.data1, fade_s};
            return true;
        }

        case MidiMessage::kNoteOff: {
            out_cmd = cmd::DeactivateCue{msg.data1, 0.5f};
            return true;
        }

        case MidiMessage::kPitchBend: {
            // Map pitch bend -8192..+8191 → intensity 0.5 ± 0.5
            float norm = (static_cast<float>(msg.pitch_bend()) / 8191.f + 1.f) * 0.5f;
            out_cmd = cmd::SetMasterIntensity{std::clamp(norm, 0.f, 1.f)};
            return true;
        }

        default:
            break;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Packets-per-second calculation for ArtNet
//  Uses exponential moving average over ~1 second windows.
// ─────────────────────────────────────────────────────────────────────────────
int InputRouter::artnet_packets_per_second() const {
    double now = HRTimer::now_s();
    double dt  = now - last_rate_check_s_;

    if (dt >= 1.0) {
        uint64_t cur_pkts = artnet_.packets_received();
        uint64_t delta    = cur_pkts - last_pkt_count_;
        cached_pps_       = static_cast<int>(static_cast<double>(delta) / dt);
        last_pkt_count_   = cur_pkts;
        last_rate_check_s_= now;
    }

    return cached_pps_;
}

} // namespace idhmfis
