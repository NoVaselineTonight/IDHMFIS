#pragma once
// MIDI input subsystem via RtMidi.
// Receives note, CC, pitch-bend, aftertouch, and program-change messages.
// Pushes decoded Message structs into an MpscQueue for consumption
// by InputRouter on the engine thread.
//
// RtMidi is fetched via CMake FetchContent and provides cross-platform
// MIDI I/O on Windows (WinMM/DirectSound), macOS (CoreMIDI), and Linux (ALSA/JACK).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <functional>

#include <RtMidi.h>
#include "../core/spsc_queue.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Decoded MIDI message
// ─────────────────────────────────────────────────────────────────────────────
struct MidiMessage {
    uint8_t  status;      // Status byte (includes channel in low nibble)
    uint8_t  data1;       // First data byte  (note / CC number / etc.)
    uint8_t  data2;       // Second data byte (velocity / CC value / etc.)
    double   timestamp;   // Delta time in seconds from previous message

    // Convenience accessors
    uint8_t type()    const { return status & 0xF0; }  // e.g. 0x90 = NoteOn
    uint8_t channel() const { return status & 0x0F; }  // 0-15

    // Message type constants
    static constexpr uint8_t kNoteOff       = 0x80;
    static constexpr uint8_t kNoteOn        = 0x90;
    static constexpr uint8_t kPolyAfterTouch= 0xA0;
    static constexpr uint8_t kControlChange = 0xB0;
    static constexpr uint8_t kProgramChange = 0xC0;
    static constexpr uint8_t kChanAfterTouch= 0xD0;
    static constexpr uint8_t kPitchBend     = 0xE0;
    static constexpr uint8_t kSysEx         = 0xF0;
    static constexpr uint8_t kTimingClock   = 0xF8;
    static constexpr uint8_t kStart         = 0xFA;
    static constexpr uint8_t kContinue      = 0xFB;
    static constexpr uint8_t kStop          = 0xFC;
    static constexpr uint8_t kActiveSensing = 0xFE;
    static constexpr uint8_t kReset         = 0xFF;

    // Pitch-bend value as signed -8192..+8191
    int16_t pitch_bend() const {
        return static_cast<int16_t>(((static_cast<uint16_t>(data2) << 7) | data1) - 8192);
    }

    // CC value normalised to 0..1
    float cc_norm() const { return data2 / 127.f; }

    bool is_note_on()  const { return type() == kNoteOn  && data2 > 0; }
    bool is_note_off() const { return type() == kNoteOff || (type() == kNoteOn && data2 == 0); }
};

// ─────────────────────────────────────────────────────────────────────────────
//  MidiInput
// ─────────────────────────────────────────────────────────────────────────────
class MidiInput {
public:
    // Legacy alias so existing code using MidiInput::Message still works
    using Message = MidiMessage;

    explicit MidiInput(MpscQueue<MidiMessage>& out);
    ~MidiInput();

    // Non-copyable
    MidiInput(const MidiInput&)            = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    // List available MIDI input ports (index 0 = first physical device)
    std::vector<std::string> list_ports() const;

    // Open by numeric index (from list_ports())
    bool open(int port_idx);

    // Open by port name substring match (case-insensitive)
    bool open(const std::string& port_name);

    // Close the currently open port
    void close();

    bool is_open() const;
    std::string current_port_name() const { return port_name_; }

    // Optional: ignore categories of messages (defaults: ignore sysex+timing+sensing)
    void set_ignore_sysex   (bool ignore);
    void set_ignore_timing  (bool ignore);
    void set_ignore_sensing (bool ignore);

private:
    static void callback(double timestamp,
                         std::vector<uint8_t>* msg,
                         void* userdata);

    MpscQueue<MidiMessage>&     out_;
    std::unique_ptr<RtMidiIn>   midi_;
    std::string                 port_name_;
    std::atomic<bool>           open_{false};

    // Tracked ignore-flag state so each setter can preserve the other two.
    // Defaults match the ignoreTypes call in open(): all three ignored.
    bool ignore_sysex_   = true;
    bool ignore_timing_  = true;
    bool ignore_sensing_ = true;
};

} // namespace idhmfis
