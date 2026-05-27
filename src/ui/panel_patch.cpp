// panel_patch.cpp — Multi-output patch view.
// Left column: table of all patched outputs (laser/NDI/HDMI).
// Right column: properties for the selected output.
// Toolbar: ADD SINGLE / ADD MULTIPLE type-selection modals.

#include "layout.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "theme.h"
#include "../core/types.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static const char* stream_type_label(OutputStreamType t) {
    switch (t) {
    case OutputStreamType::Laser: return "LASER";
    case OutputStreamType::NDI:   return "NDI";
    case OutputStreamType::HDMI:  return "HDMI";
    }
    return "???";
}

static ImVec4 stream_type_color(OutputStreamType t) {
    switch (t) {
    case OutputStreamType::Laser: return ImVec4(0.0f, 1.0f, 0.45f, 1.0f);
    case OutputStreamType::NDI:   return ImVec4(0.0f, 0.9f, 1.0f,  1.0f);
    case OutputStreamType::HDMI:  return ImVec4(1.0f, 0.7f, 0.0f,  1.0f);
    }
    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
}

// Collect current UIState.patched_outputs back into an OutputStreamConfig vector
// so we can fire on_output_patch_changed with the full list.
static std::vector<OutputStreamConfig> collect_configs(const UIState& state) {
    std::vector<OutputStreamConfig> out;
    out.reserve(state.patched_outputs.size());
    for (const auto& p : state.patched_outputs)
        out.push_back(p.config);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Per-output safety zone editor — shared between per-stream and global views.
//  Returns true if anything changed.
// ─────────────────────────────────────────────────────────────────────────────
static bool draw_safety_zone_editor(OutputSafetyConfig& saf, int uid_base) {
    bool changed = false;

    ImGui::PushID(uid_base);

    // Master enable
    ImGui::PushStyleColor(ImGuiCol_Text,
        saf.enabled ? ImVec4(0.0f, 1.0f, 0.45f, 1.0f) : ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Checkbox("Enable per-output safety##saf_en", &saf.enabled)) changed = true;
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip(
        "Enable per-output safety blackout for this stream.\n"
        "Applied after the global blackout, independently per output.");

    if (!saf.enabled) {
        ImGui::PopID();
        return changed;
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Border Crops##saf_bdr")) {
        ImGui::Indent(8.0f);
        ImGui::TextDisabled("Crop each edge inward (0 = no crop, 1.0 = full field).");
        ImGui::Spacing();
        ImGui::Text("Left:");   ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat("##saf_l",   &saf.border_left,   0.0f, 1.0f, "%.3f")) changed = true;
        ImGui::Text("Right:");  ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat("##saf_r",   &saf.border_right,  0.0f, 1.0f, "%.3f")) changed = true;
        ImGui::Text("Top:");    ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat("##saf_t",   &saf.border_top,    0.0f, 1.0f, "%.3f")) changed = true;
        ImGui::Text("Bottom:"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat("##saf_b",   &saf.border_bottom, 0.0f, 1.0f, "%.3f")) changed = true;
        ImGui::Text("Tilt:");   ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat("##saf_tilt",&saf.border_tilt,  -45.0f, 45.0f, "%.1f deg")) changed = true;
        ImGui::SetItemTooltip("Rotate the crop boundary for tilted projection surfaces.");
        ImGui::Unindent(8.0f);
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Block Zones##saf_blk")) {
        ImGui::Indent(8.0f);

        if (ImGui::Button("Add Block Zone##saf_add")) {
            OutputSafetyConfig::BlockZone z;
            z.name = "Zone " + std::to_string(static_cast<int>(saf.zones.size()) + 1);
            saf.zones.push_back(z);
            changed = true;
        }
        ImGui::SetItemTooltip("Add a rectangular block zone for this output.");

        ImGui::Spacing();

        int rm_idx = -1;
        for (int zi = 0; zi < static_cast<int>(saf.zones.size()); ++zi) {
            auto& z = saf.zones[static_cast<size_t>(zi)];
            ImGui::PushID(zi + 1000);

            if (ImGui::Checkbox("##saf_zen", &z.enabled)) changed = true;
            ImGui::SameLine();
            {
                char nbuf[64];
                std::strncpy(nbuf, z.name.c_str(), sizeof(nbuf) - 1);
                nbuf[sizeof(nbuf) - 1] = '\0';
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::InputText("##saf_znm", nbuf, sizeof(nbuf))) {
                    z.name = nbuf;
                    changed = true;
                }
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.4f, 0.05f, 0.05f, 1.0f));
            if (ImGui::Button("Remove##saf_zrm")) rm_idx = zi;
            ImGui::PopStyleColor(3);

            if (rm_idx != zi) {
                ImGui::Indent(20.0f);
                ImGui::Text("CX:"); ImGui::SameLine(50.0f); ImGui::SetNextItemWidth(150.0f);
                if (ImGui::SliderFloat("##saf_cx",  &z.cx,        -1.0f, 1.0f,   "%.3f")) changed = true;
                ImGui::Text("CY:"); ImGui::SameLine(50.0f); ImGui::SetNextItemWidth(150.0f);
                if (ImGui::SliderFloat("##saf_cy",  &z.cy,        -1.0f, 1.0f,   "%.3f")) changed = true;
                ImGui::Text("W:");  ImGui::SameLine(50.0f); ImGui::SetNextItemWidth(150.0f);
                if (ImGui::SliderFloat("##saf_hw",  &z.hw,         0.0f, 1.0f,  "hw=%.3f")) changed = true;
                ImGui::SetItemTooltip("Half-width (full width = 2 * this value).");
                ImGui::Text("H:");  ImGui::SameLine(50.0f); ImGui::SetNextItemWidth(150.0f);
                if (ImGui::SliderFloat("##saf_hh",  &z.hh,         0.0f, 1.0f,  "hh=%.3f")) changed = true;
                ImGui::SetItemTooltip("Half-height (full height = 2 * this value).");
                ImGui::Text("Ang:"); ImGui::SameLine(50.0f); ImGui::SetNextItemWidth(150.0f);
                if (ImGui::SliderFloat("##saf_ang", &z.angle_deg, -180.0f, 180.0f, "%.1f deg")) changed = true;
                ImGui::Unindent(20.0f);
            }

            ImGui::Separator();
            ImGui::PopID();
        }

        if (rm_idx >= 0) {
            saf.zones.erase(saf.zones.begin() + rm_idx);
            changed = true;
        }

        ImGui::Unindent(8.0f);
    }

    ImGui::PopID();
    return changed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Right panel — properties for the selected output
