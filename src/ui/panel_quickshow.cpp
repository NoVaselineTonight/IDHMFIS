// IDHMFIS — Quick Show cue grid panel implementation

#include "panel_quickshow.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace idhmfis {

// Window title (stable for docking)
static constexpr const char* kWinQuickShow = "Quick Show##w";

void panel_quickshow(UIState& state, LayoutCallbacks& cbs, bool* p_open)
{
    // Enforce a sensible minimum so the grid never collapses to nothing
    ImGui::SetNextWindowSizeConstraints(ImVec2(320.f, 200.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.10f, 1.f));

    if (!ImGui::Begin(kWinQuickShow, p_open)) {
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    ImGui::PopStyleColor();

    const int pages = UIState::kQSPages;
    const int rows  = UIState::kQSRows;
    const int cols  = UIState::kQSCols;

    // ── Section header ────────────────────────────────────────────────────────
    ImGui::SeparatorText("Page");

    // ── Page selector row ─────────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.f, 2.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.f, 2.f));

    float avail_w    = ImGui::GetContentRegionAvail().x;
    float page_btn_w = (avail_w - (float)(pages - 1) * 2.f) / (float)pages;
    if (page_btn_w < 14.f) page_btn_w = 14.f;

    for (int p = 0; p < pages; ++p) {
        if (p > 0) ImGui::SameLine(0.f, 2.f);

        bool is_cur = (state.quickshow_page == p);
        if (is_cur) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.6f, 1.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 1.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.1f, 0.5f, 0.9f, 1.f));
        }

        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "%d", p + 1);
        if (ImGui::Button(lbl, ImVec2(page_btn_w, 18.f))) {
            state.quickshow_page = p;
        }
        // Tooltip on every page button
        {
            char tip[48];
            std::snprintf(tip, sizeof(tip), "Switch to page %d (6 \xc3\x97 10 cue grid)", p + 1);
            ImGui::SetItemTooltip("%s", tip);
        }

        if (is_cur)
            ImGui::PopStyleColor(3);
    }

    ImGui::PopStyleVar(2);
    ImGui::Spacing();

    // ── Cue grid section ─────────────────────────────────────────────────────
    ImGui::SeparatorText("Cue Grid");

    // Context-menu confirm-clear state
    static int ctx_page = -1, ctx_row = -1, ctx_col = -1;
    static char rename_buf[128] = {};
    static bool s_confirm_clear = false;   // two-step clear guard

    int  cur_page = state.quickshow_page;
    auto& page    = state.quickshow[cur_page];

    // Remaining height for grid
    float grid_h = ImGui::GetContentRegionAvail().y;
    float grid_w = ImGui::GetContentRegionAvail().x;

    float gap_x   = 3.f;
    float gap_y   = 3.f;
    float btn_w   = (grid_w  - (float)(cols - 1) * gap_x) / (float)cols;
    float btn_h   = (grid_h  - (float)(rows - 1) * gap_y) / (float)rows;
    if (btn_w < 40.f) btn_w = 40.f;
    if (btn_h < 24.f) btn_h = 24.f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap_x, gap_y));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 4.f));

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (c > 0) ImGui::SameLine(0.f, gap_x);

            const UIState::QuickShowSlot& slot = page[r][c];
            bool empty   = (slot.cue_idx < 0);

            char btn_id[32];
            std::snprintf(btn_id, sizeof(btn_id), "##qs_%d_%d_%d", cur_page, r, c);

            if (!empty) {
                // Colour coding: bright green=active, amber=next, neutral otherwise
                ImVec4 col_normal, col_hover, col_active;
                if (slot.active) {
                    col_normal = ImVec4(0.0f, 0.55f, 0.15f, 1.f);
                    col_hover  = ImVec4(0.0f, 0.70f, 0.20f, 1.f);
                    col_active = ImVec4(0.0f, 0.45f, 0.12f, 1.f);
                } else if (slot.is_next) {
                    col_normal = ImVec4(0.60f, 0.40f, 0.00f, 1.f);
                    col_hover  = ImVec4(0.75f, 0.50f, 0.00f, 1.f);
                    col_active = ImVec4(0.50f, 0.33f, 0.00f, 1.f);
                } else {
                    col_normal = ImVec4(0.20f, 0.20f, 0.25f, 1.f);
                    col_hover  = ImVec4(0.28f, 0.28f, 0.34f, 1.f);
                    col_active = ImVec4(0.14f, 0.14f, 0.18f, 1.f);
                }

                ImGui::PushStyleColor(ImGuiCol_Button,        col_normal);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col_hover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  col_active);

                // Truncate cue name to fit button width (approx 7 px/char)
                const char* raw = slot.cue_name.c_str();
                char display[32];
                int max_chars = (int)(btn_w / 7.f);
                if (max_chars < 2) max_chars = 2;
                if ((int)slot.cue_name.size() > max_chars) {
                    std::snprintf(display, sizeof(display), "%.*s~", max_chars - 1, raw);
                } else {
                    std::snprintf(display, sizeof(display), "%s", raw);
                }

                char full_id[64];
                std::snprintf(full_id, sizeof(full_id), "%s##%s", display, btn_id + 2);

                if (ImGui::Button(full_id, ImVec2(btn_w, btn_h))) {
                    if (cbs.on_quickshow_trigger)
                        cbs.on_quickshow_trigger(cur_page, r, c);
                }

                // Rich tooltip for assigned slots
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    const char* state_str = slot.active  ? "ACTIVE"
                                          : slot.is_next ? "NEXT"
                                                         : "idle";
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(slot.cue_name.c_str());
                    ImGui::TextDisabled("Page %d  Row %d  Col %d  |  %s",
                        cur_page + 1, r + 1, c + 1, state_str);
                    ImGui::TextDisabled("Click to trigger  |  Right-click for options");
                    ImGui::EndTooltip();
                }

                ImGui::PopStyleColor(3);
            } else {
                // Empty slot — dark grey
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.10f, 0.12f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.16f, 0.20f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.08f, 0.08f, 0.10f, 1.f));

                // Pre-capture hovered state before the button consumes it
                bool pre_hovered = ImGui::IsMouseHoveringRect(
                    ImGui::GetCursorScreenPos(),
                    ImVec2(ImGui::GetCursorScreenPos().x + btn_w,
                           ImGui::GetCursorScreenPos().y + btn_h));

                char empty_lbl[32];
                std::snprintf(empty_lbl, sizeof(empty_lbl), "%s%s",
                              pre_hovered ? "+" : " ", btn_id + 2);

                ImGui::Button(empty_lbl, ImVec2(btn_w, btn_h));

                // Tooltip on empty slots
                ImGui::SetItemTooltip("Empty slot — right-click to assign a cue");

                ImGui::PopStyleColor(3);
            }

            // Right-click context menu trigger
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                ctx_page = cur_page;
                ctx_row  = r;
                ctx_col  = c;
                s_confirm_clear = false;
                const char* sname = page[r][c].cue_name.c_str();
                std::strncpy(rename_buf, sname, sizeof(rename_buf) - 1);
                rename_buf[sizeof(rename_buf) - 1] = '\0';
                ImGui::OpenPopup("##qs_ctx");
            }
        }
    }

    ImGui::PopStyleVar(2);

    // ── Context menu ──────────────────────────────────────────────────────────
    if (ImGui::BeginPopup("##qs_ctx")) {
        // Header showing slot coordinates
        char ctx_hdr[64];
        std::snprintf(ctx_hdr, sizeof(ctx_hdr), "Page %d  Row %d  Col %d",
                      ctx_page + 1, ctx_row + 1, ctx_col + 1);
        ImGui::TextDisabled("%s", ctx_hdr);

        // Show assigned cue name if slot is occupied
        if (ctx_page >= 0 && ctx_row >= 0 && ctx_col >= 0) {
            const auto& ctx_slot = state.quickshow[ctx_page][ctx_row][ctx_col];
            if (ctx_slot.cue_idx >= 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("—  %s", ctx_slot.cue_name.c_str());
            }
        }
        ImGui::Separator();

        if (ImGui::MenuItem("Assign Cue...")) {
            // Assigns first available cue; a real implementation opens a picker.
            if (cbs.on_quickshow_assign)
                cbs.on_quickshow_assign(ctx_page, ctx_row, ctx_col, 0);
        }
        ImGui::SetItemTooltip("Load a cue into this slot from the cue library");

        // Destructive clear — two-step confirm
        bool slot_occupied = (ctx_page >= 0 && ctx_row >= 0 && ctx_col >= 0
                              && state.quickshow[ctx_page][ctx_row][ctx_col].cue_idx >= 0);
        ImGui::BeginDisabled(!slot_occupied);
        if (!s_confirm_clear) {
            if (ImGui::MenuItem("Clear Slot")) {
                s_confirm_clear = true;
            }
            ImGui::SetItemTooltip("Remove the cue assigned to this slot");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.3f, 0.3f, 1.f));
            if (ImGui::MenuItem("Confirm Clear")) {
                state.quickshow[ctx_page][ctx_row][ctx_col] = UIState::QuickShowSlot{};
                if (cbs.on_quickshow_clear)
                    cbs.on_quickshow_clear(ctx_page, ctx_row, ctx_col);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("This will remove the assigned cue — click again to confirm");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##clr")) s_confirm_clear = false;
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        // Rename inline
        ImGui::TextDisabled("Rename slot label:");
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::InputText("##qs_rename", rename_buf, sizeof(rename_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (ctx_page >= 0 && ctx_row >= 0 && ctx_col >= 0)
                state.quickshow[ctx_page][ctx_row][ctx_col].cue_name = rename_buf;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip("Press Enter to apply the new label");

        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace idhmfis
