// panel_connection_status.cpp — Floating connection status panel

#include "layout.h"
#include "theme.h"
#include "imgui.h"

#include <cstdio>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Helper: draw a coloured status dot (7 px) with outer glow (10 px, 25% alpha)
//  then advance the cursor
// ─────────────────────────────────────────────────────────────────────────────
static void draw_status_dot(ImDrawList* dl, ImVec2 cursor, ImU32 colour)
{
    // Dot vertically centred on current text line
    float cy = cursor.y + ImGui::GetTextLineHeight() * 0.5f;
    ImVec2 centre = {cursor.x + 8.f, cy};

    // Outer glow — same hue at 25% alpha
    ImU32 r = (colour >> IM_COL32_R_SHIFT) & 0xFF;
    ImU32 g = (colour >> IM_COL32_G_SHIFT) & 0xFF;
    ImU32 b = (colour >> IM_COL32_B_SHIFT) & 0xFF;
    ImU32 glow_col = IM_COL32(r, g, b, 64);  // ~25 % alpha
    dl->AddCircleFilled(centre, 10.f, glow_col);

    // Core dot
    dl->AddCircleFilled(centre, 7.f, colour);
}

// Dot colours
static constexpr ImU32 kGreen  = IM_COL32(0,  220,  80, 255);
static constexpr ImU32 kYellow = IM_COL32(255, 200,  0, 255);
static constexpr ImU32 kRed    = IM_COL32(220,  40, 40, 255);
static constexpr ImU32 kGrey   = IM_COL32(100, 100, 110, 255);

