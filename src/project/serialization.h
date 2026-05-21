#pragma once
// serialization.h — declares all ADL to_json / from_json overloads used by
// Project.  project.h already includes nlohmann/json.hpp so this file only
// needs to include project.h.

#include "project.h"
// cuelist_types.h is already transitively included through project.h.
// timeline_types.h is transitively included through project.h.

namespace idhmfis {

// ── ADL overloads for nlohmann/json ──────────────────────────────────────────

// GeneratorType
NLOHMANN_JSON_SERIALIZE_ENUM(GeneratorType, {
    { GeneratorType::Beams,          "Beams"          },
    { GeneratorType::Waves,          "Waves"          },
    { GeneratorType::Lissajous,      "Lissajous"      },
    { GeneratorType::Tunnel,         "Tunnel"         },
    { GeneratorType::TextScroller,   "TextScroller"   },
    { GeneratorType::Oscilloscope,   "Oscilloscope"   },
    { GeneratorType::FFTBars,        "FFTBars"        },
    { GeneratorType::Spirograph,     "Spirograph"     },
    { GeneratorType::ParticleField,  "ParticleField"  },
    { GeneratorType::GeometricMorph, "GeometricMorph" },
    { GeneratorType::Ribbon,         "Ribbon"         },
    { GeneratorType::Grid,           "Grid"           },
    { GeneratorType::Starburst,      "Starburst"      },
    { GeneratorType::FanSweep,       "FanSweep"       },
    { GeneratorType::ConeSweep,      "ConeSweep"      },
    { GeneratorType::ILDASequence,   "ILDASequence"   },
    { GeneratorType::Custom,         "Custom"         },
})

// KeyframePoint::Interp
NLOHMANN_JSON_SERIALIZE_ENUM(KeyframePoint::Interp, {
    { KeyframePoint::Interp::Linear,      "Linear"      },
    { KeyframePoint::Interp::Step,        "Step"        },
    { KeyframePoint::Interp::CubicBezier, "CubicBezier" },
})

void to_json(nlohmann::json& j, const Color4& v);
void from_json(const nlohmann::json& j, Color4& v);

void to_json(nlohmann::json& j, const GeneratorParams& v);
void from_json(const nlohmann::json& j, GeneratorParams& v);

void to_json(nlohmann::json& j, const KeyframePoint& v);
void from_json(const nlohmann::json& j, KeyframePoint& v);

void to_json(nlohmann::json& j, const ParamTrack& v);
void from_json(const nlohmann::json& j, ParamTrack& v);

void to_json(nlohmann::json& j, const Cue& v);
void from_json(const nlohmann::json& j, Cue& v);

void to_json(nlohmann::json& j, const DmxFixtureProfile::ChannelDef& v);
void from_json(const nlohmann::json& j, DmxFixtureProfile::ChannelDef& v);

void to_json(nlohmann::json& j, const DmxFixtureProfile& v);
void from_json(const nlohmann::json& j, DmxFixtureProfile& v);

void to_json(nlohmann::json& j, const DmxPatch& v);
void from_json(const nlohmann::json& j, DmxPatch& v);

void to_json(nlohmann::json& j, const CueListEntry& v);
void from_json(const nlohmann::json& j, CueListEntry& v);

// ── CueList types ─────────────────────────────────────────────────────────────

NLOHMANN_JSON_SERIALIZE_ENUM(PathInterp, {
    { PathInterp::Linear,     "Linear"     },
    { PathInterp::SCurve,     "SCurve"     },
    { PathInterp::EaseIn,     "EaseIn"     },
    { PathInterp::EaseOut,    "EaseOut"    },
    { PathInterp::EaseInOut,  "EaseInOut"  },
    { PathInterp::SnapLate,   "SnapLate"   },
    { PathInterp::SnapEarly,  "SnapEarly"  },
    { PathInterp::Expression, "Expression" },
})

NLOHMANN_JSON_SERIALIZE_ENUM(FanMode, {
    { FanMode::None,       "None"      },
    { FanMode::FrontBack,  "FrontBack" },
    { FanMode::Even,       "Even"      },
    { FanMode::CenterOut,  "CenterOut" },
    { FanMode::EndsOut,    "EndsOut"   },
    { FanMode::Random,     "Random"    },
})

NLOHMANN_JSON_SERIALIZE_ENUM(LinkMode, {
    { LinkMode::Stop,  "Stop"  },
    { LinkMode::Loop,  "Loop"  },
    { LinkMode::Next,  "Next"  },
    { LinkMode::Jump,  "Jump"  },
})

NLOHMANN_JSON_SERIALIZE_ENUM(TriggerType, {
    { TriggerType::Halt,      "Halt"      },
    { TriggerType::Follow,    "Follow"    },
    { TriggerType::Wait,      "Wait"      },
    { TriggerType::Timecode,  "Timecode"  },
    { TriggerType::MIDI,      "MIDI"      },
    { TriggerType::OSC,       "OSC"       },
    { TriggerType::DMX,       "DMX"       },
    { TriggerType::Audio,     "Audio"     },
})

NLOHMANN_JSON_SERIALIZE_ENUM(TrackingMode, {
    { TrackingMode::Tracking, "Tracking" },
    { TrackingMode::CueOnly,  "CueOnly"  },
})

void to_json(nlohmann::json& j, const CueNumber& v);
void from_json(const nlohmann::json& j, CueNumber& v);

void to_json(nlohmann::json& j, const SplitTimes& v);
void from_json(const nlohmann::json& j, SplitTimes& v);

void to_json(nlohmann::json& j, const CueTimingBlock& v);
void from_json(const nlohmann::json& j, CueTimingBlock& v);

void to_json(nlohmann::json& j, const CueTrigger& v);
void from_json(const nlohmann::json& j, CueTrigger& v);

void to_json(nlohmann::json& j, const FxStackDiff& v);
void from_json(const nlohmann::json& j, FxStackDiff& v);

NLOHMANN_JSON_SERIALIZE_ENUM(GlobalFxType, {
    { GlobalFxType::Sine,     "Sine"     },
    { GlobalFxType::Square,   "Square"   },
    { GlobalFxType::Saw,      "Saw"      },
    { GlobalFxType::Triangle, "Triangle" },
    { GlobalFxType::Flicker,  "Flicker"  },
    { GlobalFxType::RampUp,   "RampUp"   },
    { GlobalFxType::RampDown, "RampDown" },
    { GlobalFxType::Bump,     "Bump"     },
    { GlobalFxType::Chase,    "Chase"    },
    { GlobalFxType::Strobe,   "Strobe"   },
})

NLOHMANN_JSON_SERIALIZE_ENUM(FxDirection, {
    { FxDirection::Sync,      "Sync"      },
    { FxDirection::Forward,   "Forward"   },
    { FxDirection::Backward,  "Backward"  },
    { FxDirection::CentreOut, "CentreOut" },
    { FxDirection::CentreIn,  "CentreIn"  },
    { FxDirection::OddEven,   "OddEven"   },
    { FxDirection::Random,    "Random"    },
})

NLOHMANN_JSON_SERIALIZE_ENUM(FxBlendMode, {
    { FxBlendMode::Add,      "Add"      },
    { FxBlendMode::Subtract, "Subtract" },
    { FxBlendMode::Absolute, "Absolute" },
    { FxBlendMode::Multiply, "Multiply" },
})

NLOHMANN_JSON_SERIALIZE_ENUM(FrameFxType, {
    { FrameFxType::PanX,         "PanX"         },
    { FrameFxType::PanY,         "PanY"         },
    { FrameFxType::Rotate,       "Rotate"       },
    { FrameFxType::Scale,        "Scale"        },
    { FrameFxType::BounceX,      "BounceX"      },
    { FrameFxType::BounceY,      "BounceY"      },
    { FrameFxType::ShakeX,       "ShakeX"       },
    { FrameFxType::ShakeY,       "ShakeY"       },
    { FrameFxType::ColorCycle,   "ColorCycle"   },
    { FrameFxType::ColorPulse,   "ColorPulse"   },
    { FrameFxType::RainbowTrail, "RainbowTrail" },
    { FrameFxType::Spiral,       "Spiral"       },
    { FrameFxType::Col2,         "Col2"         },
    { FrameFxType::Col3,         "Col3"         },
    { FrameFxType::ColFlick,     "ColFlick"     },
    { FrameFxType::Strobe,       "Strobe"       },
})

void to_json(nlohmann::json& j, const GlobalFxEntry& v);
void from_json(const nlohmann::json& j, GlobalFxEntry& v);

void to_json(nlohmann::json& j, const FrameFxEntry& v);
void from_json(const nlohmann::json& j, FrameFxEntry& v);

void to_json(nlohmann::json& j, const FxLayer& v);
void from_json(const nlohmann::json& j, FxLayer& v);

void to_json(nlohmann::json& j, const ChaserStep& v);
void from_json(const nlohmann::json& j, ChaserStep& v);

void to_json(nlohmann::json& j, const FullCueEntry& v);
void from_json(const nlohmann::json& j, FullCueEntry& v);

// ── 3-layer system types ──────────────────────────────────────────────────────

NLOHMANN_JSON_SERIALIZE_ENUM(LaserObjectType, {
    { LaserObjectType::Line,   "Line"   },
    { LaserObjectType::Dot,    "Dot"    },
    { LaserObjectType::Bezier, "Bezier" },
    { LaserObjectType::Arc,    "Arc"    },
    { LaserObjectType::Circle, "Circle" },
    { LaserObjectType::Text,   "Text"   },
})

void to_json(nlohmann::json& j, const GlobalLayer& v);
void from_json(const nlohmann::json& j, GlobalLayer& v);

void to_json(nlohmann::json& j, const LaserObjectPoint& v);
void from_json(const nlohmann::json& j, LaserObjectPoint& v);

void to_json(nlohmann::json& j, const LaserObject& v);
void from_json(const nlohmann::json& j, LaserObject& v);

void to_json(nlohmann::json& j, const KeyframeLayer& v);
void from_json(const nlohmann::json& j, KeyframeLayer& v);

void to_json(nlohmann::json& j, const PlaybackConfig& v);
void from_json(const nlohmann::json& j, PlaybackConfig& v);

void to_json(nlohmann::json& j, const PlaybackDef& v);
void from_json(const nlohmann::json& j, PlaybackDef& v);

// ── Palettes ─────────────────────────────────────────────────────────────────

void to_json(nlohmann::json& j, const ColorPalette::Slot& v);
void from_json(const nlohmann::json& j, ColorPalette::Slot& v);

void to_json(nlohmann::json& j, const ColorPalette& v);
void from_json(const nlohmann::json& j, ColorPalette& v);

void to_json(nlohmann::json& j, const PositionPalette::Slot& v);
void from_json(const nlohmann::json& j, PositionPalette::Slot& v);

void to_json(nlohmann::json& j, const PositionPalette& v);
void from_json(const nlohmann::json& j, PositionPalette& v);

// ── OutputStreamConfig (output patch serialization) ───────────────────────────

NLOHMANN_JSON_SERIALIZE_ENUM(OutputStreamType, {
    { OutputStreamType::Laser, "Laser" },
    { OutputStreamType::NDI,   "NDI"   },
    { OutputStreamType::HDMI,  "HDMI"  },
})

void to_json(nlohmann::json& j, const OutputSafetyConfig::BlockZone& v);
void from_json(const nlohmann::json& j, OutputSafetyConfig::BlockZone& v);

void to_json(nlohmann::json& j, const OutputSafetyConfig& v);
void from_json(const nlohmann::json& j, OutputSafetyConfig& v);

void to_json(nlohmann::json& j, const OutputTransform& v);
void from_json(const nlohmann::json& j, OutputTransform& v);

void to_json(nlohmann::json& j, const OutputStreamConfig& v);
void from_json(const nlohmann::json& j, OutputStreamConfig& v);

// ── Project output groups ─────────────────────────────────────────────────────

void to_json(nlohmann::json& j, const Project::OutputGroup& v);
void from_json(const nlohmann::json& j, Project::OutputGroup& v);

// ── Project safety blackout + color swatches ─────────────────────────────────

void to_json(nlohmann::json& j, const Project::ColorSwatch& v);
void from_json(const nlohmann::json& j, Project::ColorSwatch& v);

void to_json(nlohmann::json& j, const Project::SafetyBlackoutConfig::BlockZone& v);
void from_json(const nlohmann::json& j, Project::SafetyBlackoutConfig::BlockZone& v);

void to_json(nlohmann::json& j, const Project::SafetyBlackoutConfig::BorderCrop& v);
void from_json(const nlohmann::json& j, Project::SafetyBlackoutConfig::BorderCrop& v);

void to_json(nlohmann::json& j, const Project::SafetyBlackoutConfig& v);
void from_json(const nlohmann::json& j, Project::SafetyBlackoutConfig& v);

// ── Timeline system types ─────────────────────────────────────────────────────

NLOHMANN_JSON_SERIALIZE_ENUM(TimelineEventType, {
    { TimelineEventType::CueGo,            "CueGo"            },
    { TimelineEventType::PlaybackGo,       "PlaybackGo"       },
    { TimelineEventType::PlaybackActivate, "PlaybackActivate" },
    { TimelineEventType::PlaybackRelease,  "PlaybackRelease"  },
    { TimelineEventType::SetLevel,         "SetLevel"         },
    { TimelineEventType::Flash,            "Flash"            },
    { TimelineEventType::Command,          "Command"          },
    { TimelineEventType::Marker,           "Marker"           },
    { TimelineEventType::WaitForGo,        "WaitForGo"        },
})

NLOHMANN_JSON_SERIALIZE_ENUM(TimelineState, {
    { TimelineState::Idle,    "Idle"    },
    { TimelineState::Armed,   "Armed"   },
    { TimelineState::Playing, "Playing" },
    { TimelineState::Paused,  "Paused"  },
})

NLOHMANN_JSON_SERIALIZE_ENUM(TimecodeSourceStatus, {
    { TimecodeSourceStatus::Disabled, "Disabled" },
    { TimecodeSourceStatus::Present,  "Present"  },
    { TimecodeSourceStatus::Active,   "Active"   },
})

NLOHMANN_JSON_SERIALIZE_ENUM(SmpteRate, {
    { SmpteRate::Fps24,   "Fps24"   },
    { SmpteRate::Fps25,   "Fps25"   },
    { SmpteRate::Fps2997, "Fps2997" },
    { SmpteRate::Fps30,   "Fps30"   },
})

NLOHMANN_JSON_SERIALIZE_ENUM(TimecodeSource, {
    { TimecodeSource::Internal, "Internal" },
    { TimecodeSource::LTC,      "LTC"      },
    { TimecodeSource::MTC,      "MTC"      },
    { TimecodeSource::ArtNetTC, "ArtNetTC" },
})

void to_json(nlohmann::json& j, const TimelineEvent& v);
void from_json(const nlohmann::json& j, TimelineEvent& v);

void to_json(nlohmann::json& j, const TimelineTrack& v);
void from_json(const nlohmann::json& j, TimelineTrack& v);

void to_json(nlohmann::json& j, const TimelineDef::DmxTrigger& v);
void from_json(const nlohmann::json& j, TimelineDef::DmxTrigger& v);

void to_json(nlohmann::json& j, const TimelineDef& v);
void from_json(const nlohmann::json& j, TimelineDef& v);

void to_json(nlohmann::json& j, const TimecodeSettings& v);
void from_json(const nlohmann::json& j, TimecodeSettings& v);

void to_json(nlohmann::json& j, const ProjectTimecodeConfig& v);
void from_json(const nlohmann::json& j, ProjectTimecodeConfig& v);

// ── Project ───────────────────────────────────────────────────────────────────

void to_json(nlohmann::json& j, const Project& v);
void from_json(const nlohmann::json& j, Project& v);

} // namespace idhmfis
