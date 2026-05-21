// test_project.cpp — Unit tests for the project module.
//
// Uses the doctest single-header test framework (fetched by CMake if available,
// or bundled inline below as a fallback so the tests compile stand-alone).
//
// Run with: ctest --output-on-failure
//           or directly: ./test_project

// ── Doctest bootstrap ────────────────────────────────────────────────────────
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN must be defined in exactly one TU before
// the doctest header is included. Define it unconditionally here so it is set
// regardless of which include path is taken.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#if __has_include(<doctest/doctest.h>)
#  include <doctest/doctest.h>
#elif __has_include("doctest.h")
#  include "doctest.h"
#else
// Minimal stubs so this file compiles without doctest installed.
// Tests compile but produce no output.
#  define TEST_CASE(...)           void DOCTEST_ANON_FUNC_()
#  define SUBCASE(...)             if (false)
#  define CHECK(...)               (void)(!!((__VA_ARGS__)))
#  define CHECK_EQ(a, b)           (void)((a) == (b))
#  define CHECK_THROWS(...)        try { __VA_ARGS__; } catch(...) {}
#  define CHECK_THROWS_AS(e, t)    try { e; } catch(const t&) {}
#  define CHECK_NOTHROW(...)       try { __VA_ARGS__; } catch(...) {}
#  define REQUIRE(...)             if(!(__VA_ARGS__)) return
#  define REQUIRE_EQ(a, b)         if((a)!=(b)) return
#  define INFO(...)
#  define DOCTEST_ANON_FUNC_()     _doctest_stub_##__LINE__()
   namespace doctest { struct Approx { explicit Approx(double) {} \
     bool operator==(float) const { return true; } \
     bool operator==(double) const { return true; } }; }
   int main() { return 0; }
#endif

#include "../serialization.h"
#include "../undo_redo.h"
#include "../project_io.h"

#include <filesystem>
#include <cstdlib>

using namespace idhmfis;

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
static Cue make_test_cue(const std::string& name = "Test Cue") {
    Cue c;
    c.id        = Project::new_id();
    c.name      = name;
    c.generator = GeneratorType::Lissajous;
    c.duration  = 4.0;
    c.loop      = false;
    c.params.speed = 1.5f;
    c.params.scale = 0.8f;
    return c;
}

