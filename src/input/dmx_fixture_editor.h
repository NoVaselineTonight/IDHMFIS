#pragma once
// DMX fixture profile editor — data model only.
// The ImGui-based UI lives in src/ui/; this file defines the
// edit operations and serialization contract.
//
// Design notes:
//   - DmxFixtureEditor wraps a DmxFixtureProfile by reference.
//     The profile is owned by the caller (usually a DmxPatch inside a Project).
//   - All mutations go through the editor's methods so that the UI can easily
//     bind to them and call undo/redo without knowing about the internals.
//   - export_to_json / import_from_json use the nlohmann::json schema defined
//     in serialization.cpp.
//   - import_from_gdtf() parses a subset of GDTF 1.0 XML:
//       FixtureType/DMXModes/DMXMode/DMXChannels/DMXChannel
//     It is a best-effort importer; unsupported features are silently skipped.
//
// JSON schema for a DmxFixtureProfile:
// {
//   "schema": "idhmfis-fixture-1.0",
//   "name": "My Fixture",
//   "channels": [
//     {
//       "number": 1,
//       "name": "Intensity",
//       "param_name": "intensity",
//       "scale_min": 0.0,
//       "scale_max": 1.0,
//       "is_16bit": false,
//       "notes": ""
//     },
//     ...
//   ]
// }

#include <cstdint>
#include <string>
#include <vector>

// Require nlohmann/json to be available before including this header when
// using serialisation methods.
#ifdef NLOHMANN_JSON_VERSION_MAJOR
#  include <nlohmann/json.hpp>
#  define IDHMFIS_FIXTURE_JSON_AVAILABLE 1
#endif

#include "../project/project.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  FixtureChannel — richer per-channel definition used by the editor
//  The simpler DmxFixtureProfile::ChannelDef is used at runtime;
//  FixtureChannel carries the extra metadata only needed at edit time.
// ─────────────────────────────────────────────────────────────────────────────
struct FixtureChannel {
    int         number     = 1;         // 1-based channel number within the profile
    std::string name;                   // Human-readable label e.g. "Intensity"
    std::string param_name;             // maps to GeneratorParams field name
    float       scale_min  = 0.f;       // DMX value 0 maps to this param value
    float       scale_max  = 1.f;       // DMX value 255 maps to this param value
    bool        is_16bit   = false;     // if true, this + next channel = 16-bit pair (MSB first)
    std::string notes;                  // free-text annotation for the fixture designer

    // Convert to the runtime ChannelDef used by DmxFixtureProfile
    DmxFixtureProfile::ChannelDef to_channel_def() const {
        return { number, param_name, scale_min, scale_max };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  DmxFixtureEditor
// ─────────────────────────────────────────────────────────────────────────────
class DmxFixtureEditor {
public:
    // Wrap an existing profile (must outlive this editor)
    explicit DmxFixtureEditor(DmxFixtureProfile& profile);

    // Non-copyable
    DmxFixtureEditor(const DmxFixtureEditor&)            = delete;
    DmxFixtureEditor& operator=(const DmxFixtureEditor&) = delete;

    // ── Channel CRUD ──────────────────────────────────────────────────────

    // Set (or add) a channel definition.
    // Channel numbers are 1-512; out-of-range numbers are silently ignored.
    void set_channel(int ch, const FixtureChannel& def);

    // Remove a channel definition; silently no-ops if not present
    void remove_channel(int ch);

    // Retrieve a channel; returns an empty FixtureChannel{} if not found
    FixtureChannel get_channel(int ch) const;

    // Returns a sorted list of all channel numbers that have definitions
    std::vector<int> channel_numbers() const;

    // Total number of channels defined
    int channel_count() const { return static_cast<int>(channels_.size()); }

    // ── Profile management ───────────────────────────────────────────────

    // Replace everything with the canonical 40-channel default profile.
    // Propagates changes back to the wrapped DmxFixtureProfile.
    void reset_to_default();

    // Commit editor state to the wrapped DmxFixtureProfile
    // (called automatically by all mutating methods)
    void commit();

    // ── Serialisation ────────────────────────────────────────────────────

#ifdef IDHMFIS_FIXTURE_JSON_AVAILABLE
    // Export the current editor state as JSON.
    // Returns a complete JSON object following the schema above.
    nlohmann::json export_to_json() const;

    // Import from a JSON object.  Returns true on success.
    // Overwrites all channels; does not merge.
    // Validates the schema field and returns false on mismatch.
    bool import_from_json(const nlohmann::json& j);
#endif

    // Import from a GDTF 1.0 XML file (best-effort; stub for fields not yet needed).
    // Returns true if at least one channel was successfully parsed.
    // Does not overwrite existing channels on failure.
    bool import_from_gdtf(const std::string& path);

    // ── Profile name ─────────────────────────────────────────────────────
    const std::string& name() const { return profile_.name; }
    void set_name(const std::string& n) { profile_.name = n; }

private:
    DmxFixtureProfile&         profile_;
    std::vector<FixtureChannel> channels_;  // editor-side data (may differ from profile_ until commit)

    // Sync channels_ from the wrapped profile (called on construction)
    void load_from_profile();
};

} // namespace idhmfis
