// IDHMFIS — Macro/Executor grid panel (§Macro)
// 4 columns x 8 rows = 32 macro slots.
// Each button shows the macro name (or "M{n}" if unnamed).
// Clicking a button fires on_macro_trigger(slot_index).

#include "layout.h"
#include "theme.h"
#include "imgui.h"

#include <cstdio>
#include <string>

namespace idhmfis {

void panel_macros(UIState& state, LayoutContext& /*ctx*/, LayoutCallbacks& cbs)
{
    // Enforce a minimum size so the 4x8 grid is always readable.
    ImGui::SetNextWindowSizeConstraints(ImVec2(180.f, 200.f), ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::Begin("Macros##w")) {
        ImGui::End();
        return;
    }

    constexpr int kCols = 4;
    constexpr int kRows = 8;
    static_assert(kCols * kRows == 32, "Macro grid must be exactly 32 slots");

    // ── Header hint ───────────────────────────────────────────────────────────
    ImGui::TextDisabled("32 executor slots — click to fire  |  right-click to rename");
    ImGui::Separator();

    // Rename popup state — persistent across frames.
    static int  s_rename_slot = -1;
    static char s_rename_buf[64] = {};

    // Compute button size from available space
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // Reserve the hint row height.
    float header_h = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    avail.y -= header_h;

    float btn_w  = (avail.x - (kCols - 1) * 4.f) / static_cast<float>(kCols);
    float btn_h  = (avail.y - (kRows - 1) * 4.f) / static_cast<float>(kRows);
    // Clamp to a reasonable minimum so the buttons are always readable
    if (btn_w < 40.f) btn_w = 40.f;
    if (btn_h < 24.f) btn_h = 24.f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 4.f));

    // Subtle accent tint for all macro buttons
    ImVec4 acc = theme::accent();
    ImVec4 tint_normal  = { acc.x * 0.18f, acc.y * 0.18f, acc.z * 0.20f, 1.f };
    ImVec4 tint_hovered = { acc.x * 0.30f, acc.y * 0.30f, acc.z * 0.32f, 1.f };
    ImVec4 tint_active  = { acc.x * 0.55f, acc.y * 0.55f, acc.z * 0.60f, 1.f };

    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            int slot = row * kCols + col;

            ImGui::PushID(slot);
            ImGui::PushStyleColor(ImGuiCol_Button,        tint_normal);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tint_hovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  tint_active);

            // Label: user-defined name or "M{slot+1}"
            char label[64];
            const std::string& user_name = state.macro_names[static_cast<std::size_t>(slot)];
            if (user_name.empty()) {
                std::snprintf(label, sizeof(label), "M%d", slot + 1);
            } else {
                std::snprintf(label, sizeof(label), "%s", user_name.c_str());
            }

            if (ImGui::Button(label, ImVec2(btn_w, btn_h))) {
                if (cbs.on_macro_trigger) cbs.on_macro_trigger(slot);
            }

            ImGui::PopStyleColor(3);

            // Tooltip — always shown so empty slots are discoverable.
            if (ImGui::IsItemHovered()) {
                if (user_name.empty()) {
                    ImGui::SetTooltip(
                        "Macro slot %d (empty)\n"
                        "Right-click to assign a name.\n"
                        "Assign a macro in the cue programmer to activate this slot.",
                        slot + 1);
                } else {
                    ImGui::SetTooltip(
                        "Slot %d: %s\nLeft-click to fire this macro.",
                        slot + 1, user_name.c_str());
                }
            }

            // Right-click: open rename popup.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                s_rename_slot = slot;
                std::strncpy(s_rename_buf, user_name.c_str(), sizeof(s_rename_buf) - 1);
                s_rename_buf[sizeof(s_rename_buf) - 1] = '\0';
                ImGui::OpenPopup("##macro_rename");
            }

            ImGui::PopID();

            if (col < kCols - 1)
                ImGui::SameLine(0.f, 4.f);
        }
        // No SameLine after the last column — starts a new row automatically
    }

    ImGui::PopStyleVar();

    // ── Rename popup ──────────────────────────────────────────────────────────
    if (ImGui::BeginPopup("##macro_rename") && s_rename_slot >= 0) {
        ImGui::TextUnformatted("Rename macro slot:");
        ImGui::TextDisabled("Slot %d", s_rename_slot + 1);
        ImGui::SetNextItemWidth(180.f);
        bool commit = ImGui::InputText("##mr_input", s_rename_buf, sizeof(s_rename_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetItemTooltip("Enter a short name for this macro slot. Press Enter to confirm.");
        ImGui::SameLine();
        if (ImGui::Button("OK") || commit) {
            state.macro_names[static_cast<std::size_t>(s_rename_slot)] = s_rename_buf;
            s_rename_slot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            state.macro_names[static_cast<std::size_t>(s_rename_slot)].clear();
            s_rename_slot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip("Remove the name from this slot, reverting to 'M{n}' label.");
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace idhmfis
