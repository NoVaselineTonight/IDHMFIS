#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace idhmfis {

enum class MidiMsgType { CC, Note, PitchBend, Aftertouch };

struct MidiBinding {
    MidiMsgType type     = MidiMsgType::CC;
    int         channel  = 0;   // 0 = any
    int         number   = 0;   // CC number or note
    std::string target;         // "master_intensity", "bpm", "cue_N_fade", etc.
    float       min_val  = 0.f;
    float       max_val  = 1.f;
};

struct MidiLearnMsg {
    MidiMsgType type;
    int channel;
    int number;
    float value; // 0..1 normalized
};

class MidiLearnMap {
public:
    void add_binding(MidiBinding b);
    void remove_binding(const std::string& target);
    void clear();

    // Returns all targets matched by this message
    std::vector<const MidiBinding*> resolve(const MidiLearnMsg& msg) const;

    // Serialization
    std::string to_json() const;
    bool from_json(const std::string& json);

    const std::vector<MidiBinding>& bindings() const { return bindings_; }

private:
    std::vector<MidiBinding> bindings_;
    mutable std::mutex mtx_;
};

// MIDI learn state machine: arm a target, receive one MIDI message, bind it
class MidiLearnSession {
public:
    void arm(const std::string& target, float min_val, float max_val);
    void cancel();
    bool is_armed() const { return armed_; }
    const std::string& armed_target() const { return target_; }

    // Call on every incoming MIDI message. Returns true if a binding was completed.
    bool on_message(const MidiLearnMsg& msg, MidiLearnMap& map_out);

private:
    bool        armed_  = false;
    std::string target_;
    float       min_    = 0.f;
    float       max_    = 1.f;
};

} // namespace idhmfis
