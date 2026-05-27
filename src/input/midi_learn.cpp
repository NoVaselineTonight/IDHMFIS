// midi_learn.cpp — MIDI learn map + session state machine.

#include "midi_learn.h"
#include <sstream>
#include <algorithm>
#include <cstring>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  MidiLearnMap
// ─────────────────────────────────────────────────────────────────────────────

void MidiLearnMap::add_binding(MidiBinding b)
{
    std::lock_guard<std::mutex> lk(mtx_);
    // Replace existing binding for same target, if any
    for (auto& existing : bindings_) {
        if (existing.target == b.target) {
            existing = std::move(b);
            return;
        }
    }
    bindings_.push_back(std::move(b));
}

void MidiLearnMap::remove_binding(const std::string& target)
{
    std::lock_guard<std::mutex> lk(mtx_);
    bindings_.erase(
        std::remove_if(bindings_.begin(), bindings_.end(),
                       [&](const MidiBinding& b){ return b.target == target; }),
        bindings_.end());
}

void MidiLearnMap::clear()
{
    std::lock_guard<std::mutex> lk(mtx_);
    bindings_.clear();
}

std::vector<MidiBinding> MidiLearnMap::resolve(const MidiLearnMsg& msg) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<MidiBinding> results;
    for (const auto& b : bindings_) {
        if (b.type != msg.type) continue;
        // channel 0 = "any channel"
        if (b.channel != 0 && b.channel != msg.channel) continue;
        // PitchBend and Aftertouch don't use a number field — skip match
        if (b.type != MidiMsgType::PitchBend && b.type != MidiMsgType::Aftertouch) {
            if (b.number != msg.number) continue;
        }
        results.push_back(b);
    }
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Simple hand-written JSON serialization (no external deps)
// ─────────────────────────────────────────────────────────────────────────────

static std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

static const char* type_to_str(MidiMsgType t)
{
    switch (t) {
    case MidiMsgType::CC:          return "cc";
    case MidiMsgType::Note:        return "note";
    case MidiMsgType::PitchBend:   return "pitchbend";
    case MidiMsgType::Aftertouch:  return "aftertouch";
    }
    return "cc";
}

static MidiMsgType str_to_type(const std::string& s)
{
    if (s == "note")        return MidiMsgType::Note;
    if (s == "pitchbend")   return MidiMsgType::PitchBend;
    if (s == "aftertouch")  return MidiMsgType::Aftertouch;
    return MidiMsgType::CC;
}

std::string MidiLearnMap::to_json() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::ostringstream os;
    os << "{\"bindings\":[";
    for (size_t i = 0; i < bindings_.size(); ++i) {
        const auto& b = bindings_[i];
        if (i > 0) os << ',';
        // BUG #50: use std::string / ostringstream directly so that long
        // target names (control_name, channel_name) are never silently
        // truncated by a fixed-size snprintf buffer.
        char fmin[32], fmax[32];
        std::snprintf(fmin, sizeof(fmin), "%.6f", b.min_val);
        std::snprintf(fmax, sizeof(fmax), "%.6f", b.max_val);
        os << "{\"type\":\"" << type_to_str(b.type) << "\""
           << ",\"channel\":"   << b.channel
           << ",\"number\":"    << b.number
           << ",\"target\":\""  << json_escape(b.target) << "\""
           << ",\"min_val\":"   << fmin
           << ",\"max_val\":"   << fmax
           << "}";
    }
    os << "]}";
    return os.str();
}

// Minimal JSON parser for the format we write above.
// Parses: {"bindings":[{...},...]}
static std::string extract_str_field(const std::string& obj, const char* key)
{
    // Looks for "key":"value" in obj
    std::string needle = std::string("\"") + key + "\":\"";
    auto pos = obj.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = obj.find('"', pos);
    if (end == std::string::npos) return {};
    return obj.substr(pos, end - pos);
}

static int extract_int_field(const std::string& obj, const char* key, int def = 0)
{
    std::string needle = std::string("\"") + key + "\":";
    auto pos = obj.find(needle);
    if (pos == std::string::npos) return def;
    pos += needle.size();
    // skip quote if present (for string-typed ints — shouldn't happen but be safe)
    if (pos < obj.size() && obj[pos] == '"') ++pos;
    try { return std::stoi(obj.substr(pos)); }
    catch (...) { return def; }
}

static float extract_float_field(const std::string& obj, const char* key, float def = 0.f)
{
    std::string needle = std::string("\"") + key + "\":";
    auto pos = obj.find(needle);
    if (pos == std::string::npos) return def;
    pos += needle.size();
    try { return std::stof(obj.substr(pos)); }
    catch (...) { return def; }
}

bool MidiLearnMap::from_json(const std::string& json)
{
    // Find "bindings":[...]
    auto arr_start = json.find("[");
    auto arr_end   = json.rfind("]");
    if (arr_start == std::string::npos || arr_end == std::string::npos) return false;

    std::string arr = json.substr(arr_start + 1, arr_end - arr_start - 1);

    std::vector<MidiBinding> new_bindings;

    // Split on top-level objects {...}
    size_t pos = 0;
    while (pos < arr.size()) {
        auto obj_start = arr.find('{', pos);
        if (obj_start == std::string::npos) break;
        // Find matching closing brace
        int depth = 0;
        size_t obj_end = obj_start;
        for (size_t k = obj_start; k < arr.size(); ++k) {
            if (arr[k] == '{') ++depth;
            else if (arr[k] == '}') { --depth; if (depth == 0) { obj_end = k; break; } }
        }
        std::string obj = arr.substr(obj_start, obj_end - obj_start + 1);

        MidiBinding b;
        b.type    = str_to_type(extract_str_field(obj, "type"));
        b.channel = extract_int_field(obj, "channel", 0);
        b.number  = extract_int_field(obj, "number", 0);
        b.target  = extract_str_field(obj, "target");
        b.min_val = extract_float_field(obj, "min_val", 0.f);
        b.max_val = extract_float_field(obj, "max_val", 1.f);

        if (!b.target.empty())
            new_bindings.push_back(std::move(b));

        pos = obj_end + 1;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    bindings_ = std::move(new_bindings);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  MidiLearnSession
// ─────────────────────────────────────────────────────────────────────────────

void MidiLearnSession::arm(const std::string& target, float min_val, float max_val)
{
    armed_  = true;
    target_ = target;
    min_    = min_val;
    max_    = max_val;
}

void MidiLearnSession::cancel()
{
    armed_  = false;
    target_.clear();
}

bool MidiLearnSession::on_message(const MidiLearnMsg& msg, MidiLearnMap& map_out)
{
    if (!armed_) return false;

    MidiBinding b;
    b.type    = msg.type;
    b.channel = msg.channel;
    b.number  = msg.number;
    b.target  = target_;
    b.min_val = min_;
    b.max_val = max_;

    map_out.add_binding(std::move(b));

    armed_  = false;
    target_.clear();
    return true;
}

} // namespace idhmfis
