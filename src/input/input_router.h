#pragma once
// InputRouter — aggregates all input sources and translates them into
// ShowEngine commands.
//
// Ownership model:
//   InputRouter owns ArtNetListener, MidiInput, OscServer, SACNListener,
//   and the MpscQueues that connect them to the router's tick() method.
//   ShowEngine is held by reference; InputRouter must not outlive it.
//
// Threading:
//   start()  — starts all listener threads
//   tick()   — called from the engine thread (or any single consumer thread)
//              drains all queues and translates messages to engine commands
//   stop()   — stops all listener threads (blocks until they exit)
//
// OSC → Engine command mapping:
//   /idhmfis/play                   → cmd::Play
//   /idhmfis/stop                   → cmd::Stop
//   /idhmfis/pause                  → cmd::Pause
//   /idhmfis/intensity f            → cmd::SetMasterIntensity{f}
//   /idhmfis/master f               → cmd::SetMasterIntensity{f}
//   /idhmfis/bpm f                  → cmd::SetBPM{f}
//   /idhmfis/pointrate i            → cmd::SetPointRate{i}
//   /idhmfis/cue/activate i f       → cmd::ActivateCue{i, f}
//   /idhmfis/cue/deactivate i       → cmd::DeactivateCue{i, 0}
//   /idhmfis/param s f              → cmd::SetGeneratorParam{0, s, f}
//   /idhmfis/go, /idhmfis/cue/go    → cmd::CueListGo
//   /idhmfis/back, /cue/back        → cmd::CueListBack
//   /idhmfis/jump i i               → cmd::CueListJump{major, minor}
//   /idhmfis/cue/jump i i           → cmd::CueListJump{major, minor}
//   /idhmfis/beam/thickness f       → cmd::SetBeamThickness
//   /idhmfis/beam/bloom f           → cmd::SetBloomRadius
//   /idhmfis/fx/set i i s f         → cmd::SetFxParam
//   /idhmfis/fx/enable i i i        → cmd::SetFxEnabled
//   /idhmfis/fx/bypass i i i        → cmd::SetFxBypassed
//   /idhmfis/fx/wet i i f           → cmd::SetFxWet
//   /idhmfis/quickshow/trigger i i i → cmd::QuickShowTrigger
//   /idhmfis/quickshow/assign i i i i → cmd::QuickShowAssign
//   /idhmfis/livepro/xy f f         → cmd::LiveProXY
//   /idhmfis/livepro/scale f        → cmd::LiveProScale
//   /idhmfis/livepro/speed f        → cmd::LiveProSpeed
//   /idhmfis/livepro/beat i         → cmd::LiveProBeat
//   /idhmfis/macro i                → cmd::TriggerMacro
//   /idhmfis/output i               → cmd::SetMasterIntensity (0=blackout, 1=full)
//   /idhmfis/bam/clear              → cmd::BamClear
//   /idhmfis/bam/enable i           → cmd::BamSetEnabled
//
// MIDI → Engine command mapping:
//   CC 1  (modwheel)  → SetGeneratorParam{param_a}
//   CC 2              → SetGeneratorParam{param_b}
//   CC 3              → SetGeneratorParam{param_c}
//   CC 7              → SetMasterIntensity (volume)
//   CC 10 (pan)       → LiveProXY x component
//   CC 11 (expression)→ SetMasterIntensity
//   CC 64             → Play/Stop toggle (sustain pedal)
//   Note-on           → ActivateCue (note number → cue slot, velocity → fade time 0-2s)
//   Note-off          → DeactivateCue
//   Pitch bend        → SetMasterIntensity (0.5 ± 0.5)

#include <atomic>
#include <cstdint>
#include <string>

#include "../core/show_engine.h"
#include "../core/spsc_queue.h"
#include "../core/types.h"
#include "../core/timer.h"

#include "artnet.h"
#include "midi_input.h"
#include "osc_server.h"

namespace idhmfis {

class InputRouter {
public:
    explicit InputRouter(ShowEngine& engine);
    ~InputRouter();

    // Non-copyable, non-movable
    InputRouter(const InputRouter&)            = delete;
    InputRouter& operator=(const InputRouter&) = delete;

    // Start all listeners.  Individual listener configs can be set via
    // the accessor methods before calling start().
    void start(const ArtNetConfig& artnet_cfg = {},
               int osc_port = 8000);
    void stop();

    // Drain all pending input queues and dispatch engine commands.
    // Call this once per engine tick (1000 Hz).
    void tick();

    // ── Accessor helpers ──────────────────────────────────────────────────
    ArtNetListener& artnet()     { return artnet_; }
    MidiInput&      midi()       { return midi_input_; }
    OscServer&      osc()        { return osc_server_; }
    SACNListener&   sacn()       { return sacn_; }

    // ── Latency stats ─────────────────────────────────────────────────────
    double artnet_p99_latency_ms()   const { return artnet_.latency_p99_ms(); }
    int    artnet_packets_per_second() const;

private:
    void process_dmx();
    void process_midi();
    void process_osc();

    // Map OSC path + args to an EngineCommand; returns false if unrecognized
    bool osc_to_command(const OscServer::Message& msg, EngineCommand& out_cmd);

    // Map MIDI message to an EngineCommand; returns false if unrecognized
    bool midi_to_command(const MidiMessage& msg, EngineCommand& out_cmd);

    ShowEngine& engine_;

    // Queues
    MpscQueue<std::pair<int, DmxUniverse>> dmx_queue_;
    MpscQueue<MidiMessage>                 midi_queue_;
    MpscQueue<OscServer::Message>          osc_queue_;

    // Listeners (in dependency order)
    ArtNetListener artnet_;
    SACNListener   sacn_;
    MidiInput      midi_input_;
    OscServer      osc_server_;

    // Packet-rate tracking for artnet_packets_per_second()
    mutable uint64_t last_pkt_count_{0};
    mutable double   last_rate_check_s_{0.0};
    mutable int      cached_pps_{0};

    // Sustain pedal state for MIDI play/stop toggle
    bool sustain_held_{false};

    // Self-tick background thread — drains all input queues at ~1 kHz.
    // Calls tick() so DMX/MIDI/OSC reach the engine without requiring the
    // caller to poll.  Stopped before the listeners in stop().
    std::atomic<bool> tick_running_{false};
    std::thread       tick_thread_;
};

} // namespace idhmfis