// ─────────────────────────────────────────────────────────────────────────────
static void draw_output_properties(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    // Find selected output
    UIState::PatchedOutput* sel = nullptr;
    for (auto& p : state.patched_outputs) {
        if (p.id == ctx.patch_selected_id) { sel = &p; break; }
    }

    if (!sel) {
        ImGui::Spacing();
        ImGui::TextDisabled("Select an output on the left to edit its properties.");
        return;
    }

    OutputStreamConfig& cfg = sel->config;
    bool patch_changed = false;

    // ── Output name ───────────────────────────────────────────────────────────
    ImGui::SeparatorText("Output Identity");

    char namebuf[128];
    std::strncpy(namebuf, cfg.name.c_str(), sizeof(namebuf) - 1);
    namebuf[sizeof(namebuf) - 1] = '\0';
    ImGui::Text("Name:"); ImGui::SameLine(80.0f); ImGui::SetNextItemWidth(240.0f);
    if (ImGui::InputText("##out_name", namebuf, sizeof(namebuf))) {
        cfg.name = namebuf;
        sel->name = namebuf;
        patch_changed = true;
    }

    ImGui::SameLine(340.0f);
    bool en = cfg.enabled;
    if (ImGui::Checkbox("Enabled##out_en", &en)) {
        cfg.enabled = en;
        sel->enabled = en;
        patch_changed = true;
    }

    ImGui::Spacing();

    // ── Target for programming toggle ─────────────────────────────────────────
    {
        bool in_sel = false;
        for (int sid : state.selected_output_ids)
            if (sid == sel->id) { in_sel = true; break; }

        ImGui::PushStyleColor(ImGuiCol_Text,
            in_sel ? ImVec4(1.0f, 0.85f, 0.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        bool tgt = in_sel;
        if (ImGui::Checkbox("Target for programming##out_tgt", &tgt)) {
            if (tgt) {
                bool already = false;
                for (int sid : state.selected_output_ids)
                    if (sid == sel->id) { already = true; break; }
                if (!already)
                    state.selected_output_ids.push_back(sel->id);
            } else {
                auto& sv = state.selected_output_ids;
                sv.erase(std::remove(sv.begin(), sv.end(), sel->id), sv.end());
            }
            sel->selected_for_programming = tgt;
            state.broadcast_to_all = false;
            std::vector<int> old_ids_tgt = state.active_stream_ids;
            state.active_stream_ids = state.selected_output_ids;
            if (cbs.on_active_streams_changed)
                cbs.on_active_streams_changed(old_ids_tgt, state.active_stream_ids);
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(
            "ON (gold): programmer content (frame editor) targets this output.\n"
            "OFF: output still plays back cues normally, but not targeted by programmer.\n"
            "Use Broadcast to All in the list to send programmer to every output.");
    }

    ImGui::Spacing();

    // ── Type-specific settings ─────────────────────────────────────────────────
    if (cfg.type == OutputStreamType::Laser) {
        ImGui::SeparatorText("Laser / DAC Settings");
        ImGui::Spacing();

        // DAC type selector
        static const char* kDacTypes[] = { "auto", "etherdream", "idn", "helios", "riya", "ilda_usb" };
        int dac_idx = 0;
        for (int i = 0; i < static_cast<int>(std::size(kDacTypes)); ++i) {
            if (cfg.dac_type == kDacTypes[i]) { dac_idx = i; break; }
        }
        ImGui::Text("DAC Type:"); ImGui::SameLine(100.0f); ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("##dac_type", &dac_idx, kDacTypes, static_cast<int>(std::size(kDacTypes)))) {
            cfg.dac_type = kDacTypes[dac_idx];
            patch_changed = true;
        }
        ImGui::SetItemTooltip(
            "\"auto\" — scan and use the first DAC found.\n"
            "\"etherdream\" — EtherDream / ILDA over Ethernet.\n"
            "\"idn\" — ILDA Digital Network (UDP 7255).\n"
            "\"helios\" — Helios USB DAC.\n"
            "\"riya\" — Riya USB DAC.\n"
            "\"ilda_usb\" — Generic ILDA USB DAC.");

        // DAC address (IP for etherdream/idn, serial for USB)
        if (cfg.dac_type == "etherdream" || cfg.dac_type == "idn") {
            char addrbuf[64];
            std::strncpy(addrbuf, cfg.dac_address.c_str(), sizeof(addrbuf) - 1);
            addrbuf[sizeof(addrbuf) - 1] = '\0';
            ImGui::Text("IP Address:"); ImGui::SameLine(100.0f); ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputText("##dac_addr", addrbuf, sizeof(addrbuf))) {
                cfg.dac_address = addrbuf;
                patch_changed = true;
            }
            ImGui::SetItemTooltip("IP address of the DAC hardware. Leave empty to use broadcast discovery.");
        }

        // Point rate
        ImGui::Text("Point Rate:"); ImGui::SameLine(100.0f); ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderInt("##out_pps", &cfg.point_rate, kMinPointRate, kMaxPointRate, "%d pps")) {
            patch_changed = true;
        }
        ImGui::SetItemTooltip(
            "Target output scan rate in points per second.\n"
            "Higher = smoother shapes but may exceed DAC hardware limits.\n"
            "Recommended: 30000–40000 pps for most hardware.");

        // Available DACs (informational list from engine)
        if (!state.available_dacs.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Detected DACs:");
            for (const auto& dac : state.available_dacs) {
                ImGui::BulletText("%s", dac.name.c_str());
            }
        }

        // CITP/CAEX stream name — tells the operator which Capture source name
        // corresponds to this output so they can patch it in the visualiser.
        ImGui::Spacing();
        ImGui::SeparatorText("CITP/CAEX (Capture 2024)");
        if (!sel->citp_stream_name.empty()) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.45f, 1.0f),
                "Source: \"%s\"", sel->citp_stream_name.c_str());
            ImGui::SetItemTooltip(
                "This output broadcasts as \"%s\" in Capture 2024.\n"
                "Look for this name in Capture's Patch panel under CITP sources.",
                sel->citp_stream_name.c_str());
        } else {
            ImGui::TextDisabled("CITP source: (starting...)");
        }

    } else if (cfg.type == OutputStreamType::NDI) {
        ImGui::SeparatorText("NDI Stream Settings");
        ImGui::Spacing();

        char ndi_name_buf[128];
        std::strncpy(ndi_name_buf, cfg.ndi_name.c_str(), sizeof(ndi_name_buf) - 1);
        ndi_name_buf[sizeof(ndi_name_buf) - 1] = '\0';
        ImGui::Text("Source Name:"); ImGui::SameLine(110.0f); ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputText("##ndi_sname", ndi_name_buf, sizeof(ndi_name_buf))) {
            cfg.ndi_name = ndi_name_buf;
            patch_changed = true;
        }
        ImGui::SetItemTooltip("NDI source name visible to receivers on the LAN.");

        // Resolution
        static const char* kResLabels[] = {
            "1920x1080", "1280x720", "854x480", "3840x2160",
            "800x600",   "640x480",  "512x512", "Custom"
        };
        static const int kResW[] = { 1920, 1280, 854, 3840, 800, 640, 512, 0 };
        static const int kResH[] = { 1080,  720, 480, 2160, 600, 480, 512, 0 };
        constexpr int kResCount = 8;

        int res_idx = kResCount - 1; // "Custom" default
        for (int i = 0; i < kResCount - 1; ++i) {
            if (cfg.ndi_width == kResW[i] && cfg.ndi_height == kResH[i]) {
                res_idx = i; break;
            }
        }
        ImGui::Text("Resolution:"); ImGui::SameLine(110.0f); ImGui::SetNextItemWidth(140.0f);
        if (ImGui::Combo("##ndi_res", &res_idx, kResLabels, kResCount)) {
            if (res_idx < kResCount - 1) {
                cfg.ndi_width  = kResW[res_idx];
                cfg.ndi_height = kResH[res_idx];
                patch_changed = true;
            }
        }
        if (res_idx == kResCount - 1) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::InputInt("##ndi_w", &cfg.ndi_width, 0, 0)) {
                cfg.ndi_width = std::max(64, cfg.ndi_width);
                patch_changed = true;
            }
            ImGui::SameLine(); ImGui::TextDisabled("x");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::InputInt("##ndi_h", &cfg.ndi_height, 0, 0)) {
                cfg.ndi_height = std::max(64, cfg.ndi_height);
                patch_changed = true;
            }
        }

        // Frame rate
        static const char* kFpsLabels[] = {
            "60 fps", "59.94 fps", "50 fps", "30 fps", "29.97 fps", "25 fps", "24 fps", "Custom"
        };
        static const int kFpsN[] = { 60, 60000, 50, 30, 30000, 25, 24, 0 };
        static const int kFpsD[] = {  1,  1001,  1,  1,  1001,  1,  1, 0 };
        constexpr int kFpsCount = 8;

        int fps_idx = kFpsCount - 1;
        for (int i = 0; i < kFpsCount - 1; ++i) {
            if (cfg.ndi_fps_N == kFpsN[i] && cfg.ndi_fps_D == kFpsD[i]) {
                fps_idx = i; break;
            }
        }
        ImGui::Text("Frame Rate:"); ImGui::SameLine(110.0f); ImGui::SetNextItemWidth(140.0f);
        if (ImGui::Combo("##ndi_fps", &fps_idx, kFpsLabels, kFpsCount)) {
            if (fps_idx < kFpsCount - 1) {
                cfg.ndi_fps_N = kFpsN[fps_idx];
                cfg.ndi_fps_D = kFpsD[fps_idx];
                patch_changed = true;
            }
        }
        if (fps_idx == kFpsCount - 1) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::InputInt("##ndi_fn", &cfg.ndi_fps_N, 0, 0)) {
                cfg.ndi_fps_N = std::max(1, cfg.ndi_fps_N);
                patch_changed = true;
            }
            ImGui::SameLine(); ImGui::TextDisabled("/");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.0f);
            if (ImGui::InputInt("##ndi_fd", &cfg.ndi_fps_D, 0, 0)) {
                cfg.ndi_fps_D = std::max(1, cfg.ndi_fps_D);
                patch_changed = true;
            }
        }

        // Clock mode
        if (ImGui::Checkbox("NDI-clocked timing##ndi_clk", &cfg.ndi_clock_video)) {
            patch_changed = true;
        }
        ImGui::SetItemTooltip(
            "ON (recommended): timing driven by NDI internal clock.\n"
            "   Provides stable frame pacing, adds ~1 frame latency.\n"
            "OFF: timing driven by application render loop.\n"
            "   Lower latency, less stable under CPU load.");

        // Status
        ImGui::Spacing();
        if (sel->ndi_active) {
            ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f),
                "Streaming — %d receiver%s", sel->ndi_conns, sel->ndi_conns == 1 ? "" : "s");
        } else {
            ImGui::TextDisabled("Not streaming (output disabled or no NDI sender active)");
        }

    } else if (cfg.type == OutputStreamType::HDMI) {
        ImGui::SeparatorText("HDMI / Display Window Settings");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Opens a borderless window on the selected display. "
            "Drag it to the target projector screen in your OS display settings.");
        ImGui::Spacing();
        ImGui::TextDisabled("Display selection: drag window to target monitor after launch.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "(Full per-display mapping, resolution override, and bezel correction "
            "will be added in a future release.)");
    }

    // ── Output Transform ──────────────────────────────────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Transform##out_xf", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);
        ImGui::TextDisabled(
            "Offsets and transforms applied to this output's frame "
            "before safety blackout.");
        ImGui::Spacing();

        float step = 0.005f;

        ImGui::Text("Offset X:"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragFloat("##xf_ox", &cfg.transform.offset_x, step, -1.0f, 1.0f, "%.3f"))
            patch_changed = true;
        ImGui::Text("Offset Y:"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragFloat("##xf_oy", &cfg.transform.offset_y, step, -1.0f, 1.0f, "%.3f"))
            patch_changed = true;

        ImGui::Text("Scale X:"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragFloat("##xf_sx", &cfg.transform.scale_x, step, 0.05f, 4.0f, "%.3f"))
            patch_changed = true;
        ImGui::Text("Scale Y:"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragFloat("##xf_sy", &cfg.transform.scale_y, step, 0.05f, 4.0f, "%.3f"))
            patch_changed = true;

        ImGui::Text("Rotation:"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("##xf_rot", &cfg.transform.rotation, -180.0f, 180.0f, "%.1f deg"))
            patch_changed = true;

        if (ImGui::Checkbox("Flip X##xf_fx", &cfg.transform.flip_x)) patch_changed = true;
        ImGui::SameLine(0, 20.0f);
        if (ImGui::Checkbox("Flip Y##xf_fy", &cfg.transform.flip_y)) patch_changed = true;

        ImGui::SameLine(0, 20.0f);
        if (ImGui::Button("Reset##xf_rst")) {
            cfg.transform = OutputTransform{};
            patch_changed = true;
        }
        ImGui::SetItemTooltip("Reset all transform parameters to defaults.");

        ImGui::Unindent(8.0f);
    }

    // ── Per-output Safety Zones ───────────────────────────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Per-Output Safety Zones##out_saf")) {
        ImGui::Indent(8.0f);
        ImGui::TextWrapped(
            "Safety blackout applied to this output only, after the global blackout. "
            "Use for per-projector audience protection zones.");
        ImGui::Spacing();
        if (draw_safety_zone_editor(cfg.safety, sel->id * 10000))
            patch_changed = true;
        ImGui::Unindent(8.0f);
    }

    // ── Fire callback if anything changed ─────────────────────────────────────
    if (patch_changed && cbs.on_output_patch_changed)
        cbs.on_output_patch_changed(collect_configs(state));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Add-output modal
