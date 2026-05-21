// panel_clock.cpp — Floating clock / timecode panel

#include "layout.h"
#include "theme.h"
#include "imgui.h"
#include "../timeline/timeline_types.h"

#include <ctime>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace idhmfis {

void panel_clock(UIState& state, LayoutContext& ctx, LayoutCallbacks& /*cbs*/)
{
    ImGui::SetNextWindowSize(ImVec2(260.f, 100.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(180.f, 80.f), ImVec2(FLT_MAX, FLT_MAX));

    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        IM_COL32(10, 12, 16, 245));

    if (!ImGui::Begin("Clock##clk",
                      &ctx.clock_open,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImGui::End();
        ImGui::PopStyleColor(); // WindowBg
        return;
    }

    // ── Mode selector (pill-shaped tab bar) ───────────────────────────────────
    static int s_clock_mode = 0; // 0=Wall, 1=Timeline TC (LTC/MTC slot), 2=MIDI TC

    const char* mode_labels[3] = { "CLOCK", "TC", "MIDI TC" };
    ImVec4 text_dim  = theme::text_secondary();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);

    for (int m = 0; m < 3; ++m)
    {
        if (m > 0) ImGui::SameLine();
        if (s_clock_mode == m)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0, 200, 230, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 200, 230, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(0, 200, 230, 255));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.f, 0.f, 0.f, 1.f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(30, 33, 40, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 44, 52, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(30, 33, 40, 255));
            ImGui::PushStyleColor(ImGuiCol_Text,          text_dim);
        }
        if (ImGui::Button(mode_labels[m], ImVec2(70.f, 24.f)))
            s_clock_mode = m;
        ImGui::PopStyleColor(4);
    }

    ImGui::PopStyleVar(); // FrameRounding

    ImGui::Separator();

    // ── Time display ──────────────────────────────────────────────────────────
    char time_buf[64] = "00:00:00.000";
    bool show_status_dot = false;
    bool status_active   = false;

    if (s_clock_mode == 0)
    {
        // Wall clock — system time HH:MM:SS.ms
        using namespace std::chrono;
        auto now   = system_clock::now();
        auto epoch = now.time_since_epoch();
        auto secs  = duration_cast<seconds>(epoch);
        auto ms    = duration_cast<milliseconds>(epoch) - duration_cast<milliseconds>(secs);
        std::time_t t = system_clock::to_time_t(now);
        struct tm tm_buf{};
        localtime_s(&tm_buf, &t);
        std::snprintf(time_buf, sizeof(time_buf),
                      "%02d:%02d:%02d.%03d",
                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                      static_cast<int>(ms.count()));
    }
    else if (s_clock_mode == 1)
    {
        // Timeline TC — show position of the selected (or first active) timeline
        show_status_dot = true;
        const UIState::TimelineInfo* tl = nullptr;
        if (state.selected_timeline_idx >= 0 &&
            state.selected_timeline_idx < static_cast<int>(state.timelines.size()))
        {
            tl = &state.timelines[static_cast<size_t>(state.selected_timeline_idx)];
        }
        else if (!state.timelines.empty())
        {
            tl = &state.timelines[0];
        }

        if (tl)
        {
            status_active = (tl->source_status == TimecodeSourceStatus::Active);
            std::string tc = tc_to_string(tl->position_frames, tl->fps);
            std::snprintf(time_buf, sizeof(time_buf), "%s", tc.c_str());
        }
        else
        {
            std::snprintf(time_buf, sizeof(time_buf), "--:--:--:--");
        }
    }
    else // s_clock_mode == 2
    {
        // MIDI TC — find first timeline using MTC source slot
        show_status_dot = true;
        const UIState::TimelineInfo* tl = nullptr;
        for (const auto& t : state.timelines)
        {
            if (!t.tc_slot.empty() && t.tc_slot != "Default")
            {
                tl = &t;
                break;
            }
        }
        if (!tl && !state.timelines.empty())
            tl = &state.timelines[0];

        if (tl)
        {
            status_active = (tl->source_status == TimecodeSourceStatus::Active);
            std::string tc = tc_to_string(tl->position_frames, tl->fps);
            std::snprintf(time_buf, sizeof(time_buf), "%s", tc.c_str());
        }
        else
        {
            std::snprintf(time_buf, sizeof(time_buf), "--:--:--:--");
        }
    }

    // Draw status dot if needed (8px dot + 12px glow at 20% alpha)
    if (show_status_dot)
    {
        ImVec2 dot_pos = ImGui::GetCursorScreenPos();
        dot_pos.x += 4.f;
        dot_pos.y += ImGui::GetTextLineHeight() * 0.5f;
        ImU32 dot_col  = status_active
                         ? IM_COL32(0, 220, 80, 255)
                         : IM_COL32(220, 40, 40, 255);
        ImU32 glow_col = status_active
                         ? IM_COL32(0, 220, 80, 51)   // 20% alpha ≈ 51
                         : IM_COL32(220, 40, 40, 51);
        ImDrawList* dl_dot = ImGui::GetWindowDrawList();
        dl_dot->AddCircleFilled(dot_pos, 12.f, glow_col); // glow ring
        dl_dot->AddCircleFilled(dot_pos, 4.f,  dot_col);  // solid 8px dot (r=4)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.f);
    }

    // ── Large time display — split on ':' to colour colons in accent ──────────
    // Accent color: IM_COL32(0,200,230,255); colon color: IM_COL32(0,200,230,200)
    ImGui::SetWindowFontScale(2.2f);
    {
        ImDrawList* dl_time = ImGui::GetWindowDrawList();
        ImVec2      draw_pos = ImGui::GetCursorScreenPos();
        ImFont*     font     = ImGui::GetFont();
        float       fs       = ImGui::GetFontSize(); // already scaled by 2.2

        ImU32 col_digit  = IM_COL32(0, 200, 230, 255);
        ImU32 col_colon  = IM_COL32(0, 200, 230, 200);

        // Walk each character, switching color on ':' or '.'
        float cursor_x = draw_pos.x;
        float cursor_y = draw_pos.y;
        const char* p  = time_buf;
        while (*p)
        {
            char ch[2] = { *p, '\0' };
            ImU32 ch_col = ((*p == ':') || (*p == '.')) ? col_colon : col_digit;
            dl_time->AddText(font, fs, ImVec2(cursor_x, cursor_y), ch_col, ch);
            cursor_x += font->CalcTextSizeA(fs, FLT_MAX, 0.f, ch).x;
            ++p;
        }

        // Advance ImGui cursor by the text height so layout is correct
        float text_w = font->CalcTextSizeA(fs, FLT_MAX, 0.f, time_buf).x;
        ImGui::Dummy(ImVec2(text_w, fs));
    }
    ImGui::SetWindowFontScale(1.0f);

    // ── Thin accent-colored bottom border line via DrawList ───────────────────
    {
        ImDrawList* dl_line = ImGui::GetWindowDrawList();
        ImVec2 win_pos  = ImGui::GetWindowPos();
        ImVec2 win_size = ImGui::GetWindowSize();
        float  line_y   = win_pos.y + win_size.y - 2.f;
        dl_line->AddLine(
            ImVec2(win_pos.x,                 line_y),
            ImVec2(win_pos.x + win_size.x,    line_y),
            IM_COL32(0, 200, 230, 180),
            1.5f);
    }

    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg
}

} // namespace idhmfis
