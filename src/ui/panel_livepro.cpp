// IDHMFIS — LivePRO performance mode panel implementation
// Three zones: XY pad | Quick faders | Beat grid

#include "panel_livepro.h"
#include "imgui.h"

#include <cstdio>
#include <algorithm>
#include <cmath>

namespace idhmfis {

static constexpr const char* kWinLivePRO = "LivePRO##w";

// ─────────────────────────────────────────────────────────────────────────────
//  panel_livepro
// ─────────────────────────────────────────────────────────────────────────────
void panel_livepro(UIState& state, LayoutCallbacks& cbs, bool* p_open)
{
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.f, 240.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.09f, 1.f));

    if (!ImGui::Begin(kWinLivePRO, p_open)) {
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    ImGui::PopStyleColor();

    float dt = ImGui::GetIO().DeltaTime;

    // Tick-down beat flash timers
    for (int i = 0; i < 16; ++i) {
        if (state.beat_flash_timers[i] > 0.f)
            state.beat_flash_timers[i] -= dt;
        if (state.beat_flash_timers[i] < 0.f)
            state.beat_flash_timers[i] = 0.f;
    }

    float total_w = ImGui::GetContentRegionAvail().x;
    float total_h = ImGui::GetContentRegionAvail().y;

    // Zone widths: A=40%, B=25%, C=35%
    float zone_a_w = total_w * 0.40f;
    float zone_b_w = total_w * 0.25f;
    float zone_c_w = total_w - zone_a_w - zone_b_w;  // remainder (~35%)

    // =========================================================================
    // Zone A — XY Performance Pad (left 40%)
    // =========================================================================
    ImGui::BeginGroup();
    {
        ImGui::SeparatorText("PAN / TILT");
        float label_h = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        float pad_size = total_h - label_h - ImGui::GetStyle().ItemSpacing.y;
        // Keep the pad square using the smaller of width/height
        pad_size = std::min(pad_size, zone_a_w - ImGui::GetStyle().ItemSpacing.x);
        if (pad_size < 20.f) pad_size = 20.f;

        ImVec2 pad_pos = ImGui::GetCursorScreenPos();

        // Invisible button captures mouse interaction
        ImGui::InvisibleButton("##xy_pad", ImVec2(pad_size, pad_size));
        bool is_active  = ImGui::IsItemActive();
        bool is_hovered = ImGui::IsItemHovered();

        // XY pad tooltip
        ImGui::SetItemTooltip(
            "XY performance pad — drag to set Pan (X) and Tilt (Y)\n"
            "Range: -1.0 to +1.0 on each axis");

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(
            pad_pos,
            ImVec2(pad_pos.x + pad_size, pad_pos.y + pad_size),
            IM_COL32(18, 18, 24, 255),
            4.f);
        dl->AddRect(
            pad_pos,
            ImVec2(pad_pos.x + pad_size, pad_pos.y + pad_size),
            is_hovered ? IM_COL32(80, 120, 200, 255) : IM_COL32(50, 50, 70, 255),
            4.f, 0, 1.5f);

        // Crosshair at center
        float cx = pad_pos.x + pad_size * 0.5f;
        float cy = pad_pos.y + pad_size * 0.5f;
        dl->AddLine(ImVec2(pad_pos.x + 4.f, cy),
                    ImVec2(pad_pos.x + pad_size - 4.f, cy),
                    IM_COL32(60, 60, 80, 200), 1.f);
        dl->AddLine(ImVec2(cx, pad_pos.y + 4.f),
                    ImVec2(cx, pad_pos.y + pad_size - 4.f),
                    IM_COL32(60, 60, 80, 200), 1.f);

        // Drag handling
        if (is_active) {
            ImVec2 mouse = ImGui::GetMousePos();
            // Normalise to -1..1
            float nx = ((mouse.x - pad_pos.x) / pad_size) * 2.f - 1.f;
            float ny = ((mouse.y - pad_pos.y) / pad_size) * 2.f - 1.f;
            nx = std::clamp(nx, -1.f, 1.f);
            ny = std::clamp(ny, -1.f, 1.f);

            if (state.livepro_x != nx || state.livepro_y != ny) {
                state.livepro_x = nx;
                state.livepro_y = ny;
                if (cbs.on_livepro_xy)
                    cbs.on_livepro_xy(nx, ny);
            }
        }

        // Draw position dot (convert -1..1 to screen)
        float dot_sx = pad_pos.x + (state.livepro_x * 0.5f + 0.5f) * pad_size;
        float dot_sy = pad_pos.y + (state.livepro_y * 0.5f + 0.5f) * pad_size;
        dot_sx = std::clamp(dot_sx, pad_pos.x + 2.f, pad_pos.x + pad_size - 2.f);
        dot_sy = std::clamp(dot_sy, pad_pos.y + 2.f, pad_pos.y + pad_size - 2.f);

        dl->AddCircleFilled(ImVec2(dot_sx, dot_sy), 7.f, IM_COL32(0, 210, 255, 230));
        dl->AddCircle(ImVec2(dot_sx, dot_sy), 7.f, IM_COL32(255, 255, 255, 180), 16, 1.5f);

        // Coordinate readout in bottom-left corner of pad
        char coord_lbl[40];
        std::snprintf(coord_lbl, sizeof(coord_lbl),
                      "X:%.2f  Y:%.2f", state.livepro_x, state.livepro_y);
        dl->AddText(
            ImVec2(pad_pos.x + 4.f, pad_pos.y + pad_size - ImGui::GetTextLineHeight() - 4.f),
            IM_COL32(120, 120, 140, 200),
            coord_lbl);

        // Corner axis labels for orientation
        dl->AddText(ImVec2(pad_pos.x + 4.f, pad_pos.y + 4.f),
                    IM_COL32(70, 70, 100, 160), "L");
        dl->AddText(ImVec2(pad_pos.x + pad_size - 10.f, pad_pos.y + 4.f),
                    IM_COL32(70, 70, 100, 160), "R");
        dl->AddText(ImVec2(pad_pos.x + 4.f, pad_pos.y + 4.f + ImGui::GetTextLineHeight()),
                    IM_COL32(70, 70, 100, 160), "U");
    }
    ImGui::EndGroup();

    ImGui::SameLine(0.f, 6.f);

    // =========================================================================
    // Zone B — Quick Faders (center 25%)
    // =========================================================================
    ImGui::BeginGroup();
    {
        ImGui::SeparatorText("FADERS");

        float fader_h    = (total_h - ImGui::GetTextLineHeightWithSpacing()
                            - ImGui::GetStyle().ItemSpacing.y * 6.f) / 4.f;
        float fader_w    = zone_b_w - ImGui::GetStyle().ItemSpacing.x;
        if (fader_h < 30.f) fader_h = 30.f;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 6.f));

        // 1. Master Intensity (0–100%)
        {
            ImGui::TextUnformatted("Master");
            float pct = state.master_intensity * 100.f;
            ImGui::SetNextItemWidth(fader_w);
            if (ImGui::SliderFloat("##lp_intensity", &pct, 0.f, 100.f, "%.0f%%",
                                   ImGuiSliderFlags_AlwaysClamp))
            {
                state.master_intensity = pct / 100.f;
                if (cbs.on_master_intensity)
                    cbs.on_master_intensity(state.master_intensity);
            }
            ImGui::SetItemTooltip("Master output intensity (0%% = blackout, 100%% = full)");
        }

        // 2. BPM (40–200)
        {
            char bpm_lbl[32];
            std::snprintf(bpm_lbl, sizeof(bpm_lbl), "BPM: %.0f", state.bpm);
            ImGui::TextUnformatted(bpm_lbl);
            ImGui::SetNextItemWidth(fader_w);
            if (ImGui::SliderFloat("##lp_bpm", &state.bpm, 40.f, 200.f, "%.0f BPM",
                                   ImGuiSliderFlags_AlwaysClamp))
            {
                if (cbs.on_bpm_changed)
                    cbs.on_bpm_changed(state.bpm);
            }
            ImGui::SetItemTooltip(
                "Tempo in beats per minute — used by beat-sync effects and the beat grid\n"
                "Range: 40 – 200 BPM  |  Ctrl+Click to type a value");
        }

        // 3. Cue Scale (0–200%)
        {
            ImGui::TextUnformatted("Scale");
            float pct = state.livepro_scale * 100.f;
            ImGui::SetNextItemWidth(fader_w);
            if (ImGui::SliderFloat("##lp_scale", &pct, 0.f, 200.f, "%.0f%%",
                                   ImGuiSliderFlags_AlwaysClamp))
            {
                state.livepro_scale = pct / 100.f;
                if (cbs.on_livepro_scale)
                    cbs.on_livepro_scale(state.livepro_scale);
            }
            ImGui::SetItemTooltip(
                "Output geometry scale — 100%% is native cue size, 200%% is double");
        }

        // 4. Cue Speed (0–400%)
        {
            ImGui::TextUnformatted("Speed");
            float pct = state.livepro_speed * 100.f;
            ImGui::SetNextItemWidth(fader_w);
            if (ImGui::SliderFloat("##lp_speed", &pct, 0.f, 400.f, "%.0f%%",
                                   ImGuiSliderFlags_AlwaysClamp))
            {
                state.livepro_speed = pct / 100.f;
                if (cbs.on_livepro_speed)
                    cbs.on_livepro_speed(state.livepro_speed);
            }
            ImGui::SetItemTooltip(
                "Animation playback speed multiplier\n"
                "100%% = normal, 200%% = double speed, 0%% = frozen");
        }

        ImGui::PopStyleVar();
    }
    ImGui::EndGroup();

    ImGui::SameLine(0.f, 6.f);

    // =========================================================================
    // Zone C — Beat Grid (right 35%, 4x4)
    // =========================================================================
    ImGui::BeginGroup();
    {
        // Header row with FLASH HOLD toggle
        ImGui::SeparatorText("BEAT GRID");

        // FLASH HOLD toggle — clearly labelled ON/OFF state
        {
            bool hold = state.livepro_flash_hold;
            if (hold) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.8f, 0.2f, 0.1f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.1f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.7f, 0.1f, 0.0f, 1.f));
            }
            const char* flash_lbl = hold ? "FLASH [ON]" : "FLASH [OFF]";
            if (ImGui::SmallButton(flash_lbl))
                state.livepro_flash_hold = !hold;
            if (hold) ImGui::PopStyleColor(3);
            ImGui::SetItemTooltip(
                hold
                ? "Flash Hold ON — beat pads stay lit while held; click to disable"
                : "Flash Hold OFF — beat pads flash briefly on trigger; click to enable hold mode");
        }

        // 4×4 beat button grid
        constexpr int kBeatRows = 4;
        constexpr int kBeatCols = 4;

        float header_h  = ImGui::GetTextLineHeightWithSpacing()
                          + ImGui::GetStyle().ItemSpacing.y * 2.f;
        float btn_gap   = 4.f;
        float avail_h   = total_h - header_h - (float)(kBeatRows - 1) * btn_gap;
        float avail_bw  = zone_c_w - (float)(kBeatCols - 1) * btn_gap
                          - ImGui::GetStyle().WindowPadding.x;
        float btn_h     = avail_h / (float)kBeatRows;
        float btn_w     = avail_bw / (float)kBeatCols;
        if (btn_h < 20.f) btn_h = 20.f;
        if (btn_w < 24.f) btn_w = 24.f;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(btn_gap, btn_gap));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f, 2.f));

        for (int br = 0; br < kBeatRows; ++br) {
            for (int bc = 0; bc < kBeatCols; ++bc) {
                if (bc > 0) ImGui::SameLine(0.f, btn_gap);

                int slot = br * kBeatCols + bc;
                float flash = state.beat_flash_timers[slot];
                float flash_t = std::clamp(flash / 0.15f, 0.f, 1.f);

                // Base colour: dark blue-grey; flash: bright orange-yellow
                float r = 0.15f + flash_t * 0.85f;
                float g = 0.15f + flash_t * 0.60f;
                float b = 0.22f - flash_t * 0.10f;
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(r, g, b, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(r + 0.08f, g + 0.08f, b + 0.08f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(r - 0.05f, g - 0.05f, b - 0.05f, 1.f));

                // Label: use beat_labels if set, else default B1-B16
                char lbl[16];
                const std::string& user_lbl = state.beat_labels[slot];
                if (!user_lbl.empty())
                    std::snprintf(lbl, sizeof(lbl), "%s", user_lbl.c_str());
                else
                    std::snprintf(lbl, sizeof(lbl), "B%d", slot + 1);

                char btn_id[32];
                std::snprintf(btn_id, sizeof(btn_id), "%s##beat_%d", lbl, slot);

                if (ImGui::Button(btn_id, ImVec2(btn_w, btn_h))) {
                    // Trigger: fire callback + start flash
                    if (!state.livepro_flash_hold)
                        state.beat_flash_timers[slot] = 0.15f;
                    if (cbs.on_livepro_beat_trigger)
                        cbs.on_livepro_beat_trigger(slot);
                }

                // Beat button tooltip
                {
                    const char* user_name = (!state.beat_labels[slot].empty())
                        ? state.beat_labels[slot].c_str() : nullptr;
                    if (user_name) {
                        char tip[64];
                        std::snprintf(tip, sizeof(tip),
                            "Beat slot %d — \"%s\"\nClick to trigger effect",
                            slot + 1, user_name);
                        ImGui::SetItemTooltip("%s", tip);
                    } else {
                        char tip[48];
                        std::snprintf(tip, sizeof(tip),
                            "Beat slot B%d — click to trigger beat effect",
                            slot + 1);
                        ImGui::SetItemTooltip("%s", tip);
                    }
                }

                ImGui::PopStyleColor(3);
            }
        }

        ImGui::PopStyleVar(2);
    }
    ImGui::EndGroup();

    ImGui::End();
}

} // namespace idhmfis
