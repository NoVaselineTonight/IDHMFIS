// dmx_fixture_editor.cpp — DMX fixture profile editor implementation
//
// JSON serialisation schema: "idhmfis-fixture-1.0"
//
// GDTF (General Device Type Format) parsing:
//   GDTF 1.0 is a ZIP archive containing "description.xml".
//   Since we don't have a ZIP library dependency available, the GDTF importer
//   assumes the caller has already extracted description.xml and passes its path.
//   It parses:
//     FixtureType/DMXModes/DMXMode[first]/DMXChannels/DMXChannel
//   attributes: DMXBreak (mapped to channel number), Name (mapped to channel name).
//   Logical channels and channel functions are not fully parsed — only the first
//   ChannelFunction is used to extract the attribute name.

#include "dmx_fixture_editor.h"
#include "../core/logger.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

// Simple XML tag/attribute parser (no external dependency)
namespace {

// Find value of attribute `attr` in XML element starting at `s`.
// Returns empty string if not found.
std::string xml_attr(const std::string& s, const std::string& attr) {
    std::string key = attr + "=\"";
    auto pos = s.find(key);
    if (pos == std::string::npos) {
        key = attr + "='";
        pos = s.find(key);
        if (pos == std::string::npos) return {};
    }
    pos += key.size();
    char end_char = (key.back() == '"') ? '"' : '\'';
    auto end = s.find(end_char, pos);
    if (end == std::string::npos) return {};
    return s.substr(pos, end - pos);
}

// Extract all occurrences of a named XML element (opening tag only).
std::vector<std::string> xml_elements(const std::string& doc,
                                      const std::string& tag) {
    std::vector<std::string> result;
    std::string open = "<" + tag;
    size_t pos = 0;
    while ((pos = doc.find(open, pos)) != std::string::npos) {
        size_t end = doc.find('>', pos);
        if (end == std::string::npos) break;
        result.push_back(doc.substr(pos, end - pos + 1));
        pos = end + 1;
    }
    return result;
}

} // anonymous namespace

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────────────
DmxFixtureEditor::DmxFixtureEditor(DmxFixtureProfile& profile)
    : profile_(profile) {
    load_from_profile();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Load from wrapped profile
// ─────────────────────────────────────────────────────────────────────────────
void DmxFixtureEditor::load_from_profile() {
    channels_.clear();
    channels_.reserve(profile_.channels.size());

    for (const auto& cd : profile_.channels) {
        FixtureChannel fc{};
        fc.number     = cd.offset;
        fc.param_name = cd.param_name;
        fc.scale_min  = cd.scale_min;
        fc.scale_max  = cd.scale_max;
        fc.name       = cd.param_name;  // best-effort: use param_name as display name
        fc.is_16bit   = (cd.param_name.size() >= 4 &&
                         cd.param_name.substr(cd.param_name.size() - 4) == "_msb");
        channels_.push_back(fc);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Commit editor state → wrapped profile
// ─────────────────────────────────────────────────────────────────────────────
void DmxFixtureEditor::commit() {
    profile_.channels.clear();
    profile_.channels.reserve(channels_.size());

    for (const auto& fc : channels_) {
        profile_.channels.push_back(fc.to_channel_def());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Channel CRUD
// ─────────────────────────────────────────────────────────────────────────────
void DmxFixtureEditor::set_channel(int ch, const FixtureChannel& def) {
    if (ch < 1 || ch > 512) return;

    // Find existing or insert
    for (auto& fc : channels_) {
        if (fc.number == ch) {
            fc = def;
            fc.number = ch;  // ensure number matches the key
            commit();
            return;
        }
    }

    // Not found — append then sort
    FixtureChannel copy = def;
    copy.number = ch;
    channels_.push_back(copy);
    std::sort(channels_.begin(), channels_.end(),
              [](const FixtureChannel& a, const FixtureChannel& b) {
                  return a.number < b.number;
              });
    commit();
}

void DmxFixtureEditor::remove_channel(int ch) {
    auto it = std::remove_if(channels_.begin(), channels_.end(),
                             [ch](const FixtureChannel& fc) {
                                 return fc.number == ch;
                             });
    if (it != channels_.end()) {
        channels_.erase(it, channels_.end());
        commit();
    }
}

FixtureChannel DmxFixtureEditor::get_channel(int ch) const {
    for (const auto& fc : channels_) {
        if (fc.number == ch) return fc;
    }
    return FixtureChannel{};
}

std::vector<int> DmxFixtureEditor::channel_numbers() const {
    std::vector<int> nums;
    nums.reserve(channels_.size());
    for (const auto& fc : channels_) nums.push_back(fc.number);
    return nums;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Reset to default
// ─────────────────────────────────────────────────────────────────────────────
void DmxFixtureEditor::reset_to_default() {
    profile_ = DmxFixtureProfile::make_default_40ch();
    load_from_profile();
    // profile_ has already been updated by make_default_40ch(); no further commit needed
}

// ─────────────────────────────────────────────────────────────────────────────
//  JSON serialisation
// ─────────────────────────────────────────────────────────────────────────────
#ifdef IDHMFIS_FIXTURE_JSON_AVAILABLE

nlohmann::json DmxFixtureEditor::export_to_json() const {
    nlohmann::json j;
    j["schema"] = "idhmfis-fixture-1.0";
    j["name"]   = profile_.name;

    auto arr = nlohmann::json::array();
    for (const auto& fc : channels_) {
        nlohmann::json ch_j;
        ch_j["number"]     = fc.number;
        ch_j["name"]       = fc.name;
        ch_j["param_name"] = fc.param_name;
        ch_j["scale_min"]  = fc.scale_min;
        ch_j["scale_max"]  = fc.scale_max;
        ch_j["is_16bit"]   = fc.is_16bit;
        ch_j["notes"]      = fc.notes;
        arr.push_back(std::move(ch_j));
    }
    j["channels"] = std::move(arr);
    return j;
}

bool DmxFixtureEditor::import_from_json(const nlohmann::json& j) {
    try {
        // Schema validation
        if (!j.contains("schema") ||
            j["schema"].get<std::string>() != "idhmfis-fixture-1.0") {
            log::error("DmxFixtureEditor: unknown schema '%s'",
                       j.value("schema", "<missing>").c_str());
            return false;
        }

        // Name
        if (j.contains("name"))
            profile_.name = j["name"].get<std::string>();

        // Channels
        if (!j.contains("channels") || !j["channels"].is_array()) {
            log::error("DmxFixtureEditor: JSON missing 'channels' array");
            return false;
        }

        std::vector<FixtureChannel> new_channels;
        for (const auto& ch_j : j["channels"]) {
            FixtureChannel fc{};
            fc.number     = ch_j.value("number",     1);
            fc.name       = ch_j.value("name",       "");
            fc.param_name = ch_j.value("param_name", "");
            fc.scale_min  = ch_j.value("scale_min",  0.f);
            fc.scale_max  = ch_j.value("scale_max",  1.f);
            fc.is_16bit   = ch_j.value("is_16bit",   false);
            fc.notes      = ch_j.value("notes",      "");

            if (fc.number < 1 || fc.number > 512) {
                log::warn("DmxFixtureEditor: channel number %d out of range, skipping",
                          fc.number);
                continue;
            }
            new_channels.push_back(fc);
        }

        // Sort by channel number
        std::sort(new_channels.begin(), new_channels.end(),
                  [](const FixtureChannel& a, const FixtureChannel& b) {
                      return a.number < b.number;
                  });

        channels_ = std::move(new_channels);
        commit();
        log::info("DmxFixtureEditor: imported %d channels from JSON",
                  static_cast<int>(channels_.size()));
        return true;

    } catch (const nlohmann::json::exception& e) {
        log::error("DmxFixtureEditor: JSON parse error: %s", e.what());
        return false;
    }
}

#endif // IDHMFIS_FIXTURE_JSON_AVAILABLE

// ─────────────────────────────────────────────────────────────────────────────
//  GDTF importer (best-effort, description.xml path)
//
//  Parses the first DMXMode element and maps DMXChannels to FixtureChannels.
//  Expected XML structure (GDTF DIN SPEC 15800):
//
//    <FixtureType>
//      <DMXModes>
//        <DMXMode Name="...">
//          <DMXChannels>
//            <DMXChannel DMXBreak="1" Offset="1" ...>
//              <LogicalChannel Attribute="Dimmer" ...>
//                <ChannelFunction .../>
//              </LogicalChannel>
//            </DMXChannel>
//          </DMXChannels>
//        </DMXMode>
//      </DMXModes>
//    </FixtureType>
//
//  This parser handles only a subset; complex multi-break and virtual channels
//  are skipped. The Attribute field is mapped to a param_name using a simple
//  table.
// ─────────────────────────────────────────────────────────────────────────────
bool DmxFixtureEditor::import_from_gdtf(const std::string& path) {
    // GDTF attribute → param_name mapping table
    static const struct { const char* gdtf_attr; const char* param_name; } kAttrMap[] = {
        { "Dimmer",          "intensity"    },
        { "Pan",             "pan_msb"      },
        { "Tilt",            "tilt_msb"     },
        { "Zoom",            "zoom"         },
        { "Red",             "color_r"      },
        { "Green",           "color_g"      },
        { "Blue",            "color_b"      },
        { "White",           "color_w"      },
        { "ColorTemperature","param_a"      },
        { "Gobo1",           "param_b"      },
        { "Gobo2",           "param_c"      },
        { "Shutter",         "blackout"     },
        { "Strobe",          "strobe"       },
        { "Speed",           "scan_speed"   },
        { "Rotation",        "rotation_msb" },
        { nullptr, nullptr }
    };

    auto lookup_param = [&](const std::string& gdtf_attr) -> std::string {
        for (int i = 0; kAttrMap[i].gdtf_attr; ++i) {
            if (gdtf_attr == kAttrMap[i].gdtf_attr)
                return kAttrMap[i].param_name;
        }
        // Unknown attribute: use lower-cased gdtf_attr as a best guess
        std::string s = gdtf_attr;
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    // Read file
    std::ifstream f(path);
    if (!f.is_open()) {
        log::error("DmxFixtureEditor: cannot open GDTF description file: %s", path.c_str());
        return false;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string doc = ss.str();

    if (doc.empty()) {
        log::error("DmxFixtureEditor: GDTF file is empty: %s", path.c_str());
        return false;
    }

    // Extract FixtureType name
    auto ft_elems = xml_elements(doc, "FixtureType");
    if (!ft_elems.empty()) {
        std::string ft_name = xml_attr(ft_elems[0], "Name");
        if (!ft_name.empty()) profile_.name = ft_name;
    }

    // Find the first DMXMode section
    auto mode_pos = doc.find("<DMXMode");
    if (mode_pos == std::string::npos) {
        log::warn("DmxFixtureEditor: no DMXMode found in %s", path.c_str());
        return false;
    }

    auto mode_end = doc.find("</DMXMode>", mode_pos);
    if (mode_end == std::string::npos) mode_end = doc.size();
    std::string mode_section = doc.substr(mode_pos, mode_end - mode_pos);

    // Parse all DMXChannel elements in this mode
    auto channel_elems = xml_elements(mode_section, "DMXChannel");
    if (channel_elems.empty()) {
        log::warn("DmxFixtureEditor: no DMXChannel elements in mode section");
        return false;
    }

    std::vector<FixtureChannel> new_channels;
    int channel_idx = 1;

    for (const auto& ch_elem : channel_elems) {
        // Get offset (may be "1" or "1,2" for 16-bit)
        std::string offset_str = xml_attr(ch_elem, "Offset");
        if (offset_str.empty()) {
            // Use sequential numbering
            offset_str = std::to_string(channel_idx);
        }

        // Parse primary offset (before any comma)
        int ch_number = channel_idx;
        try {
            ch_number = std::stoi(offset_str);
        } catch (...) {
            ch_number = channel_idx;
        }

        bool is_16bit = (offset_str.find(',') != std::string::npos);

        // Find the LogicalChannel Attribute in the rest of the document
        // We search for the first LogicalChannel after this DMXChannel tag
        auto lc_pos = mode_section.find("<LogicalChannel", mode_section.find(ch_elem));
        std::string attr_name;
        if (lc_pos != std::string::npos) {
            auto lc_end = mode_section.find('>', lc_pos);
            if (lc_end != std::string::npos) {
                std::string lc_elem = mode_section.substr(lc_pos, lc_end - lc_pos + 1);
                attr_name = xml_attr(lc_elem, "Attribute");
            }
        }

        // Get channel name from DMXChannel Name attribute
        std::string ch_name = xml_attr(ch_elem, "Name");
        if (ch_name.empty()) ch_name = attr_name;

        FixtureChannel fc{};
        fc.number     = ch_number;
        fc.name       = ch_name;
        fc.param_name = lookup_param(attr_name);
        fc.scale_min  = 0.f;
        fc.scale_max  = 1.f;
        fc.is_16bit   = is_16bit;
        fc.notes      = "Imported from GDTF";

        if (fc.number >= 1 && fc.number <= 512) {
            new_channels.push_back(fc);
        }
        ++channel_idx;
    }

    if (new_channels.empty()) {
        log::warn("DmxFixtureEditor: GDTF import produced no valid channels from %s", path.c_str());
        return false;
    }

    std::sort(new_channels.begin(), new_channels.end(),
              [](const FixtureChannel& a, const FixtureChannel& b) {
                  return a.number < b.number;
              });

    channels_ = std::move(new_channels);
    commit();
    log::info("DmxFixtureEditor: GDTF import: %d channels from %s",
              static_cast<int>(channels_.size()), path.c_str());
    return true;
}

} // namespace idhmfis
