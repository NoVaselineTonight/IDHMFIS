// panel_bam.cpp — Beam Attenuation Map editor panel.
// Displays a 64x64 interactive grid for audience-scanning protection.

#include "layout.h"
#include "theme.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "../safety/bam.h"

#include <cstdio>
#include <algorithm>
#include <cstring>

namespace idhmfis {

// Window name — stable ID for docking
static constexpr const char* kWinBam = "Safety/BAM##w";

void panel_bam(UIState& state, LayoutContext& /*ctx*/, LayoutCallbacks& cbs)
{
    // Enforce minimum window size so the grid is always usable.
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 260.f), ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::Begin(kWinBam)) {
        ImGui::End();
        return;
    }

    // ── Header: enable toggle + safety status indicator ──────────────────────
    {
        bool enabled = state.bam.enabled;
        if (ImGui::Checkbox("Enable BAM", &enabled)) {
            if (cbs.on_bam_enabled) cbs.on_bam_enabled(enabled);
        }
        ImGui::SetItemTooltip(
            "Enable the Beam Attenuation Map. When active, each grid cell "
            "limits the laser power that can pass through that screen region.");

        ImGui::SameLine(0, 16);

        // Safety status badge — coloured circle + text for unambiguous status
        bool safe = state.bam.safety_ok;
        const char* status_text = state.bam.safety_status.c_str();

        // Filled circle indicator: green = safe, red = fault
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        float  r      = ImGui::GetTextLineHeight() * 0.45f;
        ImU32  dot_col = safe
            ? IM_COL32(40, 200, 60, 255)
            : IM_COL32(220, 40, 30, 255);
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(cursor.x + r, cursor.y + r + 2.f), r, dot_col);
        ImGui::Dummy(ImVec2(r * 2.f + 4.f, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0, 4);

        ImVec4 badge_col = safe
            ? ImVec4(0.25f, 0.90f, 0.35f, 1.f)
            : ImVec4(1.00f, 0.30f, 0.20f, 1.f);
        ImGui::PushStyleColor(ImGuiCol_Text, badge_col);
        if (!safe) {
            ImGui::TextUnformatted("\xe2\x9a\xa0 SAFETY FAULT: ");
        } else {
            ImGui::TextUnformatted("Safety: ");
        }
        ImGui::SameLine(0, 4);
        ImGui::TextUnformatted(status_text);
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::SeparatorText("Brush");

    // ── Toolbar: brush size + attenuation value + presets ────────────────────
    static int   s_brush_size = 1;      // 1, 2, 4, or 8
    static float s_atten_pct  = 0.f;   // 0% = full block, 100% = full pass

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Size:");
    ImGui::SameLine(0, 6);
    const int brush_sizes[] = { 1, 2, 4, 8 };
    for (int bs : brush_sizes) {
        char label[8];
        std::snprintf(label, sizeof(label), "%dx%d", bs, bs);
        bool selected = (s_brush_size == bs);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(theme::accent().x, theme::accent().y, theme::accent().z, 1.f));
        }
        if (ImGui::Button(label, ImVec2(36.f, 0.f)))
            s_brush_size = bs;
        if (selected)
            ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Paint with a %dx%d cell brush.", bs, bs);
        ImGui::SameLine(0, 4);
    }

    ImGui::SameLine(0, 16);
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("##atten", &s_atten_pct, 0.f, 100.f, "Atten: %.0f%%",
                       ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Attenuation level painted by the brush.\n"
        "0%% = fully blocked (red), 100%% = full pass (transparent).\n"
        "Ctrl+click on the grid to erase (set to 100%%).");

    ImGui::SameLine(0, 16);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.15f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.16f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.40f, 0.10f, 0.08f, 1.f));

    // Destructive action: require user to hold Ctrl to confirm clear.
    bool ctrl_held_global = ImGui::GetIO().KeyCtrl;
    ImGui::BeginDisabled(!ctrl_held_global);
    if (ImGui::Button("Clear All")) {
        if (cbs.on_bam_clear) cbs.on_bam_clear();
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip(
        "Reset all cells to full pass (100%%). Hold Ctrl then click to confirm.\n"
        "This is irreversible — all painted attenuation will be lost.");

    ImGui::SameLine(0, 8);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.45f, 0.10f, 0.55f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.16f, 0.70f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.32f, 0.07f, 0.40f, 1.f));
    if (ImGui::Button("Block Bottom Half")) {
        // Set cells where row > kSize/2 to 0 (audience area protection)
        if (cbs.on_bam_paint) {
            constexpr int half = BamGrid::kSize / 2;
            // Paint each cell in the lower half individually
            for (int r = half; r < BamGrid::kSize; ++r) {
                for (int c = 0; c < BamGrid::kSize; ++c) {
                    cbs.on_bam_paint(r, c, 1, 0);
                }
            }
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip(
        "Block the lower half of the output area (rows 32-63).\n"
        "Use this for quick audience-scan protection when the audience "
        "is located in the bottom half of the projection zone.");

    ImGui::Separator();
    // Keyboard hint
    ImGui::TextDisabled("Left-click+drag to paint  |  Ctrl+click to erase  |  brush size above");
    ImGui::Separator();

    // ── 64x64 grid canvas ─────────────────────────────────────────────────────
    ImVec2 canvas_pos  = ImGui::GetCursorScreenPos();
    ImVec2 avail       = ImGui::GetContentRegionAvail();
    // Reserve space for the Safety Zones section below (approx 3 lines).
    float zones_reserve = ImGui::GetTextLineHeightWithSpacing() * 3.5f;
    avail.y = std::max(avail.y - zones_reserve, 40.f);

    constexpr int  kGrid    = BamGrid::kSize;
    constexpr float kMinCell = 4.f;
    float cell_w = std::max(kMinCell, std::floor(avail.x / static_cast<float>(kGrid)));
    float cell_h = std::max(kMinCell, cell_w);  // keep cells square

    float grid_w = cell_w * static_cast<float>(kGrid);
    float grid_h = cell_h * static_cast<float>(kGrid);

    // Reserve space in the layout for the grid
    ImGui::InvisibleButton("##bam_grid", ImVec2(grid_w, grid_h),
                            ImGuiButtonFlags_MouseButtonLeft |
                            ImGuiButtonFlags_MouseButtonRight);

    // Interaction state
    static bool s_dragging = false;
    bool hovered  = ImGui::IsItemHovered();
    bool lmb_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool ctrl_held= ImGui::GetIO().KeyCtrl;

    if (hovered && lmb_down)  s_dragging = true;
    if (!lmb_down)             s_dragging = false;

    if ((s_dragging || ImGui::IsItemClicked(ImGuiMouseButton_Left)) && hovered) {
        ImVec2 mouse  = ImGui::GetIO().MousePos;
        float  rel_x  = mouse.x - canvas_pos.x;
        float  rel_y  = mouse.y - canvas_pos.y;
        int    col    = static_cast<int>(rel_x / cell_w);
        int    row    = static_cast<int>(rel_y / cell_h);
        col = std::clamp(col, 0, kGrid - 1);
        row = std::clamp(row, 0, kGrid - 1);

        // Ctrl+click = erase (255), plain click = paint with current attenuation
        uint8_t paint_val = ctrl_held
            ? static_cast<uint8_t>(255)
            : static_cast<uint8_t>(s_atten_pct / 100.f * 255.f);

        if (cbs.on_bam_paint)
            cbs.on_bam_paint(row, col, s_brush_size, paint_val);
    }

    // Draw cells
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int r = 0; r < kGrid; ++r) {
        for (int c = 0; c < kGrid; ++c) {
            uint8_t cell_val = state.bam.cells[r * kGrid + c];
            float  atten = static_cast<float>(cell_val) / 255.f;

            // Lerp from red (blocked) toward transparent (full pass)
            ImU32 fill_col;
            if (atten < 0.01f) {
                // Fully blocked — solid red
                fill_col = IM_COL32(200, 20, 20, 220);
            } else if (atten > 0.99f) {
                // Full pass — very faint grid line visible
                fill_col = IM_COL32(30, 35, 42, 60);
            } else {
                // Partial — lerp red → dark
                uint8_t r_ch = static_cast<uint8_t>(200.f * (1.f - atten));
                uint8_t a_ch = static_cast<uint8_t>(50.f + 170.f * (1.f - atten));
                fill_col = IM_COL32(r_ch, 10, 10, a_ch);
            }

            ImVec2 p0 = ImVec2(
                canvas_pos.x + static_cast<float>(c) * cell_w,
                canvas_pos.y + static_cast<float>(r) * cell_h);
            ImVec2 p1 = ImVec2(p0.x + cell_w - 1.f, p0.y + cell_h - 1.f);

            dl->AddRectFilled(p0, p1, fill_col);
        }
    }

    // Draw brush preview rectangle when hovering
    if (hovered) {
        ImVec2 mouse  = ImGui::GetIO().MousePos;
        float  rel_x  = mouse.x - canvas_pos.x;
        float  rel_y  = mouse.y - canvas_pos.y;
        int    col    = std::clamp(static_cast<int>(rel_x / cell_w), 0, kGrid - 1);
        int    row    = std::clamp(static_cast<int>(rel_y / cell_h), 0, kGrid - 1);
        int    half   = s_brush_size / 2;

        ImVec2 bp0 = ImVec2(
            canvas_pos.x + static_cast<float>(std::max(0, col - half)) * cell_w,
            canvas_pos.y + static_cast<float>(std::max(0, row - half)) * cell_h);
        ImVec2 bp1 = ImVec2(
            canvas_pos.x + static_cast<float>(std::min(kGrid - 1, col + half) + 1) * cell_w,
            canvas_pos.y + static_cast<float>(std::min(kGrid - 1, row + half) + 1) * cell_h);

        dl->AddRect(bp0, bp1, IM_COL32(255, 255, 100, 220), 0.f, 0, 1.5f);
    }

    // Grid border
    dl->AddRect(canvas_pos,
                ImVec2(canvas_pos.x + grid_w, canvas_pos.y + grid_h),
                IM_COL32(80, 85, 95, 180), 0.f, 0, 1.f);

    // ── Safety Zones list ─────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Safety Zones");
    ImGui::TextDisabled("Polygon exclusion zones rasterized onto the grid.");
    ImGui::TextDisabled("No zones configured. Use the C++ API to add polygon zones.");

    ImGui::End();
}

} // namespace idhmfis
