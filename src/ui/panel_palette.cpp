// panel_palette.cpp — Color and Position palette panels.
// Includes MOVE mode (drag-and-drop rearrange) for the color palette grid.

#include "layout.h"
#include "imgui.h"
#include "../project/palette.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace idhmfis {

static constexpr const char* kWinPalette = "Palette##w";

void panel_palette(UIState& state, LayoutCallbacks& cbs)
{
    // Enforce minimum window size so swatches are never clipped.
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.f, 280.f), ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::Begin(kWinPalette)) {
        ImGui::End();
        return;
    }

    // ── Color Palette (adjustable grid) ─────────────────────────────────────
    static int s_pal_cols = 6;
    static int s_pal_rows = 4;   // visible rows (ColorPalette::kSlots = 24 = 6x4)

    ImGui::SeparatorText("Color Palette");
    {
        static bool s_pal_auto = false;
        if (ImGui::SmallButton("[=]##pal_cfg")) ImGui::OpenPopup("##pal_settings");
        ImGui::SetItemTooltip("Color palette grid settings");
        ImGui::SameLine(0, 8.f);
        ImGui::TextDisabled("Left-click to select  |  Right-click to rename");
        ImGui::SameLine(0, 12.f);
        ImGui::Checkbox("Auto##palAuto", &s_pal_auto);
        ImGui::SetItemTooltip("Auto-fit columns to window width");
        if (ImGui::BeginPopup("##pal_settings")) {
            ImGui::TextUnformatted("Palette Grid");
            ImGui::Separator();
            ImGui::SetNextItemWidth(120.f);
            ImGui::SliderInt("Columns##palc", &s_pal_cols, 1, 12, "%d",
                             ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetNextItemWidth(120.f);
            ImGui::SliderInt("Rows##palr", &s_pal_rows, 1, 12, "%d",
                             ImGuiSliderFlags_AlwaysClamp);
            ImGui::TextDisabled("(%d slots total)", s_pal_rows * s_pal_cols);
            ImGui::EndPopup();
        }

        // Auto column computation — runs after popup so avail width is current
        if (s_pal_auto) {
            const float avail_auto = ImGui::GetContentRegionAvail().x;
            const float cell_gap_auto = 4.f;
            const float target = 52.f;
            int auto_cols = std::max(1, static_cast<int>(
                (avail_auto + cell_gap_auto) / (target + cell_gap_auto)));
            s_pal_cols = std::clamp(auto_cols, 1, ColorPalette::kSlots);
        }
    }
    ImGui::Spacing();

    // ── MOVE toggle for color palette ──────────────────────────────────────────
    static bool s_color_move_mode = false;
    {
        if (s_color_move_mode) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(0.10f, 0.45f, 0.80f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(0.15f, 0.55f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(0.08f, 0.35f, 0.65f, 1.0f));
        }
        if (ImGui::SmallButton(s_color_move_mode ? "MOVE [ON]##cmv" : "MOVE##cmv")) {
            s_color_move_mode = !s_color_move_mode;
        }
        if (s_color_move_mode)
            ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip(
            s_color_move_mode
            ? "MOVE mode ON — drag color swatches to rearrange. Click to deactivate."
            : "Activate MOVE mode to drag-and-drop color swatches into a different order.");
    }
    ImGui::Spacing();

    // Compute square swatch size from available width and height
    const float avail_pw   = ImGui::GetContentRegionAvail().x;
    const float avail_ph   = ImGui::GetContentRegionAvail().y;
    const float cell_gap   = 4.f;
    // Max size derived from width
    float sq_from_w = (avail_pw - cell_gap * (static_cast<float>(s_pal_cols) - 1.f))
                    / static_cast<float>(s_pal_cols);
    // Max size derived from height (reserve ~80px for header buttons above and below)
    float sq_from_h = (avail_ph - 80.f - cell_gap * (static_cast<float>(s_pal_rows) - 1.f))
                    / static_cast<float>(s_pal_rows);
    // Use the smaller of the two so swatches fit in both dimensions; clamp 18..96
    const float sq_side    = std::min(96.f, std::max(18.f, std::min(sq_from_w, sq_from_h)));
    const ImVec2 swatch_size(sq_side, sq_side);
    const int    kCols = s_pal_cols;

    // Rename popup state
    static int    s_rename_slot = -1;
    static char   s_rename_buf[64] = {};

    bool any_color = false;
    for (int i = 0; i < ColorPalette::kSlots; ++i) {
        const auto& slot = state.color_palette.slots[static_cast<size_t>(i)];
        const LaserColor& c = slot.color;
        if (c.r > 0.01f || c.g > 0.01f || c.b > 0.01f) { any_color = true; break; }
    }
    if (!any_color) {
        ImGui::TextDisabled("All color slots are empty (black).");
        ImGui::TextDisabled("Right-click a swatch to rename it.");
        ImGui::Spacing();
    }

    for (int i = 0; i < ColorPalette::kSlots; ++i) {
        if (i % kCols != 0) ImGui::SameLine(0.f, 4.f);

        const auto& slot = state.color_palette.slots[static_cast<size_t>(i)];
        const LaserColor& c = slot.color;

        bool selected = (state.active_color_slot == i);

        // Highlight selected slot with a yellow border (non-move mode only)
        if (selected && !s_color_move_mode) {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 0.f, 1.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.f);
        }

        // In MOVE mode, show a subtle drag-ready tint over the swatch color
        ImVec4 btn_color(c.r, c.g, c.b, 1.f);
        if (s_color_move_mode) {
            // Mix swatch color with a blue tint to signal draggability
            btn_color = ImVec4(c.r * 0.7f + 0.05f,
                               c.g * 0.7f + 0.05f,
                               c.b * 0.7f + 0.20f, 1.f);
        }
        ImGui::PushStyleColor(ImGuiCol_Button, btn_color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(
            std::min(c.r + 0.2f, 1.f),
            std::min(c.g + 0.2f, 1.f),
            std::min(c.b + 0.2f, 1.f), 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(
            std::min(c.r + 0.35f, 1.f),
            std::min(c.g + 0.35f, 1.f),
            std::min(c.b + 0.35f, 1.f), 1.f));

        char id[16];
        std::snprintf(id, sizeof(id), "##cs%d", i);
        if (ImGui::Button(id, swatch_size)) {
            if (!s_color_move_mode) {
                // Left-click: select (only outside move mode)
                state.active_color_slot = i;
                if (cbs.on_color_select) cbs.on_color_select(i);
            }
        }
        ImGui::PopStyleColor(3);

        if (selected && !s_color_move_mode) {
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        // ── Drag-and-drop (MOVE mode only) ─────────────────────────────────────
        if (s_color_move_mode) {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("COLOR_CELL", &i, sizeof(int));
                // Preview: show the actual color in the drag ghost
                ImVec2 preview_sz(28.f, 20.f);
                ImGui::ColorButton("##drag_preview",
                    ImVec4(c.r, c.g, c.b, 1.f),
                    ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoPicker,
                    preview_sz);
                if (!slot.name.empty())
                    ImGui::SameLine(); ImGui::Text("%s", slot.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("COLOR_CELL");
                if (payload) {
                    int src = *static_cast<const int*>(payload->Data);
                    if (src != i &&
                        src >= 0 && src < ColorPalette::kSlots) {
                        std::swap(
                            state.color_palette.slots[static_cast<size_t>(src)],
                            state.color_palette.slots[static_cast<size_t>(i)]);
                        // If the active slot was one of the two swapped, update it
                        if (state.active_color_slot == src)
                            state.active_color_slot = i;
                        else if (state.active_color_slot == i)
                            state.active_color_slot = src;
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        // Tooltip — always shown (slot name or slot number if unnamed)
        if (ImGui::IsItemHovered()) {
            if (s_color_move_mode) {
                ImGui::SetTooltip("Drag to rearrange color slots.");
            } else if (!slot.name.empty()) {
                ImGui::SetTooltip(
                    "Slot %d: %s\nR=%.2f G=%.2f B=%.2f\n"
                    "Left-click to apply. Right-click to rename.",
                    i + 1, slot.name.c_str(), c.r, c.g, c.b);
            } else {
                ImGui::SetTooltip(
                    "Slot %d (unnamed)\nR=%.2f G=%.2f B=%.2f\n"
                    "Left-click to apply. Right-click to rename.",
                    i + 1, c.r, c.g, c.b);
            }
        }

        // Right-click: rename popup (only outside move mode)
        if (!s_color_move_mode && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            s_rename_slot = i;
            std::strncpy(s_rename_buf, slot.name.c_str(), sizeof(s_rename_buf) - 1);
            s_rename_buf[sizeof(s_rename_buf) - 1] = '\0';
            ImGui::OpenPopup("##color_rename");
        }
    }

    // Rename popup
    if (ImGui::BeginPopup("##color_rename") && s_rename_slot >= 0) {
        ImGui::TextUnformatted("Rename color slot:");
        ImGui::TextDisabled("Slot %d", s_rename_slot + 1);
        ImGui::SetNextItemWidth(160.f);
        bool commit = ImGui::InputText("##cr_input", s_rename_buf, sizeof(s_rename_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetItemTooltip("Enter a name for this color swatch. Press Enter to confirm.");
        ImGui::SameLine();
        if (ImGui::Button("OK") || commit) {
            state.color_palette.slots[static_cast<size_t>(s_rename_slot)].name = s_rename_buf;
            s_rename_slot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Active color display
    ImGui::Spacing();
    if (state.active_color_slot >= 0 && state.active_color_slot < ColorPalette::kSlots) {
        const auto& active_slot = state.color_palette.slots[static_cast<size_t>(state.active_color_slot)];
        const LaserColor& ac = active_slot.color;
        // Inline color preview swatch next to the status line.
        ImVec2 p = ImGui::GetCursorScreenPos();
        float  sh = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(
            p, ImVec2(p.x + sh * 1.4f, p.y + sh),
            IM_COL32(
                static_cast<int>(ac.r * 255),
                static_cast<int>(ac.g * 255),
                static_cast<int>(ac.b * 255), 255));
        ImGui::Dummy(ImVec2(sh * 1.4f + 4.f, sh));
        ImGui::SameLine(0, 4);
        if (!active_slot.name.empty()) {
            ImGui::TextDisabled("Active: %s  (slot %d)",
                                active_slot.name.c_str(), state.active_color_slot + 1);
        } else {
            ImGui::TextDisabled("Active: slot %d  (unnamed)", state.active_color_slot + 1);
        }
    } else {
        ImGui::TextDisabled("No color selected.");
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ── Position Palette (5x5 grid) ──────────────────────────────────────────
    ImGui::SeparatorText("Position Palette");
    ImGui::TextDisabled("Left-click to recall position  |  Right-click to rename");
    ImGui::Spacing();

    // Compute square position swatch size from available width
    const float avail_pw_pos  = ImGui::GetContentRegionAvail().x;
    const float cell_gap_pos  = 4.f;
    float sq_side_pos = (avail_pw_pos - cell_gap_pos * (static_cast<float>(PositionPalette::kCols) - 1.f))
                      / static_cast<float>(PositionPalette::kCols);
    sq_side_pos = std::min(96.f, std::max(18.f, sq_side_pos));
    const ImVec2 pos_btn_size(sq_side_pos, sq_side_pos);

    // Empty-state check
    bool any_pos = false;
    for (int i = 0; i < PositionPalette::kSlots; ++i) {
        if (!state.position_palette.slots[static_cast<size_t>(i)].name.empty()) {
            any_pos = true; break;
        }
    }
    if (!any_pos) {
        ImGui::TextDisabled("All position slots are unnamed.");
        ImGui::TextDisabled("Right-click a slot to name it.");
        ImGui::Spacing();
    }

    // Position slot rename popup state
    static int  s_pos_rename_slot = -1;
    static char s_pos_rename_buf[64] = {};

    for (int row = 0; row < PositionPalette::kRows; ++row) {
        for (int col = 0; col < PositionPalette::kCols; ++col) {
            if (col > 0) ImGui::SameLine(0.f, 4.f);

            int idx = row * PositionPalette::kCols + col;
            const auto& slot = state.position_palette.slots[static_cast<size_t>(idx)];
            bool pos_selected = (state.active_position_slot == idx);

            if (pos_selected) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImVec4(0.15f, 0.55f, 0.95f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(0.25f, 0.65f, 1.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    ImVec4(0.10f, 0.40f, 0.75f, 1.f));
            }

            char pos_label[32];
            // Abbreviate the name to fit in the button; show slot number if unnamed
            if (!slot.name.empty()) {
                std::snprintf(pos_label, sizeof(pos_label), "%.8s", slot.name.c_str());
            } else {
                std::snprintf(pos_label, sizeof(pos_label), "P%d", idx + 1);
            }

            char pos_id[32];
            std::snprintf(pos_id, sizeof(pos_id), "%s##pp%d", pos_label, idx);

            if (ImGui::Button(pos_id, pos_btn_size)) {
                state.active_position_slot = idx;
                if (cbs.on_position_select) cbs.on_position_select(idx);
            }

            if (pos_selected)
                ImGui::PopStyleColor(3);

            if (ImGui::IsItemHovered()) {
                if (!slot.name.empty()) {
                    ImGui::SetTooltip(
                        "Slot %d: %s\n"
                        "X=%.3f  Y=%.3f  Scale=%.2f  Rot=%.0f deg\n"
                        "Left-click to apply. Right-click to rename.",
                        idx + 1, slot.name.c_str(),
                        slot.x, slot.y, slot.scale,
                        slot.rotation * 57.2957795f);
                } else {
                    ImGui::SetTooltip(
                        "Slot %d (unnamed)\n"
                        "X=%.3f  Y=%.3f  Scale=%.2f  Rot=%.0f deg\n"
                        "Left-click to apply. Right-click to rename.",
                        idx + 1,
                        slot.x, slot.y, slot.scale,
                        slot.rotation * 57.2957795f);
                }
            }

            // Right-click: rename popup
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                s_pos_rename_slot = idx;
                std::strncpy(s_pos_rename_buf, slot.name.c_str(),
                             sizeof(s_pos_rename_buf) - 1);
                s_pos_rename_buf[sizeof(s_pos_rename_buf) - 1] = '\0';
                ImGui::OpenPopup("##pos_rename");
            }
        }
    }

    // Position rename popup
    if (ImGui::BeginPopup("##pos_rename") && s_pos_rename_slot >= 0) {
        ImGui::TextUnformatted("Rename position slot:");
        ImGui::TextDisabled("Slot %d", s_pos_rename_slot + 1);
        ImGui::SetNextItemWidth(160.f);
        bool commit = ImGui::InputText("##pr_input", s_pos_rename_buf,
                                       sizeof(s_pos_rename_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetItemTooltip("Enter a name for this position slot. Press Enter to confirm.");
        ImGui::SameLine();
        if (ImGui::Button("OK") || commit) {
            state.position_palette.slots[static_cast<size_t>(s_pos_rename_slot)].name =
                s_pos_rename_buf;
            s_pos_rename_slot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    if (state.active_position_slot >= 0 &&
        state.active_position_slot < PositionPalette::kSlots) {
        const auto& ps = state.position_palette.slots[
            static_cast<size_t>(state.active_position_slot)];
        if (!ps.name.empty()) {
            ImGui::TextDisabled("Active: %s  (%.3f, %.3f)", ps.name.c_str(), ps.x, ps.y);
        } else {
            ImGui::TextDisabled("Active: slot %d  (%.3f, %.3f)",
                                state.active_position_slot + 1, ps.x, ps.y);
        }
    } else {
        ImGui::TextDisabled("No position selected.");
    }

    ImGui::End();
}

} // namespace idhmfis