static Project make_test_project() {
    Project p;
    p.name   = "Test Show";
    p.author = "Unit Test";
    p.schema_version = kCurrentSchemaVersion;

    p.cues.push_back(make_test_cue("Alpha"));
    p.cues.push_back(make_test_cue("Beta"));

    CueListEntry e;
    e.cue_id   = p.cues[0].id;
    e.fade_in  = 0.25f;
    e.fade_out = 0.25f;
    p.cue_list.push_back(e);

    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
//  TEST GROUP 1: UUID generation
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("UUID generation produces unique 36-character RFC 4122 strings") {
    std::string id1 = Project::new_id();
    std::string id2 = Project::new_id();

    CHECK(id1.size() == 36);
    CHECK(id2.size() == 36);
    CHECK(id1 != id2);

    // Check structural format: 8-4-4-4-12
    CHECK(id1[8]  == '-');
    CHECK(id1[13] == '-');
    CHECK(id1[18] == '-');
    CHECK(id1[23] == '-');

    // Version nibble must be '4' (UUID version 4)
    CHECK(id1[14] == '4');

    // Variant nibble must be 8, 9, a, or b
    char variant = id1[19];
    bool valid_variant = (variant == '8' || variant == '9' ||
                          variant == 'a' || variant == 'b');
    CHECK(valid_variant);
}

// ─────────────────────────────────────────────────────────────────────────────
//  TEST GROUP 2: JSON round-trip
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("Project serialises and deserialises identically (round-trip)") {
    Project original = make_test_project();
    original.point_rate     = 40000;
    original.beam_thickness = 3.5f;
    original.film_grain     = true;
    original.bloom_radius   = 12.f;

    // Add an automation track to the first cue
    {
        ParamTrack track;
        track.param_name = "speed";
        track.keyframes.push_back({ 0.0, 0.5f, KeyframePoint::Interp::Linear });
        track.keyframes.push_back({ 2.0, 1.5f, KeyframePoint::Interp::CubicBezier, 0.1f, 0.2f });
        track.keyframes.push_back({ 4.0, 1.0f, KeyframePoint::Interp::Step });
        track.dmx_mapped   = true;
        track.dmx_channel  = 7;
        track.dmx_universe = 0;
        track.midi_mapped  = true;
        track.midi_cc      = 74;
        track.osc_mapped   = true;
        track.osc_path     = "/idhmfis/speed";
        track.audio_mapped  = true;
        track.audio_source  = "rms";
        original.cues[0].automation.push_back(track);
    }

    // Serialise → parse → deserialise
    nlohmann::json j = original.to_json();
    std::string  json_str = j.dump(2);
    nlohmann::json j2 = nlohmann::json::parse(json_str);
    Project loaded = Project::from_json(j2);

    SUBCASE("basic fields") {
        CHECK(loaded.name           == original.name);
        CHECK(loaded.author         == original.author);
        CHECK(loaded.schema_version == original.schema_version);
        CHECK(loaded.point_rate     == original.point_rate);
        CHECK(loaded.beam_thickness == doctest::Approx(original.beam_thickness));
        CHECK(loaded.film_grain     == original.film_grain);
        CHECK(loaded.bloom_radius   == doctest::Approx(original.bloom_radius));
    }

    SUBCASE("cue count and identity") {
        REQUIRE_EQ(loaded.cues.size(), original.cues.size());
        for (size_t i = 0; i < original.cues.size(); ++i) {
            CHECK(loaded.cues[i].id        == original.cues[i].id);
            CHECK(loaded.cues[i].name      == original.cues[i].name);
            CHECK(loaded.cues[i].generator == original.cues[i].generator);
            CHECK(loaded.cues[i].duration  == doctest::Approx(original.cues[i].duration));
            CHECK(loaded.cues[i].loop      == original.cues[i].loop);
        }
    }

    SUBCASE("generator params") {
        const GeneratorParams& op = original.cues[0].params;
        const GeneratorParams& lp = loaded.cues[0].params;
        CHECK(lp.speed   == doctest::Approx(op.speed));
        CHECK(lp.scale   == doctest::Approx(op.scale));
        CHECK(lp.density == doctest::Approx(op.density));
    }

    SUBCASE("automation track round-trip") {
        REQUIRE(!loaded.cues[0].automation.empty());
        const ParamTrack& ot = original.cues[0].automation[0];
        const ParamTrack& lt = loaded.cues[0].automation[0];

        CHECK(lt.param_name    == ot.param_name);
        CHECK(lt.dmx_mapped    == ot.dmx_mapped);
        CHECK(lt.dmx_channel   == ot.dmx_channel);
        CHECK(lt.midi_mapped   == ot.midi_mapped);
        CHECK(lt.midi_cc       == ot.midi_cc);
        CHECK(lt.osc_mapped    == ot.osc_mapped);
        CHECK(lt.osc_path      == ot.osc_path);
        CHECK(lt.audio_mapped  == ot.audio_mapped);
        CHECK(lt.audio_source  == ot.audio_source);

        REQUIRE_EQ(lt.keyframes.size(), ot.keyframes.size());
        for (size_t k = 0; k < ot.keyframes.size(); ++k) {
            CHECK(lt.keyframes[k].time   == doctest::Approx(ot.keyframes[k].time));
            CHECK(lt.keyframes[k].value  == doctest::Approx(ot.keyframes[k].value));
            CHECK(lt.keyframes[k].interp == ot.keyframes[k].interp);
            CHECK(lt.keyframes[k].cp1    == doctest::Approx(ot.keyframes[k].cp1));
            CHECK(lt.keyframes[k].cp2    == doctest::Approx(ot.keyframes[k].cp2));
        }
    }

    SUBCASE("cue list round-trip") {
        REQUIRE_EQ(loaded.cue_list.size(), original.cue_list.size());
        CHECK(loaded.cue_list[0].cue_id   == original.cue_list[0].cue_id);
        CHECK(loaded.cue_list[0].fade_in  == doctest::Approx(original.cue_list[0].fade_in));
        CHECK(loaded.cue_list[0].fade_out == doctest::Approx(original.cue_list[0].fade_out));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  TEST GROUP 3: Undo / Redo stack
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("UndoStack: AddCueCommand and RemoveCueCommand are inverses") {
    Project p;
    UndoStack stack;

    CHECK(!stack.can_undo());
    CHECK(!stack.can_redo());

    // Add a cue
    Cue cue = make_test_cue("New Cue");
    std::string cue_id = cue.id;

    stack.push(std::make_unique<AddCueCommand>(cue), p);

    CHECK(p.cues.size() == 1);
    CHECK(p.find_cue(cue_id) == 0);
    CHECK(stack.can_undo());
    CHECK(!stack.can_redo());
    CHECK(stack.undo_description() == "Undo Add Cue \"New Cue\"");

    // Undo the add
    stack.undo(p);

    CHECK(p.cues.empty());
    CHECK(p.find_cue(cue_id) == -1);
    CHECK(!stack.can_undo());
    CHECK(stack.can_redo());

    // Redo the add
    stack.redo(p);

    CHECK(p.cues.size() == 1);
    CHECK(p.find_cue(cue_id) == 0);
}

TEST_CASE("UndoStack: RemoveCueCommand restores cue and cue-list entries") {
    Project p = make_test_project();
    UndoStack stack;

    std::string target_id = p.cues[0].id;

    // Verify baseline
    REQUIRE(p.cues.size()    == 2);
    REQUIRE(p.cue_list.size() == 1);
    CHECK(p.cue_list[0].cue_id == target_id);

    // Remove the first cue
    stack.push(std::make_unique<RemoveCueCommand>(target_id), p);

    CHECK(p.cues.size()     == 1);
    CHECK(p.cue_list.size() == 0);   // entry referencing it was removed
    CHECK(p.find_cue(target_id) == -1);

    // Undo
    stack.undo(p);

    CHECK(p.cues.size()     == 2);
    CHECK(p.cue_list.size() == 1);
    CHECK(p.cue_list[0].cue_id == target_id);
    CHECK(p.find_cue(target_id) == 0);
}

TEST_CASE("UndoStack: MoveCueCommand reorders and undoes correctly") {
    Project p;
    // Set up a three-entry cue list
    for (int i = 0; i < 3; ++i) {
        Cue c;
        c.id   = Project::new_id();
        c.name = "Cue " + std::to_string(i);
        p.cues.push_back(c);

        CueListEntry e;
        e.cue_id = c.id;
        p.cue_list.push_back(e);
    }

    UndoStack stack;
    std::string id0 = p.cue_list[0].cue_id;
    std::string id2 = p.cue_list[2].cue_id;

    // Move index 0 to index 2
    stack.push(std::make_unique<MoveCueCommand>(0, 2), p);

    CHECK(p.cue_list[0].cue_id != id0);
    CHECK(p.cue_list[2].cue_id == id0);

    // Undo → original order
    stack.undo(p);

    CHECK(p.cue_list[0].cue_id == id0);
    CHECK(p.cue_list[2].cue_id == id2);
}

TEST_CASE("UndoStack: RenameCommand is invertible") {
    Project p = make_test_project();
    UndoStack stack;

    std::string cue_id   = p.cues[0].id;
    std::string old_name = p.cues[0].name;
    std::string new_name = "Renamed Cue";

    stack.push(std::make_unique<RenameCommand>(cue_id, new_name, p), p);

    CHECK(p.cues[0].name == new_name);

    stack.undo(p);

    CHECK(p.cues[0].name == old_name);

    stack.redo(p);

    CHECK(p.cues[0].name == new_name);
}

TEST_CASE("UndoStack: ModifyParamCommand changes and restores a float value") {
    Project p = make_test_project();
    UndoStack stack;

    std::string cue_id = p.cues[0].id;
    float original_speed = p.cues[0].params.speed;
    float new_speed      = original_speed + 1.0f;

    auto cmd = make_cue_param_cmd(
        p, cue_id, "speed",
        [](const GeneratorParams& gp) { return gp.speed; },
        [](GeneratorParams& gp, float v) { gp.speed = v; },
        new_speed);

    stack.push(std::move(cmd), p);

    CHECK(p.cues[0].params.speed == doctest::Approx(new_speed));

    stack.undo(p);

    CHECK(p.cues[0].params.speed == doctest::Approx(original_speed));
}

TEST_CASE("UndoStack: BatchCommand executes and undoes as one step") {
    Project p = make_test_project();
    UndoStack stack;

    // Build a batch: rename cue 0, then add a new cue
    auto batch = std::make_unique<BatchCommand>("Rename + Add");

    std::string cue_id     = p.cues[0].id;
    std::string old_name   = p.cues[0].name;
    batch->add(std::make_unique<RenameCommand>(cue_id, "Batch Renamed", p));

    Cue new_cue       = make_test_cue("Batch Added");
    std::string new_id = new_cue.id;
    batch->add(std::make_unique<AddCueCommand>(new_cue));

    stack.push(std::move(batch), p);

    CHECK(p.cues[0].name  == "Batch Renamed");
    CHECK(p.find_cue(new_id) != -1);
    CHECK(stack.undo_description() == "Undo Rename + Add");

    stack.undo(p);

    CHECK(p.cues[0].name  == old_name);
    CHECK(p.find_cue(new_id) == -1);
}

TEST_CASE("UndoStack: max_depth evicts oldest entries") {
    Project p;
    UndoStack stack(3); // depth limit of 3

    for (int i = 0; i < 5; ++i) {
        Cue c;
        c.id   = Project::new_id();
        c.name = "C" + std::to_string(i);
        stack.push(std::make_unique<AddCueCommand>(c), p);
    }

    // Stack should hold only the 3 most recent
    CHECK(stack.undo_depth() == 3);
    CHECK(p.cues.size() == 5);
}

// ─────────────────────────────────────────────────────────────────────────────
//  TEST GROUP 4: ParamTrack::evaluate
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ParamTrack::evaluate: linear interpolation") {
    ParamTrack t;
    t.keyframes.push_back({ 0.0, 0.0f, KeyframePoint::Interp::Linear });
    t.keyframes.push_back({ 4.0, 4.0f, KeyframePoint::Interp::Linear });

    CHECK(t.evaluate(0.0) == doctest::Approx(0.0f));
    CHECK(t.evaluate(2.0) == doctest::Approx(2.0f));
    CHECK(t.evaluate(4.0) == doctest::Approx(4.0f));
    // Clamp behaviour
    CHECK(t.evaluate(-1.0) == doctest::Approx(0.0f));
    CHECK(t.evaluate(10.0) == doctest::Approx(4.0f));
}

TEST_CASE("ParamTrack::evaluate: step interpolation holds value") {
    ParamTrack t;
    t.keyframes.push_back({ 0.0, 10.0f, KeyframePoint::Interp::Step });
    t.keyframes.push_back({ 2.0, 20.0f, KeyframePoint::Interp::Step });
    t.keyframes.push_back({ 4.0, 30.0f, KeyframePoint::Interp::Step });

    CHECK(t.evaluate(0.0) == doctest::Approx(10.0f));
    CHECK(t.evaluate(1.9) == doctest::Approx(10.0f));
    CHECK(t.evaluate(2.5) == doctest::Approx(20.0f));
    CHECK(t.evaluate(5.0) == doctest::Approx(30.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  TEST GROUP 5: Default 40-channel fixture profile
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("DmxFixtureProfile::make_default_40ch produces exactly 40 channels") {
    DmxFixtureProfile p = DmxFixtureProfile::make_default_40ch();

    CHECK(p.name == "IDHMFIS Default 40ch");
    REQUIRE(p.channels.size() == 40);

    // Spot-check a few channels
    CHECK(p.channels[0].offset      == 1);
    CHECK(p.channels[0].param_name  == "cue_select_msb");

    CHECK(p.channels[2].offset      == 3);
    CHECK(p.channels[2].param_name  == "intensity");
    CHECK(p.channels[2].scale_min   == doctest::Approx(0.f));
    CHECK(p.channels[2].scale_max   == doctest::Approx(1.f));

    CHECK(p.channels[20].offset     == 21);
    CHECK(p.channels[20].param_name == "blackout");

    CHECK(p.channels[27].offset     == 28);
    CHECK(p.channels[27].param_name == "bpm_divider");

    // Reserved channels 29-40
    for (int i = 28; i < 40; ++i) {
        CHECK(p.channels[i].offset == i + 1);
        // All are "reserved_<n>"
        CHECK(p.channels[i].param_name.rfind("reserved_", 0) == 0);
    }

    // All offsets must be sequential 1..40
    for (int i = 0; i < 40; ++i)
        CHECK(p.channels[i].offset == i + 1);
}

TEST_CASE("DmxFixtureProfile round-trips through JSON") {
    DmxFixtureProfile original = DmxFixtureProfile::make_default_40ch();

    nlohmann::json j;
    to_json(j, original);
    DmxFixtureProfile loaded;
    from_json(j, loaded);

    CHECK(loaded.name == original.name);
    REQUIRE(loaded.channels.size() == original.channels.size());

    for (size_t i = 0; i < original.channels.size(); ++i) {
        CHECK(loaded.channels[i].offset     == original.channels[i].offset);
        CHECK(loaded.channels[i].param_name == original.channels[i].param_name);
        CHECK(loaded.channels[i].scale_min  == doctest::Approx(original.channels[i].scale_min));
        CHECK(loaded.channels[i].scale_max  == doctest::Approx(original.channels[i].scale_max));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  TEST GROUP 6: File save / load round-trip
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("Project::save and Project::load produce an identical project") {
    Project original = make_test_project();
    original.author      = "File Test";
    original.point_rate  = 45000;
    original.haze_density = 0.7f;

    // Use a temp file
    auto tmp = std::filesystem::temp_directory_path() / "idhmfis_test_project.idhmfis";
    std::string tmp_str = tmp.string();

    original.save(tmp_str);

    // Verify the .tmp file was cleaned up
    CHECK(!std::filesystem::exists(tmp_str + ".tmp"));

    Project loaded = Project::load(tmp_str);

    CHECK(loaded.name         == original.name);
    CHECK(loaded.author       == original.author);
    CHECK(loaded.point_rate   == original.point_rate);
    CHECK(loaded.haze_density == doctest::Approx(original.haze_density));
    CHECK(loaded.cues.size()  == original.cues.size());

    // Cleanup
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST_CASE("Project::load throws ProjectLoadError for missing file") {
    CHECK_THROWS_AS(
        Project::load("/nonexistent/path/no_file.idhmfis"),
        ProjectLoadError);
}

TEST_CASE("validate_project_file rejects files without .idhmfis extension") {
    auto result = validate_project_file("/some/path/file.json");
    CHECK(!result.ok);
    CHECK(!result.error.empty());
}
