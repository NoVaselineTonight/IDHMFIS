// midi_input.cpp — MIDI input via RtMidi
//
// RtMidi callback runs on a dedicated internal RtMidi thread.
// We push decoded messages into the MpscQueue which is consumed
// by InputRouter::tick() on the engine thread.
//
// Port selection strategy:
//   open(int)         — direct index, fastest path
//   open(std::string) — substring match, case-insensitive, for config files

#include "midi_input.h"

#include <RtMidi.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "../core/logger.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
MidiInput::MidiInput(MpscQueue<MidiMessage>& out)
    : out_(out) {
    try {
        midi_ = std::make_unique<RtMidiIn>();
    } catch (const RtMidiError& e) {
        log::error("MIDI: RtMidiIn construction failed: %s", e.getMessage().c_str());
    }
}

MidiInput::~MidiInput() {
    close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Port enumeration
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::string> MidiInput::list_ports() const {
    std::vector<std::string> names;
    if (!midi_) return names;

    unsigned int count = midi_->getPortCount();
    names.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        try {
            names.push_back(midi_->getPortName(i));
        } catch (const RtMidiError&) {
            names.emplace_back("<error>");
        }
    }
    return names;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Open by index
// ─────────────────────────────────────────────────────────────────────────────
bool MidiInput::open(int port_idx) {
    if (!midi_) return false;

    close();  // close any previously opened port

    unsigned int count = midi_->getPortCount();
    if (port_idx < 0 || static_cast<unsigned int>(port_idx) >= count) {
        log::error("MIDI: port index %d out of range (0..%u)", port_idx, count - 1);
        return false;
    }

    try {
        // Ignore SysEx, timing, and active sensing by default
        midi_->ignoreTypes(true, true, true);
        midi_->setCallback(&MidiInput::callback, this);
        midi_->openPort(static_cast<unsigned int>(port_idx));
        port_name_ = midi_->getPortName(static_cast<unsigned int>(port_idx));
        open_.store(true, std::memory_order_release);
        log::info("MIDI: opened port %d: %s", port_idx, port_name_.c_str());
        return true;
    } catch (const RtMidiError& e) {
        log::error("MIDI: failed to open port %d: %s",
                   port_idx, e.getMessage().c_str());
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Open by name substring (case-insensitive)
// ─────────────────────────────────────────────────────────────────────────────
bool MidiInput::open(const std::string& port_name) {
    if (!midi_) return false;

    auto to_lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return s;
    };

    std::string needle = to_lower(port_name);
    unsigned int count = midi_->getPortCount();

    for (unsigned int i = 0; i < count; ++i) {
        try {
            std::string name = midi_->getPortName(i);
            if (to_lower(name).find(needle) != std::string::npos) {
                return open(static_cast<int>(i));
            }
        } catch (const RtMidiError&) {}
    }

    log::error("MIDI: no port matching '%s' found", port_name.c_str());
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Close
// ─────────────────────────────────────────────────────────────────────────────
void MidiInput::close() {
    if (!midi_) return;
    if (!open_.load(std::memory_order_relaxed)) return;

    try {
        midi_->cancelCallback();
        midi_->closePort();
    } catch (const RtMidiError& e) {
        log::warn("MIDI: error closing port: %s", e.getMessage().c_str());
    }

    open_.store(false, std::memory_order_release);
    log::info("MIDI: port '%s' closed", port_name_.c_str());
    port_name_.clear();
}

bool MidiInput::is_open() const {
    return open_.load(std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Ignore flags
//
//  RtMidi's ignoreTypes() sets all three flags at once, so we must preserve
//  the current state of the flags we are NOT changing.  The three flags are
//  stored here so that each setter can re-apply the other two unchanged.
// ─────────────────────────────────────────────────────────────────────────────
void MidiInput::set_ignore_sysex(bool ignore) {
    ignore_sysex_ = ignore;
    if (midi_) midi_->ignoreTypes(ignore_sysex_, ignore_timing_, ignore_sensing_);
}

void MidiInput::set_ignore_timing(bool ignore) {
    ignore_timing_ = ignore;
    if (midi_) midi_->ignoreTypes(ignore_sysex_, ignore_timing_, ignore_sensing_);
}

void MidiInput::set_ignore_sensing(bool ignore) {
    ignore_sensing_ = ignore;
    if (midi_) midi_->ignoreTypes(ignore_sysex_, ignore_timing_, ignore_sensing_);
}

// ─────────────────────────────────────────────────────────────────────────────
//  RtMidi callback (runs on RtMidi's internal receive thread)
//
//  msg bytes:
//    msg[0] = status byte  (type | channel)
//    msg[1] = data1        (may be absent for 1-byte messages)
//    msg[2] = data2        (may be absent for 1-2 byte messages)
// ─────────────────────────────────────────────────────────────────────────────
void MidiInput::callback(double timestamp,
                          std::vector<uint8_t>* msg,
                          void* userdata) {
    if (!msg || msg->empty()) return;

    auto* self = static_cast<MidiInput*>(userdata);

    MidiMessage m{};
    m.timestamp = timestamp;
    m.status    = (*msg)[0];
    m.data1     = msg->size() > 1 ? (*msg)[1] : 0;
    m.data2     = msg->size() > 2 ? (*msg)[2] : 0;

    // Note-on with velocity 0 is semantically a note-off; normalise here
    // so consumers don't have to deal with this quirk
    if (m.type() == MidiMessage::kNoteOn && m.data2 == 0) {
        m.status = (m.status & 0x0F) | MidiMessage::kNoteOff;
    }

    if (!self->out_.try_push(m)) {
        // Queue full — log once per overflow sequence to avoid flooding
        log::warn("MIDI: output queue full, message dropped (status=0x%02X)", m.status);
    }
}

} // namespace idhmfis
