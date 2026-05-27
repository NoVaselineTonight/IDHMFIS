// serialization.cpp — Full nlohmann/json ADL serialisation for all project types.
//
// Design decisions:
//   - Every field is explicitly named; no implicit struct-to-array tricks.
//   - from_json uses .value(key, default) so older files with missing optional
//     fields load cleanly (forward-compatible reading).
//   - Color4 is stored as {"r":...,"g":...,"b":...,"a":...} floats (0-1 range).
//   - UUIDs, paths, enum strings are stored as JSON strings.
//   - Doubles are stored as JSON numbers (full precision).
//   - The schema_version field drives migration in Project::from_json.

#include "serialization.h"
#include <algorithm>
#include <climits>
#include <stdexcept>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Color4
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const Color4& v) {
    j = nlohmann::json{
        {"r", v.r},
        {"g", v.g},
        {"b", v.b},
        {"a", v.a}
    };
}

void from_json(const nlohmann::json& j, Color4& v) {
    v.r = j.value("r", 0.f);
    v.g = j.value("g", 0.f);
    v.b = j.value("b", 0.f);
    v.a = j.value("a", 1.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  GeneratorParams
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const GeneratorParams& v) {
    j = nlohmann::json{
        {"speed",       v.speed},
        {"scale",       v.scale},
        {"density",     v.density},
        {"param_a",     v.param_a},
        {"param_b",     v.param_b},
        {"param_c",     v.param_c},
        {"color_a",     v.color_a},
        {"color_b",     v.color_b},
        {"rotation",    v.rotation},
        {"pan",         v.pan},
        {"tilt",        v.tilt},
        {"zoom",        v.zoom},
        {"intensity",   v.intensity},
        {"blanked",     v.blanked},
        {"point_count", v.point_count}
    };
    if (!v.extra_params.empty()) {
        auto& ep = j["extra_params"];
        for (const auto& [k, val] : v.extra_params)
            ep[k] = val;
    }
}

void from_json(const nlohmann::json& j, GeneratorParams& v) {
    v.speed       = j.value("speed",       1.f);
    v.scale       = j.value("scale",       1.f);
    v.density     = j.value("density",     0.5f);
    v.param_a     = j.value("param_a",     0.5f);
    v.param_b     = j.value("param_b",     0.5f);
    v.param_c     = j.value("param_c",     0.5f);
    v.rotation    = j.value("rotation",    0.f);
    v.pan         = j.value("pan",         0.f);
    v.tilt        = j.value("tilt",        0.f);
    v.zoom        = j.value("zoom",        1.f);
    v.intensity   = j.value("intensity",   1.f);
    v.blanked     = j.value("blanked",     false);
    v.point_count = j.value("point_count", 512);

    if (j.contains("color_a")) j.at("color_a").get_to(v.color_a);
    if (j.contains("color_b")) j.at("color_b").get_to(v.color_b);
    if (j.contains("extra_params") && j.at("extra_params").is_object()) {
        for (auto& [k, val] : j.at("extra_params").items())
            if (val.is_number()) v.extra_params[k] = val.get<float>();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  KeyframePoint
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const KeyframePoint& v) {
    j = nlohmann::json{
        {"time",   v.time},
        {"value",  v.value},
        {"interp", v.interp}
    };
    // Only emit bezier handles when they are needed to avoid file bloat.
    if (v.interp == KeyframePoint::Interp::CubicBezier) {
        j["cp1"] = v.cp1;
        j["cp2"] = v.cp2;
    }
}

void from_json(const nlohmann::json& j, KeyframePoint& v) {
    v.time   = j.value("time",  0.0);
    v.value  = j.value("value", 0.f);
    v.interp = j.value("interp", KeyframePoint::Interp::Linear);
    v.cp1    = j.value("cp1",   0.f);
    v.cp2    = j.value("cp2",   0.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ParamTrack
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const ParamTrack& v) {
    j = nlohmann::json{
        {"param_name", v.param_name},
        {"keyframes",  v.keyframes}
    };

    // Only emit mapping sections when they are active.
    if (v.dmx_mapped) {
        j["dmx"] = {
            {"mapped",    true},
            {"channel",   v.dmx_channel},
            {"universe",  v.dmx_universe}
        };
    }

    if (v.midi_mapped) {
        j["midi"] = {
            {"mapped", true},
            {"cc",     v.midi_cc}
        };
    }

    if (v.osc_mapped) {
        j["osc"] = {
            {"mapped", true},
            {"path",   v.osc_path}
        };
    }

    if (v.audio_mapped) {
        j["audio"] = {
            {"mapped", true},
            {"source", v.audio_source}
        };
    }
}

void from_json(const nlohmann::json& j, ParamTrack& v) {
    v.param_name = j.value("param_name", std::string{});
    if (j.contains("keyframes")) j.at("keyframes").get_to(v.keyframes);

    if (j.contains("dmx")) {
        const auto& d = j.at("dmx");
        v.dmx_mapped   = d.value("mapped",   false);
        v.dmx_channel  = d.value("channel",  0);
        v.dmx_universe = d.value("universe", 0);
    }

    if (j.contains("midi")) {
        const auto& m = j.at("midi");
        v.midi_mapped = m.value("mapped", false);
        v.midi_cc     = m.value("cc",    -1);
    }

    if (j.contains("osc")) {
        const auto& o = j.at("osc");
        v.osc_mapped = o.value("mapped", false);
        v.osc_path   = o.value("path",   std::string{});
    }

    if (j.contains("audio")) {
        const auto& a = j.at("audio");
        v.audio_mapped  = a.value("mapped", false);
        v.audio_source  = a.value("source", std::string{});
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Cue
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const Cue& v) {
    j = nlohmann::json{
        {"id",         v.id},
        {"name",       v.name},
        {"generator",  v.generator},
        {"params",     v.params},
        {"duration",   v.duration},
        {"loop",       v.loop},
        {"color_tag",  v.color_tag},
        {"automation", v.automation}
    };
    if (!v.ilda_path.empty())
        j["ilda_path"] = v.ilda_path;
}

void from_json(const nlohmann::json& j, Cue& v) {
    v.id        = j.value("id",        std::string{});
    v.name      = j.value("name",      std::string{});
    v.generator = j.value("generator", GeneratorType::Beams);
    v.ilda_path = j.value("ilda_path", std::string{});
    v.duration  = j.value("duration",  8.0);
    v.loop      = j.value("loop",      true);
    v.color_tag = j.value("color_tag", static_cast<uint32_t>(0xFF4488FFu));

    if (j.contains("params"))     j.at("params").get_to(v.params);
    if (j.contains("automation")) j.at("automation").get_to(v.automation);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DmxFixtureProfile::ChannelDef
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const DmxFixtureProfile::ChannelDef& v) {
    j = nlohmann::json{
        {"offset",     v.offset},
        {"param_name", v.param_name},
        {"scale_min",  v.scale_min},
        {"scale_max",  v.scale_max}
    };
}

void from_json(const nlohmann::json& j, DmxFixtureProfile::ChannelDef& v) {
    v.offset     = j.value("offset",     1);
    v.param_name = j.value("param_name", std::string{});
    v.scale_min  = j.value("scale_min",  0.f);
    v.scale_max  = j.value("scale_max",  1.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DmxFixtureProfile
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const DmxFixtureProfile& v) {
    j = nlohmann::json{
        {"name",     v.name},
        {"channels", v.channels}
    };
}

void from_json(const nlohmann::json& j, DmxFixtureProfile& v) {
    v.name = j.value("name", std::string{});
    if (j.contains("channels")) j.at("channels").get_to(v.channels);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DmxPatch
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const DmxPatch& v) {
    j = nlohmann::json{
        {"universe",   v.universe},
        {"start_addr", v.start_addr},
        {"profile",    v.profile}
    };
}

void from_json(const nlohmann::json& j, DmxPatch& v) {
    v.universe   = j.value("universe",   0);
    v.start_addr = j.value("start_addr", 1);
    if (j.contains("profile")) j.at("profile").get_to(v.profile);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CueListEntry
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const CueListEntry& v) {
    j = nlohmann::json{
        {"cue_id",          v.cue_id},
        {"in_time",         v.in_time},
        {"out_time",        v.out_time},
        {"fade_in",         v.fade_in},
        {"fade_out",        v.fade_out},
        {"auto_next",       v.auto_next},
        {"auto_next_delay", v.auto_next_delay}
    };
}

void from_json(const nlohmann::json& j, CueListEntry& v) {
    v.cue_id          = j.value("cue_id",          std::string{});
    v.in_time         = j.value("in_time",          0.0);
    v.out_time        = j.value("out_time",         0.0);
    v.fade_in         = j.value("fade_in",          0.5f);
    v.fade_out        = j.value("fade_out",         0.5f);
    v.auto_next       = j.value("auto_next",        false);
    v.auto_next_delay = j.value("auto_next_delay",  0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CueNumber
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const CueNumber& v) {
    j = nlohmann::json{ {"major", v.major}, {"minor", v.minor} };
}

void from_json(const nlohmann::json& j, CueNumber& v) {
    v.major = j.value("major", 1);
    v.minor = j.value("minor", 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  SplitTimes
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const SplitTimes& v) {
    j = nlohmann::json{
        {"intensity", v.intensity},
        {"color",     v.color},
        {"position",  v.position},
        {"fx",        v.fx},
        {"beam",      v.beam}
    };
}

void from_json(const nlohmann::json& j, SplitTimes& v) {
    v.intensity = j.value("intensity", -1.f);
    v.color     = j.value("color",     -1.f);
    v.position  = j.value("position",  -1.f);
    v.fx        = j.value("fx",        -1.f);
    v.beam      = j.value("beam",      -1.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CueTimingBlock
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const CueTimingBlock& v) {
    j = nlohmann::json{
        {"fade_in",   v.fade_in},
        {"fade_out",  v.fade_out},
        {"delay_in",  v.delay_in},
        {"delay_out", v.delay_out},
        {"hold",      v.hold},
        {"wait",      v.wait},
        {"path",      v.path},
        {"fan",       v.fan},
        {"fan_seed",  v.fan_seed},
        {"split",     v.split}
    };
    if (!v.path_expr.empty())
        j["path_expr"] = v.path_expr;
}

void from_json(const nlohmann::json& j, CueTimingBlock& v) {
    // L-8: clamp all timing floats to sane ranges on load so a corrupt or
    // hand-edited project file can't push negative/extreme values into the
    // playback state machine (negative fade_in causes the engine to never
    // leave delay phase; extreme hold causes a multi-hour freeze).
    static constexpr float kMaxTimeSec = 3600.f; // 1 hour ceiling per phase
    v.fade_in   = std::max(0.f, std::min(j.value("fade_in",   0.f), kMaxTimeSec));
    v.fade_out  = std::max(0.f, std::min(j.value("fade_out",  0.f), kMaxTimeSec));
    v.delay_in  = std::max(0.f, std::min(j.value("delay_in",  0.f), kMaxTimeSec));
    v.delay_out = std::max(0.f, std::min(j.value("delay_out", 0.f), kMaxTimeSec));
    v.hold      = std::max(0.f, std::min(j.value("hold",      0.f), kMaxTimeSec));
    v.wait      = std::max(0.f, std::min(j.value("wait",      0.f), kMaxTimeSec));
    v.path      = j.value("path",      PathInterp::Linear);
    v.fan       = j.value("fan",       FanMode::None);
    v.fan_seed  = j.value("fan_seed",  0.f);
    v.path_expr = j.value("path_expr", std::string{});
    if (j.contains("split")) j.at("split").get_to(v.split);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CueTrigger
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const CueTrigger& v) {
    j = nlohmann::json{
        {"type",            v.type},
        {"time_s",          v.time_s},
        {"tc_frame",        v.tc_frame},
        {"midi_ch",         v.midi_ch},
        {"midi_note",       v.midi_note},
        {"midi_cc",         v.midi_cc},
        {"midi_pc",         v.midi_pc},
        {"osc_address",     v.osc_address},
        {"dmx_universe",    v.dmx_universe},
        {"dmx_channel",     v.dmx_channel},
        {"dmx_threshold",   v.dmx_threshold},
        {"audio_trigger",   v.audio_trigger},
        {"audio_threshold", v.audio_threshold}
    };
}

void from_json(const nlohmann::json& j, CueTrigger& v) {
    v.type            = j.value("type",            TriggerType::Halt);
    v.time_s          = j.value("time_s",          0.f);
    v.tc_frame        = j.value("tc_frame",        0);
    v.midi_ch         = j.value("midi_ch",         1);
    v.midi_note       = j.value("midi_note",       -1);
    v.midi_cc         = j.value("midi_cc",         -1);
    v.midi_pc         = j.value("midi_pc",         -1);
    v.osc_address     = j.value("osc_address",     std::string{});
    v.dmx_universe    = j.value("dmx_universe",    0);
    v.dmx_channel     = j.value("dmx_channel",     1);
    v.dmx_threshold   = j.value("dmx_threshold",   static_cast<uint8_t>(128));
    v.audio_trigger   = j.value("audio_trigger",   std::string{});
    v.audio_threshold = j.value("audio_threshold", 0.5f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  FxStackDiff (legacy — kept for backward compat reading old project files)
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const FxStackDiff& v) {
    j = nlohmann::json{ {"json_blob", v.json_blob} };
}

void from_json(const nlohmann::json& j, FxStackDiff& v) {
    v.json_blob = j.value("json_blob", std::string{});
}

// ─────────────────────────────────────────────────────────────────────────────
//  GlobalFxEntry
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const GlobalFxEntry& v) {
    j = nlohmann::json{
        {"type",      v.type},
        {"blend",     v.blend},
        {"rate",      v.rate},
        {"depth",     v.depth},
        {"duty",      v.duty},
        {"offset",    v.offset},
        {"direction", v.direction},
        {"dir_width", v.dir_width},
        {"parts",     v.parts},
        {"segs",      v.segs},
        {"width",     v.width},
        {"enabled",   v.enabled},
        {"crossfade", v.crossfade}
    };
}

void from_json(const nlohmann::json& j, GlobalFxEntry& v) {
    v.type      = j.value("type",      GlobalFxType::Sine);
    v.blend     = j.value("blend",     FxBlendMode::Absolute);
    v.rate      = j.value("rate",      1.f);
    v.depth     = j.value("depth",     1.f);
    v.duty      = j.value("duty",      0.5f);
    v.offset    = j.value("offset",    0.f);
    v.direction = j.value("direction", FxDirection::Sync);
    v.dir_width = j.value("dir_width", 1.f);
    v.parts     = j.value("parts",     1);
    v.segs      = j.value("segs",      1);
    v.width     = j.value("width",     1.f);
    v.enabled   = j.value("enabled",   true);
    v.crossfade = j.value("crossfade", 0.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  FrameFxEntry
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const FrameFxEntry& v) {
    j = nlohmann::json{
        {"type",              v.type},
        {"rate",              v.rate},
        {"depth",             v.depth},
        {"offset",            v.offset},
        {"size",              v.size},
        {"spread",            v.spread},
        {"direction",         v.direction},
        {"dir_width",         v.dir_width},
        {"parts",             v.parts},
        {"segs",              v.segs},
        {"width",             v.width},
        {"enabled",           v.enabled},
        {"crossfade",         v.crossfade},
        {"col_a_r",           v.col_a_r},
        {"col_a_g",           v.col_a_g},
        {"col_a_b",           v.col_a_b},
        {"col_b_r",           v.col_b_r},
        {"col_b_g",           v.col_b_g},
        {"col_b_b",           v.col_b_b},
        {"col_c_r",           v.col_c_r},
        {"col_c_g",           v.col_c_g},
        {"col_c_b",           v.col_c_b},
        {"col_d_r",           v.col_d_r},
        {"col_d_g",           v.col_d_g},
        {"col_d_b",           v.col_d_b},
        {"col_e_r",           v.col_e_r},
        {"col_e_g",           v.col_e_g},
        {"col_e_b",           v.col_e_b},
        {"use_custom_colors", v.use_custom_colors}
    };
}

void from_json(const nlohmann::json& j, FrameFxEntry& v) {
    v.type              = j.value("type",              FrameFxType::PanX);
    v.rate              = j.value("rate",              1.f);
    v.depth             = j.value("depth",             0.1f);
    v.offset            = j.value("offset",            0.f);
    v.size              = j.value("size",              1.f);
    v.spread            = j.value("spread",            1.f);
    v.direction         = j.value("direction",         FxDirection::Sync);
    v.dir_width         = j.value("dir_width",         1.f);
    v.parts             = j.value("parts",             1);
    v.segs              = j.value("segs",              1);
    v.width             = j.value("width",             1.f);
    v.enabled           = j.value("enabled",           true);
    v.crossfade         = j.value("crossfade",         0.f);
    v.col_a_r           = j.value("col_a_r",           1.f);
    v.col_a_g           = j.value("col_a_g",           0.f);
    v.col_a_b           = j.value("col_a_b",           0.f);
    v.col_b_r           = j.value("col_b_r",           0.f);
    v.col_b_g           = j.value("col_b_g",           0.f);
    v.col_b_b           = j.value("col_b_b",           1.f);
    v.col_c_r           = j.value("col_c_r",           0.f);
    v.col_c_g           = j.value("col_c_g",           1.f);
    v.col_c_b           = j.value("col_c_b",           0.f);
    v.col_d_r           = j.value("col_d_r",           1.f);
    v.col_d_g           = j.value("col_d_g",           1.f);
    v.col_d_b           = j.value("col_d_b",           0.f);
    v.col_e_r           = j.value("col_e_r",           1.f);
    v.col_e_g           = j.value("col_e_g",           0.f);
    v.col_e_b           = j.value("col_e_b",           1.f);
    v.use_custom_colors = j.value("use_custom_colors", false);
}

// ─────────────────────────────────────────────────────────────────────────────
//  FxLayer
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const FxLayer& v) {
    j = nlohmann::json{ {"fx", v.fx} };
}

void from_json(const nlohmann::json& j, FxLayer& v) {
    v.fx.clear();
    if (j.contains("fx")) j.at("fx").get_to(v.fx);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ChaserStep
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const ChaserStep& v) {
    j = nlohmann::json{
        {"cue_ref_idx", v.cue_ref_idx},
        {"hold_s",      v.hold_s},
        {"xfade_s",     v.xfade_s},
        {"beat_sync",   v.beat_sync},
        {"beat_div",    v.beat_div}
    };
}
void from_json(const nlohmann::json& j, ChaserStep& v) {
    v.cue_ref_idx = j.value("cue_ref_idx", 0);
    if (j.contains("hold_s"))    j.at("hold_s").get_to(v.hold_s);
    if (j.contains("xfade_s"))   j.at("xfade_s").get_to(v.xfade_s);
    if (j.contains("beat_sync")) j.at("beat_sync").get_to(v.beat_sync);
    if (j.contains("beat_div"))  j.at("beat_div").get_to(v.beat_div);
}

// ─────────────────────────────────────────────────────────────────────────────
//  FullCueEntry
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const FullCueEntry& v) {
    j = nlohmann::json{
        {"number",          v.number},
        {"cue_id",          v.cue_id},
        {"name",            v.name},
        {"comment",         v.comment},
        {"color_tag",       v.color_tag},
        {"timing",          v.timing},
        {"trigger",         v.trigger},
        {"tracking",        v.tracking},
        {"is_block",        v.is_block},
        {"assert_flag",     v.assert_flag},
        {"fx_layer",        v.fx_layer},
        {"link",            v.link},
        {"jump_major",      v.jump_major},
        {"jump_minor",      v.jump_minor}
    };

    // Sparse param_overrides — only write when non-empty.
    if (!v.param_overrides.empty()) {
        nlohmann::json overrides = nlohmann::json::array();
        for (const auto& kv : v.param_overrides) {
            overrides.push_back({ {"k", kv.first}, {"v", kv.second} });
        }
        j["param_overrides"] = std::move(overrides);
    }

    j["is_chaser"]           = v.is_chaser;
    j["chaser_global_hold"]  = v.chaser_global_hold;
    j["chaser_global_xfade"] = v.chaser_global_xfade;
    j["chaser_beat_sync"]    = v.chaser_beat_sync;
    j["chaser_beat_div"]     = v.chaser_beat_div;
    j["chaser_steps"]        = v.chaser_steps;

    // 3-layer system
    j["global_layer"]    = v.global_layer;
    j["keyframe_layer"]  = v.keyframe_layer;

    // Per-stream FX overrides — only write when non-empty (JSON object, string keys)
    if (!v.per_stream_fx.empty()) {
        nlohmann::json psf = nlohmann::json::object();
        for (const auto& [k, fxl] : v.per_stream_fx)
            psf[std::to_string(k)] = fxl;
        j["per_stream_fx"] = std::move(psf);
    }

    // Per-stream keyframe layers — only write when non-empty (JSON object, string keys)
    j["per_stream_kf"] = nlohmann::json::object();
    for (const auto& [sid, kf] : v.per_stream_kf)
        j["per_stream_kf"][std::to_string(sid)] = kf;

    // Mirrored stream IDs — streams that receive X-flipped output during playback
    if (!v.mirrored_ids.empty())
        j["mirrored_ids"] = v.mirrored_ids;
}

void from_json(const nlohmann::json& j, FullCueEntry& v) {
    if (j.contains("number"))   j.at("number").get_to(v.number);
    v.cue_id      = j.value("cue_id",      std::string{});
    v.name        = j.value("name",        std::string{});
    v.comment     = j.value("comment",     std::string{});
    v.color_tag   = j.value("color_tag",   static_cast<uint32_t>(0xFF4488FFu));
    if (j.contains("timing"))   j.at("timing").get_to(v.timing);
    if (j.contains("trigger"))  j.at("trigger").get_to(v.trigger);
    v.tracking    = j.value("tracking",    TrackingMode::Tracking);
    v.is_block    = j.value("is_block",    false);
    v.assert_flag = j.value("assert_flag", false);
    // Prefer new fx_layer; fall back to legacy fx_diff (ignored, not converted)
    if (j.contains("fx_layer")) j.at("fx_layer").get_to(v.fx_layer);
    v.link        = j.value("link",        LinkMode::Next);
    v.jump_major  = j.value("jump_major",  0);
    v.jump_minor  = j.value("jump_minor",  0);

    if (j.contains("param_overrides")) {
        v.param_overrides.clear();
        for (const auto& item : j.at("param_overrides")) {
            std::string k = item.value("k", std::string{});
            float       val = item.value("v", 0.f);
            v.param_overrides.emplace_back(std::move(k), val);
        }
    }

    if (j.contains("is_chaser"))           j.at("is_chaser").get_to(v.is_chaser);
    if (j.contains("chaser_global_hold"))  j.at("chaser_global_hold").get_to(v.chaser_global_hold);
    if (j.contains("chaser_global_xfade")) j.at("chaser_global_xfade").get_to(v.chaser_global_xfade);
    if (j.contains("chaser_beat_sync"))    j.at("chaser_beat_sync").get_to(v.chaser_beat_sync);
    if (j.contains("chaser_beat_div"))     j.at("chaser_beat_div").get_to(v.chaser_beat_div);
    v.chaser_steps.clear();
    if (j.contains("chaser_steps"))        j.at("chaser_steps").get_to(v.chaser_steps);

    // 3-layer system — backward-compatible (optional fields)
    if (j.contains("global_layer"))   j.at("global_layer").get_to(v.global_layer);
    if (j.contains("keyframe_layer")) j.at("keyframe_layer").get_to(v.keyframe_layer);

    // Per-stream FX overrides — optional, absent in old files
    v.per_stream_fx.clear();
    if (j.contains("per_stream_fx")) {
        for (const auto& [sk, sv] : j.at("per_stream_fx").items()) {
            // L-18: wrap each entry so a single malformed stream entry does not
            // abort the entire cue deserialization (std::stoi throws on non-int
            // keys; from_json may throw on schema mismatch in corrupt files).
            try {
                int sid = std::stoi(sk);
                from_json(sv, v.per_stream_fx[sid]);
            } catch (...) {
                // Skip this entry; the stream will fall back to the cue's
                // default FX chain which is correct behaviour for unknown data.
            }
        }
    }

    // Per-stream keyframe layers — optional, absent in old files
    v.per_stream_kf.clear();
    if (j.contains("per_stream_kf")) {
        for (const auto& [key, val] : j.at("per_stream_kf").items()) {
            try {
                int sid = std::stoi(key);
                v.per_stream_kf[sid] = val.get<KeyframeLayer>();
            } catch (...) {}
        }
    }

    // Mirrored stream IDs — optional, absent in old files
    v.mirrored_ids.clear();
    if (j.contains("mirrored_ids"))
        j.at("mirrored_ids").get_to(v.mirrored_ids);
}

// ─────────────────────────────────────────────────────────────────────────────
//  GlobalLayer
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const GlobalLayer& v) {
    j = nlohmann::json{
        {"global_dim",  v.global_dim},
        {"strobe_rate", v.strobe_rate},
        {"strobe_duty", v.strobe_duty},
        {"size",        v.size},
        {"spread",      v.spread},
        {"fx",          v.fx}
    };
}

void from_json(const nlohmann::json& j, GlobalLayer& v) {
    v.global_dim  = j.value("global_dim",  1.f);
    v.strobe_rate = j.value("strobe_rate", 0.f);
    v.strobe_duty = j.value("strobe_duty", 0.5f);
    v.size        = j.value("size",        1.f);
    v.spread      = j.value("spread",      1.f);
    v.fx.clear();
    if (j.contains("fx")) j.at("fx").get_to(v.fx);

    // Backward compat: if old file had strobe_rate > 0 and no fx array,
    // convert legacy strobe into a Square GlobalFxEntry.
    if (v.fx.empty() && v.strobe_rate > 0.f) {
        GlobalFxEntry e;
        e.type    = GlobalFxType::Square;
        e.blend   = FxBlendMode::Absolute;
        e.rate    = v.strobe_rate;
        e.depth   = 1.f;
        e.duty    = v.strobe_duty;
        e.offset  = 0.f;
        e.enabled = true;
        v.fx.push_back(e);
        // Clear legacy fields to avoid double-applying
        v.strobe_rate = 0.f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  LaserObjectPoint
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const LaserObjectPoint& v) {
    j = nlohmann::json{ {"x", v.x}, {"y", v.y} };
}

void from_json(const nlohmann::json& j, LaserObjectPoint& v) {
    v.x = j.value("x", 0.f);
    v.y = j.value("y", 0.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  LaserObject  (skip 'selected' — UI-only, not serialised)
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const LaserObject& v) {
    j = nlohmann::json{
        {"id",        v.id},
        {"type",      v.type},
        {"pts",       v.pts},
        {"r",         v.r},
        {"g",         v.g},
        {"b",         v.b},
        {"thickness", v.thickness},
        {"size",      v.size}
    };
    if (!v.text.empty())
        j["text"] = v.text;
}

void from_json(const nlohmann::json& j, LaserObject& v) {
    v.id        = j.value("id",        static_cast<uint64_t>(0));
    v.type      = j.value("type",      LaserObjectType::Line);
    v.r         = j.value("r",         0.f);
    v.g         = j.value("g",         0.898f);
    v.b         = j.value("b",         1.f);
    v.thickness = j.value("thickness", 1.f);
    v.size      = j.value("size",      0.05f);
    v.text      = j.value("text",      std::string{});
    v.selected  = false; // never serialised
    if (j.contains("pts")) j.at("pts").get_to(v.pts);
}

// ─────────────────────────────────────────────────────────────────────────────
//  KeyframeLayer
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const KeyframeLayer& v) {
    j = nlohmann::json{
        {"objects",       v.objects},
        {"morph_time",    v.morph_time},
        {"fade_in_time",  v.fade_in_time},
        {"fade_out_time", v.fade_out_time},
        {"morph_curve",   v.morph_curve},
        {"symmetry_mode", v.symmetry_mode},
        {"sym_cx",        v.sym_cx},
        {"sym_cy",        v.sym_cy}
    };
}

void from_json(const nlohmann::json& j, KeyframeLayer& v) {
    v.morph_time    = j.value("morph_time",    0.5f);
    v.fade_in_time  = j.value("fade_in_time",  0.3f);
    v.fade_out_time = j.value("fade_out_time", 0.3f);
    v.morph_curve   = j.value("morph_curve",   PathInterp::SCurve);
    v.symmetry_mode = j.value("symmetry_mode", 0);
    v.sym_cx        = j.value("sym_cx",        0.f);
    v.sym_cy        = j.value("sym_cy",        0.f);
    v.objects.clear();
    if (j.contains("objects")) j.at("objects").get_to(v.objects);
}

// ─────────────────────────────────────────────────────────────────────────────
//  PlaybackConfig
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const PlaybackConfig& v) {
    // Store DmxMode as an integer (0=Off, 1=OneChannel, 2=TwoChannel)
    j = nlohmann::json{
        {"dmx_mode",      static_cast<int>(v.dmx_mode)},
        {"dmx_universe",  v.dmx_universe},
        {"dmx_channel",   v.dmx_channel},
        {"dmx_threshold", static_cast<int>(v.dmx_threshold)},
        {"end_behavior",  static_cast<int>(v.end_behavior)},
        {"go_at_bpm",               v.go_at_bpm},
        {"fx_at_bpm",               v.fx_at_bpm},
        {"keyboard_go_key",              v.keyboard_go_key},
        {"fade_on_first_trigger",        v.fade_on_first_trigger},
        {"first_trigger_fade_s",         v.first_trigger_fade_s},
        {"remember_cuelist_position",    v.remember_cuelist_position},
        {"output_stream_ids",            v.output_stream_ids}
    };
}

void from_json(const nlohmann::json& j, PlaybackConfig& v) {
    v.dmx_mode         = static_cast<PlaybackConfig::DmxMode>(j.value("dmx_mode",      0));
    v.dmx_universe     = j.value("dmx_universe",    0);
    v.dmx_channel      = j.value("dmx_channel",     0);
    v.dmx_threshold    = static_cast<uint8_t>(j.value("dmx_threshold", 10));
    v.end_behavior     = static_cast<PlaybackConfig::EndBehavior>(j.value("end_behavior", 0));
    v.go_at_bpm               = j.value("go_at_bpm",             false);
    v.fx_at_bpm               = j.value("fx_at_bpm",             false);
    v.keyboard_go_key         = j.value("keyboard_go_key",       0);
    v.fade_on_first_trigger        = j.value("fade_on_first_trigger",     false);
    v.first_trigger_fade_s         = j.value("first_trigger_fade_s",      0.f);
    v.remember_cuelist_position    = j.value("remember_cuelist_position", false);
    v.output_stream_ids            = j.value("output_stream_ids",         std::vector<int>{});
}

// ─────────────────────────────────────────────────────────────────────────────
//  PlaybackDef
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const PlaybackDef& v) {
    // Safety: never persist active state — playbacks must always start stopped.
    j = nlohmann::json{
        {"id",            v.id},
        {"name",          v.name},
        {"cuelist",       v.cuelist},
        {"intensity",     v.intensity},
        {"blind",         v.blind},
        {"dmx_universe",  v.dmx_universe},
        {"dmx_channel",   v.dmx_channel},
        {"dmx_threshold", v.dmx_threshold},
        {"config",        v.config}
    };
}

void from_json(const nlohmann::json& j, PlaybackDef& v) {
    v.id            = j.value("id",           0);
    v.name          = j.value("name",         std::string{});
    v.intensity     = j.value("intensity",    1.f);
    // Safety: active is NEVER restored from file — a laser show must not
    // auto-start outputs on load.  Ignore any "active" field in the JSON.
    v.active        = false;
    v.blind         = j.value("blind",        false);
    v.dmx_universe  = j.value("dmx_universe", -1);
    v.dmx_channel   = j.value("dmx_channel",  -1);
    v.dmx_threshold = j.value("dmx_threshold", static_cast<uint8_t>(64));
    if (j.contains("config"))  j.at("config").get_to(v.config);
    v.cuelist.clear();
    if (j.contains("cuelist")) j.at("cuelist").get_to(v.cuelist);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ColorPalette
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const ColorPalette::Slot& v) {
    j = nlohmann::json{
        {"r",    v.color.r},
        {"g",    v.color.g},
        {"b",    v.color.b},
        {"name", v.name},
        {"used", v.used}
    };
}

void from_json(const nlohmann::json& j, ColorPalette::Slot& v) {
    v.color.r = j.value("r",    1.f);
    v.color.g = j.value("g",    1.f);
    v.color.b = j.value("b",    1.f);
    v.name    = j.value("name", std::string{});
    v.used    = j.value("used", false);
}

void to_json(nlohmann::json& j, const ColorPalette& v) {
    j = nlohmann::json{
        {"name",  v.name},
        {"slots", nlohmann::json::array()}
    };
    for (const auto& s : v.slots)
        j["slots"].push_back(s);
}

void from_json(const nlohmann::json& j, ColorPalette& v) {
    v.name = j.value("name", std::string{"Default"});
    if (j.contains("slots") && j.at("slots").is_array()) {
        const auto& arr = j.at("slots");
        for (int i = 0; i < ColorPalette::kSlots && i < static_cast<int>(arr.size()); ++i)
            arr[i].get_to(v.slots[i]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  PositionPalette
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const PositionPalette::Slot& v) {
    j = nlohmann::json{
        {"x",        v.x},
        {"y",        v.y},
        {"scale",    v.scale},
        {"rotation", v.rotation},
        {"name",     v.name}
    };
}

void from_json(const nlohmann::json& j, PositionPalette::Slot& v) {
    v.x        = j.value("x",        0.f);
    v.y        = j.value("y",        0.f);
    v.scale    = j.value("scale",    1.f);
    v.rotation = j.value("rotation", 0.f);
    v.name     = j.value("name",     std::string{});
}

void to_json(nlohmann::json& j, const PositionPalette& v) {
    j = nlohmann::json{ {"slots", nlohmann::json::array()} };
    for (const auto& s : v.slots)
        j["slots"].push_back(s);
}

void from_json(const nlohmann::json& j, PositionPalette& v) {
    if (j.contains("slots") && j.at("slots").is_array()) {
        const auto& arr = j.at("slots");
        for (int i = 0; i < PositionPalette::kSlots && i < static_cast<int>(arr.size()); ++i)
            arr[i].get_to(v.slots[i]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  OutputSafetyConfig::BlockZone
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const OutputSafetyConfig::BlockZone& v) {
    j = nlohmann::json{
        {"enabled",   v.enabled},
        {"name",      v.name},
        {"cx",        v.cx},
        {"cy",        v.cy},
        {"hw",        v.hw},
        {"hh",        v.hh},
        {"angle_deg", v.angle_deg}
    };
}

void from_json(const nlohmann::json& j, OutputSafetyConfig::BlockZone& v) {
    v.enabled   = j.value("enabled",   true);
    v.name      = j.value("name",      std::string{"Zone"});
    v.cx        = j.value("cx",        0.f);
    v.cy        = j.value("cy",        0.f);
    v.hw        = j.value("hw",        0.1f);
    v.hh        = j.value("hh",        0.1f);
    v.angle_deg = j.value("angle_deg", 0.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  OutputSafetyConfig
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const OutputSafetyConfig& v) {
    j = nlohmann::json{
        {"enabled",       v.enabled},
        {"border_left",   v.border_left},
        {"border_right",  v.border_right},
        {"border_top",    v.border_top},
        {"border_bottom", v.border_bottom},
        {"border_tilt",   v.border_tilt},
        {"zones",         v.zones}
    };
}

void from_json(const nlohmann::json& j, OutputSafetyConfig& v) {
    v.enabled       = j.value("enabled",       false);
    v.border_left   = j.value("border_left",   0.f);
    v.border_right  = j.value("border_right",  0.f);
    v.border_top    = j.value("border_top",    0.f);
    v.border_bottom = j.value("border_bottom", 0.f);
    v.border_tilt   = j.value("border_tilt",   0.f);
    if (j.contains("zones")) j.at("zones").get_to(v.zones);
}

// ─────────────────────────────────────────────────────────────────────────────
//  OutputTransform
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const OutputTransform& v) {
    j = nlohmann::json{
        {"offset_x", v.offset_x},
        {"offset_y", v.offset_y},
        {"scale_x",  v.scale_x},
        {"scale_y",  v.scale_y},
        {"rotation", v.rotation},
        {"flip_x",   v.flip_x},
        {"flip_y",   v.flip_y}
    };
}

void from_json(const nlohmann::json& j, OutputTransform& v) {
    v.offset_x = j.value("offset_x", 0.f);
    v.offset_y = j.value("offset_y", 0.f);
    v.scale_x  = j.value("scale_x",  1.f);
    v.scale_y  = j.value("scale_y",  1.f);
    v.rotation = j.value("rotation", 0.f);
    v.flip_x   = j.value("flip_x",   false);
    v.flip_y   = j.value("flip_y",   false);
}

// ─────────────────────────────────────────────────────────────────────────────
//  OutputStreamConfig
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const OutputStreamConfig& v) {
    j = nlohmann::json{
        {"id",              v.id},
        {"name",            v.name},
        {"type",            v.type},
        {"enabled",         v.enabled},
        // Laser-specific
        {"dac_type",        v.dac_type},
        {"dac_address",     v.dac_address},
        {"point_rate",      v.point_rate},
        // NDI-specific
        {"ndi_name",        v.ndi_name},
        {"ndi_width",       v.ndi_width},
        {"ndi_height",      v.ndi_height},
        {"ndi_fps_N",       v.ndi_fps_N},
        {"ndi_fps_D",       v.ndi_fps_D},
        {"ndi_clock_video", v.ndi_clock_video},
        // Per-output transform and safety
        {"transform",       v.transform},
        {"safety",          v.safety}
    };
}

void from_json(const nlohmann::json& j, OutputStreamConfig& v) {
    v.id             = j.value("id",              0);
    v.name           = j.value("name",            std::string{"Output"});
    v.type           = j.value("type",            OutputStreamType::Laser);
    v.enabled        = j.value("enabled",         true);
    v.dac_type       = j.value("dac_type",        std::string{"auto"});
    v.dac_address    = j.value("dac_address",     std::string{});
    v.point_rate     = j.value("point_rate",      30000);
    v.ndi_name       = j.value("ndi_name",        std::string{"IDHMFIS"});
    v.ndi_width      = j.value("ndi_width",       1920);
    v.ndi_height     = j.value("ndi_height",      1080);
    v.ndi_fps_N      = j.value("ndi_fps_N",       60);
    v.ndi_fps_D      = j.value("ndi_fps_D",       1);
    v.ndi_clock_video= j.value("ndi_clock_video", true);
    if (j.contains("transform")) j.at("transform").get_to(v.transform);
    if (j.contains("safety"))    j.at("safety").get_to(v.safety);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Project::OutputGroup
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const Project::OutputGroup& v) {
    j = nlohmann::json{
        {"id",           v.id},
        {"name",         v.name},
        {"member_ids",   v.member_ids},
        {"mirrored_ids", v.mirrored_ids}
    };
}

void from_json(const nlohmann::json& j, Project::OutputGroup& v) {
    v.id   = j.value("id",   0);
    v.name = j.value("name", std::string{});
    if (j.contains("member_ids"))   j.at("member_ids").get_to(v.member_ids);
    if (j.contains("mirrored_ids")) j.at("mirrored_ids").get_to(v.mirrored_ids);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Project::ColorSwatch
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const Project::ColorSwatch& v) {
    j = nlohmann::json{
        {"r",    v.r},
        {"g",    v.g},
        {"b",    v.b},
        {"used", v.used}
    };
}

void from_json(const nlohmann::json& j, Project::ColorSwatch& v) {
    v.r    = j.value("r",    0.f);
    v.g    = j.value("g",    0.f);
    v.b    = j.value("b",    0.f);
    v.used = j.value("used", false);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Project::SafetyBlackoutConfig::BorderCrop
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const Project::SafetyBlackoutConfig::BorderCrop& v) {
    j = nlohmann::json{
        {"left",   v.left},
        {"right",  v.right},
        {"top",    v.top},
        {"bottom", v.bottom},
        {"tilt",   v.tilt}
    };
}

void from_json(const nlohmann::json& j, Project::SafetyBlackoutConfig::BorderCrop& v) {
    v.left   = j.value("left",   0.f);
    v.right  = j.value("right",  0.f);
    v.top    = j.value("top",    0.f);
    v.bottom = j.value("bottom", 0.f);
    v.tilt   = j.value("tilt",   0.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Project::SafetyBlackoutConfig::BlockZone
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const Project::SafetyBlackoutConfig::BlockZone& v) {
    j = nlohmann::json{
        {"enabled",   v.enabled},
        {"name",      v.name},
        {"cx",        v.cx},
        {"cy",        v.cy},
        {"hw",        v.hw},
        {"hh",        v.hh},
        {"angle_deg", v.angle_deg}
    };
}

void from_json(const nlohmann::json& j, Project::SafetyBlackoutConfig::BlockZone& v) {
    v.enabled   = j.value("enabled",   true);
    v.name      = j.value("name",      std::string{"Zone"});
    v.cx        = j.value("cx",        0.f);
    v.cy        = j.value("cy",        0.f);
    v.hw        = j.value("hw",        0.1f);
    v.hh        = j.value("hh",        0.1f);
    v.angle_deg = j.value("angle_deg", 0.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Project::SafetyBlackoutConfig
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const Project::SafetyBlackoutConfig& v) {
    j = nlohmann::json{
        {"enabled", v.enabled},
        {"borders", v.borders},
        {"zones",   v.zones}
    };
}

void from_json(const nlohmann::json& j, Project::SafetyBlackoutConfig& v) {
    v.enabled = j.value("enabled", true);
    if (j.contains("borders")) j.at("borders").get_to(v.borders);
    if (j.contains("zones"))   j.at("zones").get_to(v.zones);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Project
// ─────────────────────────────────────────────────────────────────────────────
void to_json(nlohmann::json& j, const Project& v) {
    j = nlohmann::json{
        // document identity
        {"schema_version",  v.schema_version},
        {"name",            v.name},
        {"author",          v.author},
        {"created_at",      v.created_at},
        {"modified_at",     v.modified_at},

        // content
        {"cues",            v.cues},
        {"cue_list",        v.cue_list},
        {"dmx_patches",     v.dmx_patches},
        {"full_cue_list",   v.full_cue_list},

        // render settings
        {"point_rate",      v.point_rate},
        {"ndi_width",       v.ndi_width},
        {"ndi_height",      v.ndi_height},
        {"ndi_fps",         v.ndi_fps},
        {"ndi_fps_N",       v.ndi_fps_N},
        {"ndi_fps_D",       v.ndi_fps_D},
        {"ndi_clock_video", v.ndi_clock_video},
        {"artnet_universe", v.artnet_universe},

        // ArtNet output settings
        {"artnet_out_ip",              v.artnet_out_ip},
        {"artnet_out_universe_offset", v.artnet_out_universe_offset},
        {"artnet_out_enabled",         v.artnet_out_enabled},

        // post-processing
        {"beam_thickness",  v.beam_thickness},
        {"bloom_radius",    v.bloom_radius},
        {"haze_density",    v.haze_density},
        {"exposure",        v.exposure},
        {"film_grain",      v.film_grain},

        // multiple playbacks
        {"playbacks",       v.playbacks},

        // UI key bindings
        {"bpm_tap_key",            v.bpm_tap_key},
        {"emergency_shutoff_key",  v.emergency_shutoff_key},

        // palettes
        {"color_palette",    v.color_palette},
        {"position_palette", v.position_palette},

        // safety blackout
        {"safety_blackout",  v.safety_blackout},

        // output patch
        {"output_patch",          v.output_patch},
        {"output_patch_next_id",  v.output_patch_next_id},

        // output groups
        {"output_groups",         v.output_groups},
        {"output_group_next_id",  v.output_group_next_id}
    };

    // color swatches — stored as array
    {
        auto& arr = j["color_swatches"] = nlohmann::json::array();
        for (int i = 0; i < Project::kNumSwatches; ++i)
            arr.push_back(v.color_swatches[i]);
    }

    // network config
    {
        const auto& nc = v.net_config;
        j["net_config"] = {
            {"iface_auto",              nc.iface_auto},
            {"iface_index",             nc.iface_index},
            {"artnet_enabled",          nc.artnet_enabled},
            {"artnet_auto",             nc.artnet_auto},
            {"artnet_universe",         nc.artnet_universe},
            {"artnet_net",              nc.artnet_net},
            {"artnet_subnet",           nc.artnet_subnet},
            {"artnet_listen_ip",        nc.artnet_listen_ip},
            {"artnet_port",             nc.artnet_port},
            {"artnet_merge_htp",        nc.artnet_merge_htp},
            {"artnet_merge_mode",       nc.artnet_merge_mode},
            {"artnet_priority",         nc.artnet_priority},
            {"sacn_enabled",            nc.sacn_enabled},
            {"sacn_auto",               nc.sacn_auto},
            {"sacn_universe",           nc.sacn_universe},
            {"sacn_priority",           nc.sacn_priority},
            {"sacn_multicast_ip",       nc.sacn_multicast_ip},
            {"sacn_port",               nc.sacn_port},
            {"sacn_per_universe",       nc.sacn_per_universe},
            {"osc_in_enabled",          nc.osc_in_enabled},
            {"osc_in_auto",             nc.osc_in_auto},
            {"osc_in_port",             nc.osc_in_port},
            {"osc_in_ip",               nc.osc_in_ip},
            {"osc_out_enabled",         nc.osc_out_enabled},
            {"osc_out_auto",            nc.osc_out_auto},
            {"osc_out_ip",              nc.osc_out_ip},
            {"osc_out_port",            nc.osc_out_port},
            {"osc_prefix",              nc.osc_prefix},
            {"citp_enabled",            nc.citp_enabled},
            {"citp_auto",               nc.citp_auto},
            {"citp_tcp_port",           nc.citp_tcp_port},
            {"citp_multicast_group",    nc.citp_multicast_group},
            {"citp_multicast_port",     nc.citp_multicast_port},
            {"citp_source_name",        nc.citp_source_name},
            {"citp_respond_capture",    nc.citp_respond_capture},
            {"idn_enabled",             nc.idn_enabled},
            {"idn_auto",                nc.idn_auto},
            {"idn_broadcast_addr",      nc.idn_broadcast_addr},
            {"idn_port",                nc.idn_port},
            {"idn_channel",             nc.idn_channel},
            {"cls_enabled",             nc.cls_enabled},
            {"cls_auto",                nc.cls_auto},
            {"cls_tcp_port",            nc.cls_tcp_port},
            {"cls_udp_port",            nc.cls_udp_port},
            {"cls_udp_broadcast",       nc.cls_udp_broadcast},
            {"etherdream_enabled",      nc.etherdream_enabled},
            {"etherdream_auto",         nc.etherdream_auto},
            {"etherdream_timeout",      nc.etherdream_timeout},
            {"etherdream_preferred_ip", nc.etherdream_preferred_ip},
            {"ndi_net_auto",            nc.ndi_net_auto},
            {"ndi_net_source_name",     nc.ndi_net_source_name},
            {"ndi_net_bandwidth",       nc.ndi_net_bandwidth},
            {"ndi_net_fps",             nc.ndi_net_fps},
            {"dmx_universe_offset",      nc.dmx_universe_offset}
        };
    }

    // output config
    {
        const auto& oc = v.output_config;
        j["output_config"] = {
            {"ndi_enabled",            oc.ndi_enabled},
            {"ndi_name",               oc.ndi_name},
            {"ndi_width",              oc.ndi_width},
            {"ndi_height",             oc.ndi_height},
            {"ndi_fps",                oc.ndi_fps},
            {"ndi_fps_N",              oc.ndi_fps_N},
            {"ndi_fps_D",              oc.ndi_fps_D},
            {"ndi_clock_video",        oc.ndi_clock_video},
            {"beam_radius",            oc.beam_radius},
            {"glow_radius",            oc.glow_radius},
            {"glow_alpha",             oc.glow_alpha},
            {"hdmi_window_enabled",        oc.hdmi_window_enabled},
            {"dac_enabled",                oc.dac_enabled},
            {"idn_stream_enabled",         oc.idn_stream_enabled},
            {"virtual_camera_enabled",     oc.virtual_camera_enabled},
            {"stream_type_dac_enabled",    oc.stream_type_dac_enabled},
            {"stream_type_ndi_enabled",    oc.stream_type_ndi_enabled},
            {"stream_type_artnet_enabled", oc.stream_type_artnet_enabled},
            {"stream_type_idn_enabled",    oc.stream_type_idn_enabled}
        };
    }

    // 3D preview settings
    {
        const auto& p3 = v.preview_3d;
        j["preview_3d"] = {
            {"scan_mode",       p3.scan_mode},
            {"scan_speed",      p3.scan_speed},
            {"trail_pct",       p3.trail_pct},
            {"beam_brightness", p3.beam_brightness},
            {"haze_alpha",      p3.haze_alpha},
            {"wall_glow_px",    p3.wall_glow_px},
            {"beam_width_px",   p3.beam_width_px},
            {"room_half_width", p3.room_half_width},
            {"room_height",     p3.room_height},
            {"room_depth",      p3.room_depth},
            {"proj_scale",      p3.proj_scale}
        };
    }

    // laser placements
    {
        auto& arr = j["laser_placements"] = nlohmann::json::array();
        for (const auto& lp : v.laser_placements) {
            arr.push_back({
                {"stream_id", lp.stream_id},
                {"pos_x",     lp.pos_x},
                {"pos_y",     lp.pos_y},
                {"pos_z",     lp.pos_z},
                {"yaw",       lp.yaw},
                {"pitch",     lp.pitch}
            });
        }
    }

    // color input mode
    j["color_input_mode"] = v.color_input_mode;

    // otaniemi config
    {
        const auto& ot = v.otaniemi;
        j["otaniemi"] = {
            {"enabled",          ot.enabled},
            {"line_thickness",   ot.line_thickness},
            {"auto_fill_shapes", ot.auto_fill_shapes},
            {"brightness_boost", ot.brightness_boost},
            {"glow_radius",      ot.glow_radius}
        };
    }

    // active stream ids
    j["active_stream_ids"] = v.active_stream_ids;

    // broadcast mode
    j["broadcast_to_all"] = v.broadcast_to_all;

    // timeline system
    j["timelines"] = v.timelines;
    j["tc_config"] = v.tc_config;

    // ui layout state
    j["ui_layout"] = {
        {"active_view",             v.ui_layout.active_view},
        {"show_layout_initialised", v.ui_layout.show_layout_initialised},
        {"dockspace_initialised",   v.ui_layout.dockspace_initialised}
    };
}

void from_json(const nlohmann::json& j, Project& v) {
    v.schema_version  = j.value("schema_version",  std::string{"2.9.0"});
    v.name            = j.value("name",            std::string{});
    v.author          = j.value("author",          std::string{});
    v.created_at      = j.value("created_at",      std::string{});
    v.modified_at     = j.value("modified_at",     std::string{});

    v.cues.clear();
    if (j.contains("cues") && j.at("cues").is_array()) {
        for (const auto& item : j.at("cues")) {
            try { Cue c; item.get_to(c); v.cues.push_back(std::move(c)); }
            catch (...) {}
        }
    }
    // BUG #64: guard all array fields with is_array() before iterating to
    // prevent nlohmann::json type_error crashes on malformed project files.
    v.cue_list.clear();
    if (j.contains("cue_list") && j.at("cue_list").is_array())
        j.at("cue_list").get_to(v.cue_list);
    v.dmx_patches.clear();
    if (j.contains("dmx_patches") && j.at("dmx_patches").is_array())
        j.at("dmx_patches").get_to(v.dmx_patches);
    v.full_cue_list.clear();
    if (j.contains("full_cue_list") && j.at("full_cue_list").is_array()) {
        for (const auto& item : j.at("full_cue_list")) {
            try { FullCueEntry fce; item.get_to(fce); v.full_cue_list.push_back(std::move(fce)); }
            catch (...) {}
        }
    }

    v.point_rate      = j.value("point_rate",      kDefaultPointRate);
    v.ndi_width       = j.value("ndi_width",       kDefaultNDIWidth);
    v.ndi_height      = j.value("ndi_height",      kDefaultNDIHeight);
    v.ndi_fps         = j.value("ndi_fps",         kDefaultNDIFPS);
    v.ndi_fps_N       = j.value("ndi_fps_N",       v.ndi_fps);  // fallback to legacy ndi_fps
    v.ndi_fps_D       = j.value("ndi_fps_D",       1);
    v.ndi_clock_video = j.value("ndi_clock_video", true);
    v.artnet_universe = j.value("artnet_universe",  kDefaultUniverse);

    v.artnet_out_ip              = j.value("artnet_out_ip",              std::string{"2.255.255.255"});
    v.artnet_out_universe_offset = j.value("artnet_out_universe_offset", 0);
    v.artnet_out_enabled         = j.value("artnet_out_enabled",         false);

    v.beam_thickness  = j.value("beam_thickness",  2.0f);
    v.bloom_radius    = j.value("bloom_radius",    8.0f);
    v.haze_density    = j.value("haze_density",    0.4f);
    v.exposure        = j.value("exposure",        1.0f);
    v.film_grain      = j.value("film_grain",      false);

    v.playbacks.clear();
    if (j.contains("playbacks") && j.at("playbacks").is_array()) {
        for (const auto& item : j.at("playbacks")) {
            try { PlaybackDef pb; item.get_to(pb); v.playbacks.push_back(std::move(pb)); }
            catch (...) {}
        }
    }

    v.bpm_tap_key           = j.value("bpm_tap_key",           0);
    v.emergency_shutoff_key = j.value("emergency_shutoff_key", 0);

    // palettes (optional — old files get defaults)
    if (j.contains("color_palette"))    j.at("color_palette").get_to(v.color_palette);
    if (j.contains("position_palette")) j.at("position_palette").get_to(v.position_palette);

    // safety blackout (optional — old files get defaults)
    if (j.contains("safety_blackout")) j.at("safety_blackout").get_to(v.safety_blackout);

    // color swatches (optional — old files get zeroed slots)
    if (j.contains("color_swatches") && j.at("color_swatches").is_array()) {
        const auto& arr = j.at("color_swatches");
        for (int i = 0; i < Project::kNumSwatches && i < static_cast<int>(arr.size()); ++i)
            arr[i].get_to(v.color_swatches[i]);
    }

    // output patch (optional — old files start with empty patch)
    v.output_patch.clear();
    if (j.contains("output_patch")) j.at("output_patch").get_to(v.output_patch);
    v.output_patch_next_id = j.value("output_patch_next_id", 0);
    // Ensure next_id is at least max(existing id) + 1 to prevent ID collisions
    for (const auto& cfg : v.output_patch)
        if (cfg.id >= v.output_patch_next_id)
            v.output_patch_next_id = cfg.id + 1;

    // output groups (optional — old files start with no groups)
    v.output_groups.clear();
    if (j.contains("output_groups")) j.at("output_groups").get_to(v.output_groups);
    v.output_group_next_id = j.value("output_group_next_id", 1);
    for (const auto& grp : v.output_groups)
        if (grp.id >= v.output_group_next_id)
            v.output_group_next_id = grp.id + 1;

    // network config (optional — old files get defaults)
    if (j.contains("net_config")) {
        const auto& jn = j.at("net_config");
        auto& nc = v.net_config;
        nc.iface_auto          = jn.value("iface_auto",          true);
        nc.iface_index         = jn.value("iface_index",         0);
        nc.artnet_enabled      = jn.value("artnet_enabled",      false);
        nc.artnet_auto         = jn.value("artnet_auto",         true);
        nc.artnet_universe     = jn.value("artnet_universe",     0);
        nc.artnet_net          = jn.value("artnet_net",          0);
        nc.artnet_subnet       = jn.value("artnet_subnet",       0);
        nc.artnet_listen_ip    = jn.value("artnet_listen_ip",    std::string{"0.0.0.0"});
        nc.artnet_port         = jn.value("artnet_port",         6454);
        nc.artnet_merge_htp    = jn.value("artnet_merge_htp",    true);
        nc.artnet_merge_mode   = jn.value("artnet_merge_mode",   0);
        nc.artnet_priority     = jn.value("artnet_priority",     100);
        nc.sacn_enabled        = jn.value("sacn_enabled",        false);
        nc.sacn_auto           = jn.value("sacn_auto",           true);
        nc.sacn_universe       = jn.value("sacn_universe",       1);
        nc.sacn_priority       = jn.value("sacn_priority",       100);
        nc.sacn_multicast_ip   = jn.value("sacn_multicast_ip",   std::string{"239.255.0.1"});
        nc.sacn_port           = jn.value("sacn_port",           5568);
        nc.sacn_per_universe   = jn.value("sacn_per_universe",   true);
        nc.osc_in_enabled      = jn.value("osc_in_enabled",      true);
        nc.osc_in_auto         = jn.value("osc_in_auto",         true);
        nc.osc_in_port         = jn.value("osc_in_port",         7700);
        nc.osc_in_ip           = jn.value("osc_in_ip",           std::string{"0.0.0.0"});
        nc.osc_out_enabled     = jn.value("osc_out_enabled",     false);
        nc.osc_out_auto        = jn.value("osc_out_auto",        true);
        nc.osc_out_ip          = jn.value("osc_out_ip",          std::string{"127.0.0.1"});
        nc.osc_out_port        = jn.value("osc_out_port",        7701);
        nc.osc_prefix          = jn.value("osc_prefix",          std::string{"/idhmfis/"});
        nc.citp_enabled        = jn.value("citp_enabled",        false);
        nc.citp_auto           = jn.value("citp_auto",           true);
        nc.citp_tcp_port       = jn.value("citp_tcp_port",       6430);
        nc.citp_multicast_group= jn.value("citp_multicast_group",std::string{"239.224.0.180"});
        nc.citp_multicast_port = jn.value("citp_multicast_port", 4809);
        nc.citp_source_name    = jn.value("citp_source_name",    std::string{"IDHMFIS"});
        nc.citp_respond_capture= jn.value("citp_respond_capture",true);
        nc.idn_enabled         = jn.value("idn_enabled",         false);
        nc.idn_auto            = jn.value("idn_auto",            true);
        nc.idn_broadcast_addr  = jn.value("idn_broadcast_addr",  std::string{"255.255.255.255"});
        nc.idn_port            = jn.value("idn_port",            7255);
        nc.idn_channel         = jn.value("idn_channel",         0);
        nc.cls_enabled         = jn.value("cls_enabled",         false);
        nc.cls_auto            = jn.value("cls_auto",            true);
        nc.cls_tcp_port        = jn.value("cls_tcp_port",        7256);
        nc.cls_udp_port        = jn.value("cls_udp_port",        7256);
        nc.cls_udp_broadcast   = jn.value("cls_udp_broadcast",   true);
        nc.etherdream_enabled  = jn.value("etherdream_enabled",  false);
        nc.etherdream_auto     = jn.value("etherdream_auto",     true);
        nc.etherdream_timeout  = jn.value("etherdream_timeout",  2.0f);
        nc.etherdream_preferred_ip = jn.value("etherdream_preferred_ip", std::string{});
        nc.ndi_net_auto        = jn.value("ndi_net_auto",        true);
        nc.ndi_net_source_name = jn.value("ndi_net_source_name", std::string{"IDHMFIS Laser Preview"});
        nc.ndi_net_bandwidth   = jn.value("ndi_net_bandwidth",   1);
        nc.ndi_net_fps         = jn.value("ndi_net_fps",         30);
        // New key "dmx_universe_offset"; fall back to old "dmx_channel_offset" for
        // files saved before v4.09 so existing settings are not silently lost.
        nc.dmx_universe_offset = jn.contains("dmx_universe_offset")
                                     ? jn.value("dmx_universe_offset", 0)
                                     : jn.value("dmx_channel_offset",  0);
    }

    // output config (optional — old files get defaults)
    if (j.contains("output_config")) {
        const auto& joc = j.at("output_config");
        auto& oc = v.output_config;
        oc.ndi_enabled            = joc.value("ndi_enabled",            true);
        oc.ndi_name               = joc.value("ndi_name",               std::string{"IDHMFIS"});
        oc.ndi_width              = joc.value("ndi_width",              1920);
        oc.ndi_height             = joc.value("ndi_height",             1080);
        oc.ndi_fps                = joc.value("ndi_fps",                60);
        oc.ndi_fps_N              = joc.value("ndi_fps_N",              60);
        oc.ndi_fps_D              = joc.value("ndi_fps_D",              1);
        oc.ndi_clock_video        = joc.value("ndi_clock_video",        true);
        oc.beam_radius            = joc.value("beam_radius",            2.5f);
        oc.glow_radius            = joc.value("glow_radius",            8.f);
        oc.glow_alpha             = joc.value("glow_alpha",             0.25f);
        oc.hdmi_window_enabled        = joc.value("hdmi_window_enabled",        false);
        oc.dac_enabled                = joc.value("dac_enabled",                true);
        oc.idn_stream_enabled         = joc.value("idn_stream_enabled",         false);
        oc.virtual_camera_enabled     = joc.value("virtual_camera_enabled",     false);
        oc.stream_type_dac_enabled    = joc.value("stream_type_dac_enabled",    true);
        oc.stream_type_ndi_enabled    = joc.value("stream_type_ndi_enabled",    true);
        oc.stream_type_artnet_enabled = joc.value("stream_type_artnet_enabled", true);
        oc.stream_type_idn_enabled    = joc.value("stream_type_idn_enabled",    true);
    }

    // 3D preview settings (optional — old files get defaults)
    if (j.contains("preview_3d")) {
        const auto& jp = j.at("preview_3d");
        auto& p3 = v.preview_3d;
        p3.scan_mode       = jp.value("scan_mode",       false);
        p3.scan_speed      = jp.value("scan_speed",      0.8f);
        p3.trail_pct       = jp.value("trail_pct",       20);
        p3.beam_brightness = jp.value("beam_brightness", 1.0f);
        p3.haze_alpha      = jp.value("haze_alpha",      0.35f);
        p3.wall_glow_px    = jp.value("wall_glow_px",    5.f);
        p3.beam_width_px   = jp.value("beam_width_px",   1.5f);
        p3.room_half_width = jp.value("room_half_width", 6.f);
        p3.room_height     = jp.value("room_height",     5.f);
        p3.room_depth      = jp.value("room_depth",      20.f);
        p3.proj_scale      = jp.value("proj_scale",      3.0f);
    }

    // laser placements (optional)
    if (j.contains("laser_placements") && j.at("laser_placements").is_array()) {
        v.laser_placements.clear();
        for (const auto& jlp : j.at("laser_placements")) {
            Project::LaserPlacement3D lp;
            lp.stream_id = jlp.value("stream_id", -1);
            lp.pos_x     = jlp.value("pos_x",     0.f);
            lp.pos_y     = jlp.value("pos_y",     3.f);
            lp.pos_z     = jlp.value("pos_z",     0.5f);
            lp.yaw       = jlp.value("yaw",       0.f);
            lp.pitch     = jlp.value("pitch",     0.f);
            v.laser_placements.push_back(lp);
        }
    }

    // color input mode (optional, default 0 = RGBPercent)
    v.color_input_mode = j.value("color_input_mode", 0);

    // otaniemi config (optional)
    if (j.contains("otaniemi")) {
        const auto& jo = j.at("otaniemi");
        auto& ot = v.otaniemi;
        ot.enabled          = jo.value("enabled",          false);
        ot.line_thickness   = jo.value("line_thickness",   4.0f);
        ot.auto_fill_shapes = jo.value("auto_fill_shapes", true);
        ot.brightness_boost = jo.value("brightness_boost", 1.3f);
        ot.glow_radius      = jo.value("glow_radius",      8.0f);
    }

    // active stream ids (optional)
    v.active_stream_ids.clear();
    if (j.contains("active_stream_ids") && j.at("active_stream_ids").is_array())
        j.at("active_stream_ids").get_to(v.active_stream_ids);

    // broadcast mode (optional — default true for backward compat)
    v.broadcast_to_all = j.value("broadcast_to_all", true);

    // timeline system (optional — old files get empty defaults)
    v.timelines.clear();
    if (j.contains("timelines") && j.at("timelines").is_array()) {
        for (const auto& item : j.at("timelines")) {
            try { TimelineDef td; item.get_to(td); v.timelines.push_back(std::move(td)); }
            catch (...) {}
        }
    }
    if (j.contains("tc_config"))
        j.at("tc_config").get_to(v.tc_config);

    // ui layout state (optional — old files get defaults)
    if (j.contains("ui_layout") && j.at("ui_layout").is_object()) {
        const auto& jl = j.at("ui_layout");
        v.ui_layout.active_view             = jl.value("active_view",             0);
        v.ui_layout.show_layout_initialised = jl.value("show_layout_initialised", false);
        v.ui_layout.dockspace_initialised   = jl.value("dockspace_initialised",   false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Timeline serialization
// ─────────────────────────────────────────────────────────────────────────────

void to_json(nlohmann::json& j, const TimelineEvent& v) {
    j = nlohmann::json{
        {"id",          v.id},
        {"tc_position", v.tc_position},
        {"type",        v.type},
        {"target_id",   v.target_id},
        {"label",       v.label}
    };
    if (!v.params.empty()) {
        auto& jp = j["params"];
        for (const auto& [k, val] : v.params)
            jp[k] = val;
    }
}

void from_json(const nlohmann::json& j, TimelineEvent& v) {
    v.id          = j.value("id",          int64_t{0});
    v.tc_position = j.value("tc_position", int64_t{0});
    v.type        = j.value("type",        TimelineEventType::Marker);
    v.target_id   = j.value("target_id",   std::string{});
    v.label       = j.value("label",       std::string{});
    v.params.clear();
    if (j.contains("params") && j.at("params").is_object()) {
        for (const auto& [k, val] : j.at("params").items())
            if (val.is_number()) v.params[k] = val.get<float>();
    }
}

void to_json(nlohmann::json& j, const TimelineTrack& v) {
    j = nlohmann::json{
        {"id",        v.id},
        {"name",      v.name},
        {"muted",     v.muted},
        {"locked",    v.locked},
        {"collapsed", v.collapsed},
        {"events",    v.events}
    };
}

void from_json(const nlohmann::json& j, TimelineTrack& v) {
    v.id        = j.value("id",        0);
    v.name      = j.value("name",      std::string{"Track"});
    v.muted     = j.value("muted",     false);
    v.locked    = j.value("locked",    false);
    v.collapsed = j.value("collapsed", false);
    v.events.clear();
    if (j.contains("events") && j.at("events").is_array())
        j.at("events").get_to(v.events);
}

void to_json(nlohmann::json& j, const TimelineDef::DmxTrigger& v) {
    j = nlohmann::json{
        {"universe",  v.universe},
        {"channel",   v.channel},
        {"threshold", v.threshold},
        {"enabled",   v.enabled}
    };
}

void from_json(const nlohmann::json& j, TimelineDef::DmxTrigger& v) {
    v.universe  = j.value("universe",  0);
    v.channel   = j.value("channel",   1);
    v.threshold = j.value("threshold", uint8_t{127});
    v.enabled   = j.value("enabled",   false);
}

void to_json(nlohmann::json& j, const TimelineDef& v) {
    j = nlohmann::json{
        {"id",                   v.id},
        {"name",                 v.name},
        {"fps",                  v.fps},
        {"length_frames",        v.length_frames},
        {"time_offset_frames",   v.time_offset_frames},
        {"tc_slot",              v.tc_slot},
        {"link_mode",            v.link_mode},
        {"record_armed",         v.record_armed},
        {"dmx_trigger",          v.dmx_trigger},
        {"tracks",               v.tracks}
    };
    // Serialize audio_track only when present; never serialize audio_peaks
    // (peaks are recomputed from the audio file on load).
    if (v.audio_track) {
        j["audio_track"] = {
            {"file_path",     v.audio_track->file_path},
            {"offset_frames", v.audio_track->offset_frames},
            {"volume",        v.audio_track->volume}
        };
    }
}

void from_json(const nlohmann::json& j, TimelineDef& v) {
    v.id                 = j.value("id",                 std::string{});
    v.name               = j.value("name",               std::string{"Timeline"});
    v.fps                = j.value("fps",                SmpteRate::Fps25);
    // BUG #72: clamp length_frames and time_offset_frames to prevent extreme
    // values from corrupt/malformed project files causing arithmetic overflow
    // or UI hangs in the timeline rendering code.
    v.length_frames      = std::min(j.value("length_frames",      int64_t{0}),
                                    int64_t{INT64_MAX / 2});
    v.time_offset_frames = std::min(j.value("time_offset_frames", int64_t{0}),
                                    int64_t{INT64_MAX / 2});
    v.tc_slot            = j.value("tc_slot",            std::string{"Default"});
    v.link_mode          = j.value("link_mode",          true);
    v.record_armed       = j.value("record_armed",       false);
    v.tracks.clear();
    if (j.contains("dmx_trigger")) j.at("dmx_trigger").get_to(v.dmx_trigger);
    if (j.contains("tracks") && j.at("tracks").is_array())
        j.at("tracks").get_to(v.tracks);
    // audio_track — optional; audio_peaks is always omitted (recomputed on load)
    v.audio_track = std::nullopt;
    if (j.contains("audio_track") && j.at("audio_track").is_object()) {
        const auto& jat = j.at("audio_track");
        AudioTrackDef atd;
        atd.file_path     = jat.value("file_path",     std::string{});
        atd.offset_frames = jat.value("offset_frames", int64_t{0});
        atd.volume        = jat.value("volume",        1.0f);
        v.audio_track     = atd;
    }
    v.audio_peaks.clear(); // never loaded from file; caller must recompute
}

void to_json(nlohmann::json& j, const TimecodeSettings& v) {
    j = nlohmann::json{
        {"source",              v.source},
        {"fps",                 v.fps},
        {"enabled",             v.enabled},
        {"jump_detect_frames",  v.jump_detect_frames},
        {"freewheel_frames",    v.freewheel_frames}
    };
}

void from_json(const nlohmann::json& j, TimecodeSettings& v) {
    v.source             = j.value("source",             TimecodeSource::Internal);
    v.fps                = j.value("fps",                SmpteRate::Fps25);
    v.enabled            = j.value("enabled",            false);
    v.jump_detect_frames = j.value("jump_detect_frames", 10);
    v.freewheel_frames   = j.value("freewheel_frames",   5);
}

void to_json(nlohmann::json& j, const ProjectTimecodeConfig& v) {
    nlohmann::json jslots = nlohmann::json::object();
    for (const auto& [k, s] : v.slots)
        jslots[k] = s;
    j = nlohmann::json{ {"slots", jslots} };
}

void from_json(const nlohmann::json& j, ProjectTimecodeConfig& v) {
    v = ProjectTimecodeConfig{};   // reset to defaults first
    if (j.contains("slots") && j.at("slots").is_object()) {
        for (const auto& [k, jv] : j.at("slots").items()) {
            TimecodeSettings s;
            jv.get_to(s);
            v.slots[k] = s;
        }
    }
}

} // namespace idhmfis