void panel_connection_status(UIState& state, LayoutContext& ctx)
{
    ImGui::SetNextWindowSize(ImVec2(320.f, 400.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(200.f, 200.f), ImVec2(FLT_MAX, FLT_MAX));

    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImGui::ColorConvertFloat4ToU32(theme::surface()));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(6.f, 5.f));

    if (!ImGui::Begin("Connection Status##cst",
                      &ctx.conn_status_open,
                      ImGuiWindowFlags_None))
    {
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }

    const auto& net  = state.net_config;
    const auto& out  = state.output_config;
    ImDrawList* dl   = ImGui::GetWindowDrawList();

    // ── Thin horizontal rule under the window title bar ───────────────────────
    {
        ImVec2 win_pos  = ImGui::GetWindowPos();
        float  win_w    = ImGui::GetWindowWidth();
        float  title_h  = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.f
                          + ImGui::GetStyle().WindowPadding.y;
        float  rule_y   = win_pos.y + title_h + 2.f;
        dl->AddLine(ImVec2(win_pos.x + 4.f, rule_y),
                    ImVec2(win_pos.x + win_w - 4.f, rule_y),
                    ImGui::ColorConvertFloat4ToU32(theme::kBorderSubtle), 1.f);
    }

    // ── Helper lambda: one row with alternating stripe background ─────────────
    // dot_col, label, status_str
    static int s_row_index = 0;
    s_row_index = 0;

    auto row = [&](ImU32 dot_col, const char* label, const char* status)
    {
        // Alternating stripe background
        ImVec2 row_min = ImGui::GetCursorScreenPos();
        row_min.x = ImGui::GetWindowPos().x;
        float row_h = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
        ImVec2 row_max = ImVec2(row_min.x + ImGui::GetWindowWidth(),
                                row_min.y + row_h);
        ImU32 stripe = (s_row_index % 2 == 0)
            ? IM_COL32(20, 22, 28, 255)
            : IM_COL32(16, 18, 24, 255);
        dl->AddRectFilled(row_min, row_max, stripe);
        ++s_row_index;

        ImVec2 cur = ImGui::GetCursorScreenPos();
        draw_status_dot(dl, cur, dot_col);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 22.f);

        // Label: bright if connected, dimmed if disconnected/disabled
        bool connected = (dot_col == kGreen || dot_col == kYellow);
        if (connected) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 225, 235, 255));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(100, 105, 115, 255));
        }
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
        ImGui::TextUnformatted(status);
        ImGui::PopStyleColor();
    };

    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TextUnformatted("OUTPUTS");
    ImGui::PopStyleColor();
    ImGui::Separator();

    // ── NDI ───────────────────────────────────────────────────────────────────
    {
        ImU32 col = out.ndi_enabled ? (state.ndi_streaming ? kGreen : kYellow) : kGrey;
        char buf[64];
        if (!out.ndi_enabled)
            std::snprintf(buf, sizeof(buf), "disabled");
        else if (state.ndi_streaming)
            std::snprintf(buf, sizeof(buf), "%dx%d @ %d fps",
                          out.ndi_width, out.ndi_height, out.ndi_fps);
        else
            std::snprintf(buf, sizeof(buf), "not streaming");
        row(col, "NDI", buf);
    }

    // ── ArtNet ────────────────────────────────────────────────────────────────
    {
        ImU32 col = net.artnet_enabled ? kGreen : kGrey;
        char buf[64];
        if (!net.artnet_enabled)
            std::snprintf(buf, sizeof(buf), "disabled");
        else
            std::snprintf(buf, sizeof(buf), "universe %d  port %d",
                          net.artnet_universe, net.artnet_port);
        row(col, "ArtNet", buf);
    }

    // ── sACN ──────────────────────────────────────────────────────────────────
    {
        ImU32 col = net.sacn_enabled ? kGreen : kGrey;
        char buf[64];
        if (!net.sacn_enabled)
            std::snprintf(buf, sizeof(buf), "disabled");
        else
            std::snprintf(buf, sizeof(buf), "universe %d  port %d",
                          net.sacn_universe, net.sacn_port);
        row(col, "sACN", buf);
    }

    // ── OSC ───────────────────────────────────────────────────────────────────
    {
        bool any_osc = net.osc_in_enabled || net.osc_out_enabled;
        ImU32 col = any_osc ? kGreen : kGrey;
        char buf[80];
        if (!any_osc)
            std::snprintf(buf, sizeof(buf), "disabled");
        else
        {
            char tmp[80] = {};
            int written = 0;
            if (net.osc_in_enabled)
                written = std::snprintf(tmp, sizeof(tmp),
                                        "in:%d ", net.osc_in_port);
            if (net.osc_out_enabled) {
                if (written > 0 && written < static_cast<int>(sizeof(tmp)))
                    std::snprintf(tmp + static_cast<size_t>(written),
                                  sizeof(tmp) - static_cast<size_t>(written),
                                  "out:%d", net.osc_out_port);
                else if (written <= 0)
                    std::snprintf(tmp, sizeof(tmp), "out:%d", net.osc_out_port);
            }
            std::snprintf(buf, sizeof(buf), "%s", tmp);
        }
        row(col, "OSC", buf);
    }

    // ── CITP ──────────────────────────────────────────────────────────────────
    {
        ImU32 col = net.citp_enabled ? kGreen : kGrey;
        char buf[64];
        if (!net.citp_enabled)
            std::snprintf(buf, sizeof(buf), "disabled");
        else
            std::snprintf(buf, sizeof(buf), "TCP %d  MC %d",
                          net.citp_tcp_port, net.citp_multicast_port);
        row(col, "CITP", buf);
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TextUnformatted("TIMECODE");
    ImGui::PopStyleColor();
    ImGui::Separator();

    // ── LTC / MIDI TC via timelines ───────────────────────────────────────────
    if (state.timelines.empty())
    {
        row(kGrey, "TC", "no timelines");
    }
    else
    {
        for (const auto& tl : state.timelines)
        {
            ImU32 col;
            const char* status_str;
            switch (tl.source_status)
            {
                case TimecodeSourceStatus::Active:   col = kGreen;  status_str = "Active";   break;
                case TimecodeSourceStatus::Present:  col = kYellow; status_str = "Present";  break;
                default:                             col = kGrey;   status_str = "Disabled"; break;
            }
            char buf[80];
            std::snprintf(buf, sizeof(buf), "%s  slot:%s  %s",
                          tl.name.c_str(),
                          tl.tc_slot.empty() ? "Default" : tl.tc_slot.c_str(),
                          status_str);
            row(col, "TL", buf);
        }
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TextUnformatted("MIDI");
    ImGui::PopStyleColor();
    ImGui::Separator();

    {
        // Show MIDI bindings count as proxy for MIDI connectivity
        int binding_count = static_cast<int>(state.midi_bindings.size());
        ImU32 col = binding_count > 0 ? kGreen : kGrey;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d binding%s",
                      binding_count, binding_count == 1 ? "" : "s");
        row(col, "MIDI", buf);
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TextUnformatted("DACs");
    ImGui::PopStyleColor();
    ImGui::Separator();

    if (state.available_dacs.empty())
    {
        row(kGrey, "DAC", "none discovered");
    }
    else
    {
        for (const auto& dac : state.available_dacs)
        {
            ImU32 col = dac.active ? kGreen : (dac.connected ? kYellow : kRed);
            char buf[80];
            std::snprintf(buf, sizeof(buf), "%s  %s  max %d pps",
                          dac.name.c_str(),
                          dac.active ? "active" : (dac.connected ? "connected" : "disconnected"),
                          dac.max_pps);
            row(col, "DAC", buf);
        }
    }

    // ── Overall DAC output summary ─────────────────────────────────────────────
    {
        ImU32 col = state.dac_connected ? (state.dac_pps > 0 ? kGreen : kYellow) : kGrey;
        char buf[64];
        if (!state.dac_connected)
            std::snprintf(buf, sizeof(buf), "not connected");
        else
            std::snprintf(buf, sizeof(buf), "%d pps", state.dac_pps);
        row(col, "DAC out", buf);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

} // namespace idhmfis