// ─────────────────────────────────────────────────────────────────────────────
static void draw_add_modal(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    if (!ctx.patch_add_modal) return;

    ImGui::OpenPopup("Add Output##add_modal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f));

    if (!ImGui::BeginPopupModal("Add Output##add_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ctx.patch_add_modal = false;  // dismissed (Escape key or lost focus)
        return;
    }

    ImGui::SeparatorText("Output Type");
    ImGui::Spacing();

    static const char* kTypeLabels[] = { "Laser / DAC", "NDI Video Stream", "HDMI Window" };
    for (int t = 0; t < 3; ++t) {
        bool sel = (ctx.patch_add_type == t);
        ImGui::PushStyleColor(ImGuiCol_Text,
            sel ? stream_type_color(static_cast<OutputStreamType>(t))
                : ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        if (ImGui::RadioButton(kTypeLabels[t], sel))
            ctx.patch_add_type = t;
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Count & Name");

    ImGui::Text("How many:");
    ImGui::SameLine(100.0f); ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("##add_count", &ctx.patch_add_count, 1, 4)) {
        ctx.patch_add_count = std::clamp(ctx.patch_add_count, 1, 16);
    }
    ImGui::SetItemTooltip("Number of outputs of this type to add at once (1–16).");

    // Name prefix — default changes when type radio changes
    static char s_add_name_prefix[64] = "Laser";
    // Keep prefix in sync when type changes and the field still holds the old default
    {
        static int s_last_type = 0;
        if (ctx.patch_add_type != s_last_type) {
            s_last_type = ctx.patch_add_type;
            switch (ctx.patch_add_type) {
            case 0: std::strncpy(s_add_name_prefix, "Laser", sizeof(s_add_name_prefix) - 1); break;
            case 1: std::strncpy(s_add_name_prefix, "NDI",   sizeof(s_add_name_prefix) - 1); break;
            case 2: std::strncpy(s_add_name_prefix, "HDMI",  sizeof(s_add_name_prefix) - 1); break;
            default: break;
            }
            s_add_name_prefix[sizeof(s_add_name_prefix) - 1] = '\0';
        }
    }

    ImGui::Text("Name prefix:");
    ImGui::SameLine(100.0f); ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##add_prefix", s_add_name_prefix, sizeof(s_add_name_prefix));
    ImGui::SetItemTooltip(
        "Will create: \"%s 1\", \"%s 2\", etc.",
        s_add_name_prefix, s_add_name_prefix);

    // Preview label
    if (ctx.patch_add_count > 1) {
        ImGui::TextDisabled("Will create: \"%s 1\" .. \"%s %d\"",
            s_add_name_prefix, s_add_name_prefix, ctx.patch_add_count);
    } else {
        // count of this type already in list
        int existing_count = 0;
        for (const auto& ep : state.patched_outputs)
            if (ep.type == static_cast<OutputStreamType>(ctx.patch_add_type)) ++existing_count;
        ImGui::TextDisabled("Will create: \"%s %d\"",
            s_add_name_prefix, existing_count + 1);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Add", ImVec2(110.0f, 0.0f))) {
        auto add_type = static_cast<OutputStreamType>(ctx.patch_add_type);
        // snapshot existing count before adding
        int count_of_type_base = 0;
        for (const auto& existing : state.patched_outputs)
            if (existing.type == add_type) ++count_of_type_base;

        for (int i = 0; i < ctx.patch_add_count; ++i) {
            UIState::PatchedOutput po;
            po.id   = ctx.patch_next_id++;
            po.type = add_type;

            OutputStreamConfig cfg;
            cfg.id   = po.id;
            cfg.type = add_type;

            int num = count_of_type_base + i + 1;
            cfg.name = std::string(s_add_name_prefix) + " " + std::to_string(num);

            if (add_type == OutputStreamType::NDI)
                cfg.ndi_name = "IDHMFIS " + std::to_string(num);

            po.name    = cfg.name;
            po.enabled = cfg.enabled;
            po.config  = cfg;

            state.patched_outputs.push_back(po);
        }

        // Select the first newly-added output
        if (!state.patched_outputs.empty())
            ctx.patch_selected_id = state.patched_outputs.back().id;

        if (cbs.on_output_patch_changed)
            cbs.on_output_patch_changed(collect_configs(state));

        ctx.patch_add_modal = false;
        ctx.patch_add_count = 1;
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine(0, 8.0f);

    if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f))) {
        ctx.patch_add_modal = false;
        ctx.patch_add_count = 1;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Global safety note — redirect users to the global blackout in Setup
// ─────────────────────────────────────────────────────────────────────────────
static void draw_global_safety_note() {
    ImGui::Spacing();
    ImGui::SeparatorText("Global Safety Blackout");
    ImGui::TextWrapped(
        "The global safety blackout (applied to all outputs) is configured in "
        "Setup > Blackout Zones. Per-output safety zones are configured per-output "
        "in the Properties panel on the right.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  panel_patch — main entry point
// ─────────────────────────────────────────────────────────────────────────────
void panel_patch(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    // ── Global style polish ───────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(6.f, 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(6.f, 5.f));

    const float avail_w   = ImGui::GetContentRegionAvail().x;
    const float avail_h   = ImGui::GetContentRegionAvail().y;
    // Reserve bottom bar height (one button row + spacing)
    const float bottom_h  = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    const float col_h     = avail_h - bottom_h;
    const float left_w    = std::floor(avail_w / 3.0f);
    const float right_w   = avail_w - left_w - ImGui::GetStyle().ItemSpacing.x;

    // ── Left column: output list ──────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kBg1);
    ImGui::BeginChild("##patch_left", ImVec2(left_w, col_h), ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    {
        // Toolbar
        ImGui::Spacing();

        // ADD SINGLE — accent cyan
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImVec4(0.f, theme::kAccent.y * 0.35f, theme::kAccent.z * 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(0.f, theme::kAccent.y * 0.55f, theme::kAccent.z * 0.55f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImVec4(0.f, theme::kAccent.y * 0.25f, theme::kAccent.z * 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kAccent);
        if (ImGui::Button("ADD SINGLE")) {
            ctx.patch_add_type  = 0;
            ctx.patch_add_count = 1;
            ctx.patch_add_modal = true;
        }
        ImGui::PopStyleColor(4);
        ImGui::SetItemTooltip("Add a single output (Laser, NDI, or HDMI).");

        ImGui::SameLine(0, 6.0f);

        // ADD MULTIPLE — blue-tint
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.30f, 0.55f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.45f, 0.72f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.08f, 0.22f, 0.40f, 1.0f));
        if (ImGui::Button("ADD MULTIPLE")) {
            ctx.patch_add_type  = 0;
            ctx.patch_add_count = 4;
            ctx.patch_add_modal = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip("Add multiple outputs of the same type at once.");

        // REINIT STREAMS — amber; stops and restarts every DacManager.
        // Placed here (OUTPUTS page) so it is visible whenever streams are
        // visible and accessible without switching to a different view.
        ImGui::SameLine(0, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.13f, 0.02f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.34f, 0.06f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.16f, 0.09f, 0.02f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f,  0.72f, 0.15f, 1.0f));
        if (ImGui::Button("REINIT##ppreinit")) {
            if (cbs.on_reinit_streams) cbs.on_reinit_streams();
        }
        ImGui::PopStyleColor(4);
        ImGui::SetItemTooltip(
            "Re-initialize all output streams.\n"
            "Stops every DAC manager and restarts them from scratch.\n"
            "Use when streams are not sending after loading a show,\n"
            "or when Capture 2024 / DAC hardware loses its connection.");

        // Column header underline in accent color
        {
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1 = ImVec2(p0.x + ImGui::GetContentRegionAvail().x, p0.y);
            ImGui::GetWindowDrawList()->AddLine(p0, p1,
                IM_COL32(
                    static_cast<int>(theme::kAccent.x * 255),
                    static_cast<int>(theme::kAccent.y * 255),
                    static_cast<int>(theme::kAccent.z * 255),
                    80),
                1.0f);
        }
        ImGui::Separator();
        ImGui::Spacing();

        // Empty state
        if (state.patched_outputs.empty()) {
            ImGui::TextDisabled("No outputs patched.");
            ImGui::Spacing();
            ImGui::TextDisabled("Click ADD SINGLE or ADD MULTIPLE");
            ImGui::TextDisabled("to configure your first output.");
        }

        // Broadcast toggle: when ON all outputs receive programmer content.
        // When OFF, only the targeted outputs (marked with the T button) do.
        {
            bool bcast = state.broadcast_to_all;
            if (ImGui::Checkbox("Broadcast to all##bcast", &bcast)) {
                state.broadcast_to_all = bcast;
                if (bcast) {
                    // Broadcast mode: send all enabled laser IDs so the engine runs
                    // the programmer for every output.  Sending an empty set would
                    // disable the programmer entirely (empty = no filter = cue-only).
                    state.selected_output_ids.clear();
                    for (auto& po2 : state.patched_outputs)
                        po2.selected_for_programming = false;
                    std::vector<int> all_ids;
                    for (const auto& po : state.patched_outputs)
                        if (po.type == OutputStreamType::Laser && po.config.enabled)
                            all_ids.push_back(po.id);
                    std::vector<int> old_ids_bcast = state.active_stream_ids;
                    state.active_stream_ids = all_ids;
                    if (cbs.on_active_streams_changed)
                        cbs.on_active_streams_changed(old_ids_bcast, state.active_stream_ids);
                }
            }
            ImGui::SetItemTooltip(
                "ON: programmer content goes to all outputs.\n"
                "OFF: use the TARGET button (T) next to each output\n"
                "to choose which outputs receive programmer content.");
        }
        ImGui::Spacing();

        // Output list
        int remove_id = -1;
        for (int i = 0; i < static_cast<int>(state.patched_outputs.size()); ++i) {
            auto& po = state.patched_outputs[static_cast<size_t>(i)];
            ImGui::PushID(po.id);

            // ── Alternating row background tint ───────────────────────────────
            {
                ImVec2 row_min = ImGui::GetCursorScreenPos();
                row_min.x = ImGui::GetWindowPos().x;
                float row_w   = ImGui::GetWindowSize().x;
                float row_h   = ImGui::GetTextLineHeightWithSpacing() + 2.f;
                ImVec2 row_max = ImVec2(row_min.x + row_w, row_min.y + row_h);
                bool is_selected = (ctx.patch_selected_id == po.id);
                if (is_selected) {
                    // Selected row: accent highlight at 20% alpha
                    ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max,
                        IM_COL32(
                            static_cast<int>(theme::kAccent.x * 255),
                            static_cast<int>(theme::kAccent.y * 255),
                            static_cast<int>(theme::kAccent.z * 255),
                            51));  // ~20% alpha
                } else if (i % 2 == 0) {
                    // Even rows: slightly lighter tint
                    ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max,
                        IM_COL32(255, 255, 255, 8));  // very subtle white overlay
                }
            }

            // Target indicator: small circle that when clicked toggles this output
            // into/out of selected_output_ids (only active when broadcast_to_all == false).
            {
                bool in_sel = false;
                for (int sid : state.selected_output_ids)
                    if (sid == po.id) { in_sel = true; break; }

                ImVec2 cursor = ImGui::GetCursorScreenPos();
                float  dot_r  = ImGui::GetTextLineHeight() * 0.38f;
                ImU32  tgt_col = in_sel
                    ? IM_COL32(255, 200, 0, 255)    // gold = targeted
                    : IM_COL32(70,  70,  70, 200);  // grey = not targeted
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(cursor.x + dot_r, cursor.y + dot_r + 2.0f), dot_r, tgt_col);
                ImGui::Dummy(ImVec2(dot_r * 2.0f + 4.0f, ImGui::GetTextLineHeight()));
                if (ImGui::IsItemClicked()) {
                    // Toggle this output in the selection
                    if (in_sel) {
                        auto& sel = state.selected_output_ids;
                        sel.erase(std::remove(sel.begin(), sel.end(), po.id), sel.end());
                        po.selected_for_programming = false;
                    } else {
                        state.selected_output_ids.push_back(po.id);
                        po.selected_for_programming = true;
                    }
                    // Disable broadcast mode when user manually targets outputs
                    state.broadcast_to_all = false;
                    std::vector<int> old_ids_tgt2 = state.active_stream_ids;
                    state.active_stream_ids = state.selected_output_ids;
                    if (cbs.on_active_streams_changed)
                        cbs.on_active_streams_changed(old_ids_tgt2, state.active_stream_ids);
                }
                ImGui::SetItemTooltip(in_sel
                    ? "Targeted — programmer content goes here. Click to un-target."
                    : "Click to target this output for programmer content.");
            }
            ImGui::SameLine(0, 2.0f);

            // Status indicator dot (enabled/disabled) — 6px radius with glow
            {
                ImVec2 cursor  = ImGui::GetCursorScreenPos();
                constexpr float kDotR      = 6.f;
                constexpr float kGlowR     = 9.f;
                ImVec2          dot_center = ImVec2(cursor.x + kDotR, cursor.y + kDotR + 1.0f);
                if (po.enabled) {
                    // Glow ring: green at 30% alpha
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        dot_center, kGlowR, IM_COL32(50, 210, 80, 77));
                    // Core dot
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        dot_center, kDotR, IM_COL32(50, 210, 80, 255));
                } else {
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        dot_center, kDotR, IM_COL32(110, 110, 110, 200));
                }
                ImGui::Dummy(ImVec2(kDotR * 2.0f + 4.0f, ImGui::GetTextLineHeight()));
            }
            ImGui::SameLine(0, 4.0f);

            // Type badge
            ImGui::PushStyleColor(ImGuiCol_Text, stream_type_color(po.type));
            ImGui::TextUnformatted(stream_type_label(po.type));
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 6.0f);

            // In MOVE mode show a drag-handle glyph so the row looks draggable
            if (ctx.stream_move_mode) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.6f, 0.9f, 1.f));
                ImGui::TextUnformatted(":::");
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 4.0f);
            }

            // Row — selectable
            bool selected = (ctx.patch_selected_id == po.id);
            char sel_label[64];
            std::snprintf(sel_label, sizeof(sel_label), "%s###po_%d",
                          po.name.empty() ? "(unnamed)" : po.name.c_str(), po.id);

            if (ImGui::Selectable(sel_label, selected,
                                  ImGuiSelectableFlags_SpanAllColumns,
                                  ImVec2(0.0f, 0.0f))) {
                if (state.rem_mode) {
                    remove_id = po.id;
                    state.rem_mode = false;
                } else if (!ctx.stream_move_mode) {
                    ctx.patch_selected_id = po.id;
                }
            }

            // Drag-and-drop reordering when MOVE mode is active
            if (ctx.stream_move_mode) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("PATCH_ROW", &i, sizeof(int));
                    ImGui::Text("[%s] %s", stream_type_label(po.type), po.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PATCH_ROW")) {
                        int from = *static_cast<const int*>(pl->Data);
                        int to   = i;
                        if (from >= 0 && from < static_cast<int>(state.patched_outputs.size()) &&
                            to   >= 0 && to   < static_cast<int>(state.patched_outputs.size()) &&
                            from != to) {
                            // Move the dragged row to the drop position.
                            // After erasing `from`, `to` is already the correct
                            // insertion index regardless of direction.
                            UIState::PatchedOutput moved = state.patched_outputs[static_cast<size_t>(from)];
                            state.patched_outputs.erase(
                                state.patched_outputs.begin() + from);
                            // Clamp in case erase shortened the vector
                            int insert_at = std::min(to, static_cast<int>(state.patched_outputs.size()));
                            state.patched_outputs.insert(
                                state.patched_outputs.begin() + insert_at, moved);
                            // Rebuild DacManagers with new ordinal mapping
                            if (cbs.on_output_patch_changed)
                                cbs.on_output_patch_changed(collect_configs(state));
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
            }

            // Save id before context menu — insert below can reallocate the vector,
            // making the `po` reference dangle before we finish the popup.
            const int cur_id = po.id;

            // Context menu: Duplicate, Remove (suppressed in MOVE mode)
            if (!ctx.stream_move_mode && ImGui::BeginPopupContextItem("##po_ctx")) {
                if (ImGui::MenuItem("Duplicate")) {
                    UIState::PatchedOutput copy = po;  // copy before insert
                    copy.id = ctx.patch_next_id++;
                    copy.config.id = copy.id;
                    copy.name += " (copy)";
                    copy.config.name = copy.name;
                    state.patched_outputs.insert(
                        state.patched_outputs.begin() + i + 1, copy);
                    ctx.patch_selected_id = copy.id;
                    if (cbs.on_output_patch_changed)
                        cbs.on_output_patch_changed(collect_configs(state));
                }
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                if (ImGui::MenuItem("Remove"))
                    remove_id = cur_id;
                ImGui::PopStyleColor();
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        if (remove_id >= 0) {
            auto& v = state.patched_outputs;
            v.erase(std::remove_if(v.begin(), v.end(),
                [remove_id](const UIState::PatchedOutput& p){ return p.id == remove_id; }),
                v.end());
            if (ctx.patch_selected_id == remove_id)
                ctx.patch_selected_id = v.empty() ? -1 : v.front().id;
            // Remove from target selection if it was targeted
            {
                auto& sel = state.selected_output_ids;
                std::vector<int> old_ids_del = state.active_stream_ids;
                sel.erase(std::remove(sel.begin(), sel.end(), remove_id), sel.end());
                state.active_stream_ids = state.selected_output_ids;
                if (cbs.on_active_streams_changed)
                    cbs.on_active_streams_changed(old_ids_del, state.active_stream_ids);
            }
            if (cbs.on_output_patch_changed)
                cbs.on_output_patch_changed(collect_configs(state));
        }

        draw_global_safety_note();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Right column: properties ──────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kBg1);
    ImGui::BeginChild("##patch_right", ImVec2(right_w, col_h), ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    {
        ImGui::Spacing();
        draw_output_properties(state, ctx, cbs);
    }
    ImGui::EndChild();

    // Add modal (opened from toolbar buttons)
    draw_add_modal(state, ctx, cbs);

    // ── Bottom bar: Apply Patch button + output count summary ─────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Output count summary
    {
        int n_laser = 0, n_ndi = 0, n_hdmi = 0;
        for (const auto& p : state.patched_outputs) {
            if (p.type == OutputStreamType::Laser)     ++n_laser;
            else if (p.type == OutputStreamType::NDI)  ++n_ndi;
            else if (p.type == OutputStreamType::HDMI) ++n_hdmi;
        }
        int total = static_cast<int>(state.patched_outputs.size());
        if (total == 0) {
            ImGui::TextDisabled("No outputs patched");
        } else {
            // Build a compact summary string
            char summary[128];
            int off = std::snprintf(summary, sizeof(summary),
                "%d output%s patched", total, total == 1 ? "" : "s");
            bool first = true;
            if (n_laser > 0) {
                off += std::snprintf(summary + off,
                    static_cast<size_t>(static_cast<int>(sizeof(summary)) - off),
                    "%s%d Laser", first ? " (" : ", ", n_laser);
                first = false;
            }
            if (n_ndi > 0) {
                off += std::snprintf(summary + off,
                    static_cast<size_t>(static_cast<int>(sizeof(summary)) - off),
                    "%s%d NDI", first ? " (" : ", ", n_ndi);
                first = false;
            }
            if (n_hdmi > 0) {
                off += std::snprintf(summary + off,
                    static_cast<size_t>(static_cast<int>(sizeof(summary)) - off),
                    "%s%d HDMI", first ? " (" : ", ", n_hdmi);
                first = false;
            }
            if (!first && off < static_cast<int>(sizeof(summary)) - 1)
                summary[off++] = ')';
            if (off < static_cast<int>(sizeof(summary)))
                summary[off] = '\0';
            ImGui::TextDisabled("%s", summary);
        }
    }

    // ── Close global style overrides ─────────────────────────────────────────
    ImGui::PopStyleVar(2);  // FramePadding, ItemSpacing
}

} // namespace idhmfis
