// panel_timelines.cpp — Timeline list panel for the Show view.
// Shows all timelines, their TC source, playhead position, and transport controls.

#include "layout.h"
#include "theme.h"
#include "imgui.h"
#include "../timeline/timeline_types.h"
#include "../project/project.h"

#include <cstdio>
#include <string>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
static ImU32 state_color(TimelineState st, TimecodeSourceStatus ss)
{
    if (st == TimelineState::Playing)
        return ImGui::ColorConvertFloat4ToU32(theme::kGreen);
    if (st == TimelineState::Armed)
        return ImGui::ColorConvertFloat4ToU32(theme::kYellow);
    if (ss == TimecodeSourceStatus::Active)
        return ImGui::ColorConvertFloat4ToU32(theme::kGreen);
    if (ss == TimecodeSourceStatus::Present)
        return ImGui::ColorConvertFloat4ToU32(theme::kWarm);
    if (ss == TimecodeSourceStatus::Disabled)
        return ImGui::ColorConvertFloat4ToU32(theme::kRed);
    return IM_COL32(80, 80, 90, 255); // idle grey
}

static const char* state_label(TimelineState st)
{
    switch (st) {
    case TimelineState::Idle:    return "IDLE";
    case TimelineState::Armed:   return "ARMED";
    case TimelineState::Playing: return "PLAY";
    case TimelineState::Paused:  return "PAUSE";
    }
    return "?";
}

static const char* fps_label(SmpteRate fps)
{
    switch (fps) {
    case SmpteRate::Fps24:   return "24";
    case SmpteRate::Fps25:   return "25";
    case SmpteRate::Fps2997: return "29.97";
    case SmpteRate::Fps30:   return "30";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
//  panel_timelines
// ─────────────────────────────────────────────────────────────────────────────
void panel_timelines(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs)
{
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImVec4(theme::kBg0.x, theme::kBg0.y, theme::kBg0.z, 0.96f));

    if (!ImGui::Begin("Timelines##tl_list", &ctx.panel_timeline_open, flags)) {
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    // ── Toolbar ──────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Button,
        ImVec4(theme::kAccent.x * 0.3f, theme::kAccent.y * 0.3f,
               theme::kAccent.z * 0.3f, 1.f));
    bool create_new = ImGui::Button("+ New Timeline");
    ImGui::PopStyleColor();

    if (create_new && cbs.on_timeline_create) {
        TimelineDef def;
        def.id        = Project::new_id();
        def.name      = "Timeline " + std::to_string((int)state.timelines.size() + 1);
        def.fps       = SmpteRate::Fps25;
        def.link_mode = true;
        def.tc_slot   = "Default";
        cbs.on_timeline_create(std::move(def));
    }

    // TC monitor — show position of selected/first playing timeline
    // Use a slightly larger font scale to make the TC counter stand out.
    ImGui::SameLine(0, 12);
    {
        const UIState::TimelineInfo* active_tl = nullptr;
        for (const auto& tl : state.timelines) {
            if (tl.state == TimelineState::Playing) { active_tl = &tl; break; }
        }
        if (!active_tl && !state.timelines.empty())
            active_tl = &state.timelines[0];

        ImGui::SetWindowFontScale(1.1f);
        if (active_tl) {
            std::string tc_str = tc_to_string(active_tl->position_frames, active_tl->fps);
            ImGui::TextDisabled("TC: %s", tc_str.c_str());
        } else {
            ImGui::TextDisabled("TC: --:--:--:--");
        }
        ImGui::SetWindowFontScale(1.0f);
    }

    ImGui::Separator();

    // ── Timeline list ─────────────────────────────────────────────────────────
    float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##tl_scroll", ImVec2(0.f, avail_h), ImGuiChildFlags_Borders);

    static char rename_buf[256] = {};
    static int  rename_idx      = -1;
    // Deferred rename-popup open: set inside the context menu, acted on after EndPopup.
    bool open_rename_popup = false;

    // BUG #73: reset rename state when the timeline list changes (e.g. project
    // reload), to prevent a stale rename_idx pointing at a deleted timeline.
    {
        static int s_last_timeline_count = 0;
        int current_count = static_cast<int>(state.timelines.size());
        if (current_count != s_last_timeline_count) {
            rename_idx = -1;
            rename_buf[0] = '\0';
            s_last_timeline_count = current_count;
        }
    }

    // Row shading colours — alternating dark bands
    static constexpr ImU32 kRowEven = IM_COL32(20, 22, 28, 255);
    static constexpr ImU32 kRowOdd  = IM_COL32(17, 19, 25, 255);

    // Border stripe colours
    static const ImU32 kStripeGreen = IM_COL32(47, 204, 113, 255);   // kGreen as U32
    static const ImU32 kStripeAmber = IM_COL32(243, 156, 18, 255);   // kYellow as U32

    // Dark surface colour for transport buttons (normal state)
    static constexpr ImVec4 kBtnSurface      = { 35.f / 255.f, 38.f / 255.f, 45.f / 255.f, 1.f };
    static constexpr ImVec4 kBtnSurfaceHover = { 48.f / 255.f, 52.f / 255.f, 62.f / 255.f, 1.f };
    static constexpr ImVec4 kBtnSurfaceActive= { 55.f / 255.f, 60.f / 255.f, 72.f / 255.f, 1.f };

    for (int i = 0; i < static_cast<int>(state.timelines.size()); ++i) {
        const UIState::TimelineInfo& tl = state.timelines[static_cast<size_t>(i)];

        ImGui::PushID(i);

        // ── Alternate row background ─────────────────────────────────────────
        {
            ImVec2 row_min = ImGui::GetCursorScreenPos();
            // Row height: text line height + item spacing
            float row_h = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y * 2.f + 6.f;
            ImVec2 row_max = ImVec2(
                row_min.x + ImGui::GetContentRegionAvail().x,
                row_min.y + row_h);
            ImU32 row_col = (i % 2 == 0) ? kRowEven : kRowOdd;
            ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max, row_col);

            // State border stripe (4px on left edge)
            if (tl.state == TimelineState::Playing) {
                ImGui::GetWindowDrawList()->AddRectFilled(
                    row_min,
                    ImVec2(row_min.x + 4.f, row_max.y),
                    kStripeGreen);
            } else if (tl.state == TimelineState::Armed) {
                ImGui::GetWindowDrawList()->AddRectFilled(
                    row_min,
                    ImVec2(row_min.x + 4.f, row_max.y),
                    kStripeAmber);
            }
        }

        // Status indicator dot — 6px radius with 9px glow at 25% alpha
        ImU32 dot_col = state_color(tl.state, tl.source_status);
        ImVec2 dot_pos = ImGui::GetCursorScreenPos();
        dot_pos.x += 8.f;
        dot_pos.y += ImGui::GetTextLineHeight() * 0.55f;
        // Glow ring (25% alpha)
        ImU32 glow_col = (dot_col & 0x00FFFFFFu) | 0x40000000u; // replace alpha with ~64 (25%)
        ImGui::GetWindowDrawList()->AddCircleFilled(dot_pos, 9.f, glow_col);
        // Solid dot
        ImGui::GetWindowDrawList()->AddCircleFilled(dot_pos, 6.f, dot_col);
        ImGui::Dummy(ImVec2(20.f, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0, 4);

        // Selectable row
        bool selected = (ctx.timeline_view_id == tl.id);
        if (ImGui::Selectable(("##tlrow_" + tl.id).c_str(), selected,
                              ImGuiSelectableFlags_SpanAllColumns,
                              ImVec2(0.f, 0.f)))
        {
            ctx.timeline_view_id = tl.id;
            state.selected_timeline_idx = i;
        }

        // Double-click opens in editor
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            ctx.timeline_view_id = tl.id;

        ImGui::SameLine(0, 0);

        // Name
        ImGui::Text("%-20s", tl.name.c_str());
        ImGui::SameLine(0, 8);

        // FPS + slot
        ImGui::TextDisabled("[%sfps / %s]", fps_label(tl.fps), tl.tc_slot.c_str());
        ImGui::SameLine(0, 8);

        // Position
        std::string pos_str = tc_to_string(tl.position_frames, tl.fps);
        ImGui::TextColored(
            tl.state == TimelineState::Playing ? theme::kGreen : theme::kAccent,
            "%s", pos_str.c_str());

        ImGui::SameLine(0, 8);

        // State badge — text colour matches the dot colour.
        // IM_COL32 layout: A@24, R@16, G@8, B@0 (see imgui.h IM_COL32_*_SHIFT).
        {
            ImU32 badge_col = state_color(tl.state, tl.source_status);
            float r = ((badge_col >> 16) & 0xFF) / 255.f;
            float g = ((badge_col >>  8) & 0xFF) / 255.f;
            float b = ((badge_col >>  0) & 0xFF) / 255.f;
            float a = ((badge_col >> 24) & 0xFF) / 255.f;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(r, g, b, a));
            ImGui::Text("[%s]", state_label(tl.state));
            ImGui::PopStyleColor();
        }

        // Transport buttons — sized for live operation
        ImGui::SameLine(0, 6);
        static constexpr ImVec2 kBtn = { 28.f, 22.f };

        // Shared button style: dark surface + consistent rounding/padding
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 3.f));
        ImGui::PushStyleColor(ImGuiCol_Button,        kBtnSurface);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBtnSurfaceHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kBtnSurfaceActive);

        // ARM (TC chase armed)
        bool armed = (tl.state == TimelineState::Armed);
        if (armed) {
            // Override button colour to amber when armed
            ImGui::PushStyleColor(ImGuiCol_Button,        theme::kYellow);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(theme::kYellow.x * 1.15f, theme::kYellow.y * 1.15f,
                       theme::kYellow.z * 1.15f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::kYellow);
        }
        if (ImGui::Button("ARM##arm", kBtn) && cbs.on_timeline_set_armed)
            cbs.on_timeline_set_armed(tl.id, !armed);
        if (armed) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Arm for TC chase");

        ImGui::SameLine(0, 2);

        // PLAY
        bool is_play = (tl.state == TimelineState::Playing);
        if (is_play) {
            ImGui::PushStyleColor(ImGuiCol_Button,        theme::kGreen);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(theme::kGreen.x * 1.15f, theme::kGreen.y * 1.15f,
                       theme::kGreen.z * 1.15f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::kGreen);
        }
        if (ImGui::Button(">##play", kBtn) && cbs.on_timeline_play)
            cbs.on_timeline_play(tl.id);
        if (is_play) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Play");
        ImGui::SameLine(0, 2);

        // PAUSE
        bool is_pause = (tl.state == TimelineState::Paused);
        if (is_pause) {
            ImGui::PushStyleColor(ImGuiCol_Button,        theme::kYellow);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(theme::kYellow.x * 1.15f, theme::kYellow.y * 1.15f,
                       theme::kYellow.z * 1.15f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::kYellow);
        }
        if (ImGui::Button("||##pau", kBtn) && cbs.on_timeline_pause)
            cbs.on_timeline_pause(tl.id);
        if (is_pause) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Pause");
        ImGui::SameLine(0, 2);

        // STOP
        if (ImGui::Button("[]##stp", kBtn) && cbs.on_timeline_stop)
            cbs.on_timeline_stop(tl.id);
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Stop and reset");
        ImGui::SameLine(0, 2);

        // REWIND
        if (ImGui::Button("|<##rew", kBtn) && cbs.on_timeline_rewind)
            cbs.on_timeline_rewind(tl.id);
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Rewind to start");

        // Pop shared surface style (3 colours + 2 vars)
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        ImGui::SameLine(0, 4);

        // EDIT button — accent-coloured outline style (transparent bg, cyan border)
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(4.f, 3.f));
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::kAccentHot);
        ImGui::PushStyleColor(ImGuiCol_Border,        theme::kAccent);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);

        if (ImGui::Button("EDIT##ed", { 40.f, 22.f })) {
            ctx.timeline_view_id   = tl.id;
            ctx.timeline_view_open = true;
            state.selected_timeline_idx = i;
        }
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Open in Timeline Editor");

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);

        // Right-click context menu
        if (ImGui::BeginPopupContextItem("##tl_ctx")) {
            if (ImGui::MenuItem("Open in Editor"))
                ctx.timeline_view_id = tl.id;

            if (ImGui::MenuItem("Rename")) {
                rename_idx = i;
                std::snprintf(rename_buf, sizeof(rename_buf), "%s", tl.name.c_str());
                // Do NOT call ImGui::OpenPopup here — we are inside a popup stack.
                // Set a flag and open after EndPopup so the popup is registered in
                // the correct (window-level) ID stack.
                open_rename_popup = true;
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Delete") && cbs.on_timeline_delete)
                cbs.on_timeline_delete(tl.id);

            ImGui::EndPopup();
        }

        // Deferred open: must happen outside the context-menu popup stack,
        // but still inside PushID(i) so the popup ID resolves correctly.
        if (open_rename_popup && rename_idx == i)
            ImGui::OpenPopup("RenameTimeline");

        // Rename popup
        if (rename_idx == i && ImGui::BeginPopup("RenameTimeline")) {
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::InputText("Name##ren", rename_buf, sizeof(rename_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (cbs.on_timeline_rename)
                    cbs.on_timeline_rename(tl.id, rename_buf);
                rename_idx = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    if (state.timelines.empty()) {
        // Centre the placeholder text both horizontally and vertically.
        const char* msg = "No timelines. Click '+ New Timeline' to create one.";
        ImVec2 child_size = ImGui::GetContentRegionAvail();
        ImVec2 text_size  = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPosX((child_size.x - text_size.x) * 0.5f);
        ImGui::SetCursorPosY((child_size.y - text_size.y) * 0.5f);
        ImGui::TextDisabled("%s", msg);
    }

    ImGui::EndChild();

    ImGui::PopStyleColor();
    ImGui::End();
}

} // namespace idhmfis
