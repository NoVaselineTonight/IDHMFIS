// panel_setup.cpp — Setup configuration panel for IDHMFIS.
// panel_setup_content(): renders all tabs — call from any host window.
// panel_setup_window():  floating popup variant (File > Setup... menu).

#include "imgui.h"
#include "panel_setup.h"
#include "theme.h"
#include "../version.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iphlpapi.h>
#include <winsock2.h>
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  enumerate_network_interfaces — returns non-loopback IPv4 adapters as
//  { adapter_name, ip_string } pairs. Windows implementation uses GetAdaptersInfo.
// ─────────────────────────────────────────────────────────────────────────────
#pragma warning(suppress: 4505)
static std::vector<std::pair<std::string, std::string>> enumerate_network_interfaces()
{
    std::vector<std::pair<std::string, std::string>> result;
#if defined(_WIN32)
    ULONG buf_sz = sizeof(IP_ADAPTER_INFO) * 16;
    std::vector<char> buf(static_cast<size_t>(buf_sz));
    IP_ADAPTER_INFO* info = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
    DWORD ret = GetAdaptersInfo(info, &buf_sz);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(static_cast<size_t>(buf_sz));
        info = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
        ret  = GetAdaptersInfo(info, &buf_sz);
    }
    if (ret == NO_ERROR) {
        for (IP_ADAPTER_INFO* a = info; a != nullptr; a = a->Next) {
            std::string ip = a->IpAddressList.IpAddress.String;
            // Skip loopback and unassigned (0.0.0.0)
            if (ip == "127.0.0.1" || ip == "0.0.0.0") continue;
            result.push_back({ std::string(a->Description), ip });
        }
    }
#endif
    if (result.empty())
        result.push_back({ "(none detected)", "0.0.0.0" });
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  draw_group_border — draw a thin kBorderSubtle rect around the last group.
//  Call after EndGroup(). p_min/p_max must be captured before BeginGroup content.
// ─────────────────────────────────────────────────────────────────────────────
static void draw_group_border(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max)
{
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(theme::kBorderSubtle);
    dl->AddRect(p_min, p_max, col, theme::kSmallRounding);
}

// ─────────────────────────────────────────────────────────────────────────────
//  draw_stream_dot — 6 px status dot before a stream checkbox label.
// ─────────────────────────────────────────────────────────────────────────────
static void draw_stream_dot(bool active)
{
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      pos = ImGui::GetCursorScreenPos();
    float       cy  = pos.y + ImGui::GetTextLineHeight() * 0.5f;
    ImU32 fill = active
        ? IM_COL32(50, 220, 90, 255)
        : IM_COL32(80, 85, 95, 255);
    dl->AddCircleFilled(ImVec2(pos.x + 6.f, cy), 6.f, fill);
    // advance cursor past the dot
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  panel_setup_content — all tabs, no window wrapper.
// ─────────────────────────────────────────────────────────────────────────────
void panel_setup_content(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs)
{
    // Panel-level style push
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(6.f, 5.f));

    if (!ImGui::BeginTabBar("##setup_tabs", ImGuiTabBarFlags_DrawSelectedOverline))
    {
        ImGui::PopStyleVar(2);
        return;
    }

    // ── Tab 1: Output (first — most-used during a show) ───────────────────
    if (ImGui::BeginTabItem("Output"))
    {
        ImDrawList* dl_out = ImGui::GetWindowDrawList();

        ImGui::Spacing();
        ImGui::SeparatorText("Enabled output streams");
        ImGui::Spacing();

        // Each checkbox is a master enable for that stream kind.
        // Unchecking mutes the entire stream type regardless of per-output patch.

        // ── Stream checkboxes group ───────────────────────────────────────
        ImGui::BeginGroup();
        ImVec2 grp_streams_min = ImGui::GetCursorScreenPos();

        // ── DAC / Laser Hardware ──────────────────────────────────────────
        {
            bool dac_on = state.output_config.stream_type_dac_enabled;
            draw_stream_dot(dac_on);
            ImGui::PushStyleColor(ImGuiCol_Text,
                dac_on ? ImVec4(0.f, 1.f, 0.45f, 1.f) : ImVec4(0.45f, 0.45f, 0.5f, 1.f));
            if (ImGui::Checkbox("##stype_dac", &dac_on)) {
                state.output_config.stream_type_dac_enabled = dac_on;
                if (cbs.on_stream_type_enabled) cbs.on_stream_type_enabled(0, dac_on);
            }
            ImGui::SameLine();
            ImGui::Text("DAC / Laser Hardware");
            ImGui::PopStyleColor();
            ImGui::SameLine(260.f);
            ImGui::TextDisabled("Helios, EtherDream, LaserDock, IDN DAC");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Master enable for all DAC laser hardware outputs.\n"
                    "When off, no frames are sent to any connected DAC regardless of per-output patch settings.\n"
                    "WARNING: Ensure all safety measures are in place before enabling laser hardware.");
        }

        ImGui::Spacing();

        // ── NDI Stream ────────────────────────────────────────────────────
        {
            bool ndi_on = state.output_config.stream_type_ndi_enabled;
            draw_stream_dot(ndi_on);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ndi_on ? ImVec4(0.f, 0.9f, 1.f, 1.f) : ImVec4(0.45f, 0.45f, 0.5f, 1.f));
            if (ImGui::Checkbox("##stype_ndi", &ndi_on)) {
                state.output_config.stream_type_ndi_enabled = ndi_on;
                if (cbs.on_stream_type_enabled) cbs.on_stream_type_enabled(1, ndi_on);
            }
            ImGui::SameLine();
            ImGui::Text("NDI");
            ImGui::PopStyleColor();
            ImGui::SameLine(260.f);
            ImGui::TextDisabled("(configure in NDI Output tab)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Master enable for NDI video output.\n"
                    "When off, no NDI frames are sent to any receiver on the LAN.");
        }

        ImGui::Spacing();

        // ── Art-Net Output ────────────────────────────────────────────────
        {
            bool artnet_on = state.output_config.stream_type_artnet_enabled;
            draw_stream_dot(artnet_on);
            ImGui::PushStyleColor(ImGuiCol_Text,
                artnet_on ? ImVec4(1.f, 0.75f, 0.f, 1.f) : ImVec4(0.45f, 0.45f, 0.5f, 1.f));
            if (ImGui::Checkbox("##stype_artnet", &artnet_on)) {
                state.output_config.stream_type_artnet_enabled = artnet_on;
                if (cbs.on_stream_type_enabled) cbs.on_stream_type_enabled(2, artnet_on);
            }
            ImGui::SameLine();
            ImGui::Text("Art-Net Output");
            ImGui::PopStyleColor();
            ImGui::SameLine(260.f);
            ImGui::TextDisabled("DMX512 over Art-Net 4 (UDP 6454)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Master enable for Art-Net DMX output.\n"
                    "When off, no Art-Net universes are sent regardless of per-output settings.\n"
                    "Configure destination IP and universe in the Network tab.");
        }

        ImGui::Spacing();

        // ── IDN Stream ────────────────────────────────────────────────────
        {
            bool idn_on = state.output_config.stream_type_idn_enabled;
            draw_stream_dot(idn_on);
            ImGui::PushStyleColor(ImGuiCol_Text,
                idn_on ? ImVec4(0.f, 0.8f, 1.f, 1.f) : ImVec4(0.45f, 0.45f, 0.5f, 1.f));
            if (ImGui::Checkbox("##stype_idn", &idn_on)) {
                state.output_config.stream_type_idn_enabled = idn_on;
                if (cbs.on_stream_type_enabled) cbs.on_stream_type_enabled(3, idn_on);
            }
            ImGui::SameLine();
            ImGui::Text("IDN Stream");
            ImGui::PopStyleColor();
            ImGui::SameLine(260.f);
            ImGui::TextDisabled("ILDA Digital Network \xe2\x86\x92 UDP 7255");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Master enable for IDN-Stream broadcast sidecar (UDP port 7255).\n"
                    "When off, no IDN frames are broadcast to any receiver.\n"
                    "Compatible receivers: IDN-Toolbox, OpenIDN hardware DACs.");
        }

        ImGui::Spacing();
        ImGui::EndGroup();
        {
            ImVec2 grp_streams_max = ImGui::GetItemRectMax();
            grp_streams_max.x = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            grp_streams_max.y += 4.f;
            draw_group_border(dl_out, ImVec2(grp_streams_min.x - 4.f, grp_streams_min.y - 4.f), grp_streams_max);
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Render Settings");
        ImGui::Spacing();

        ImGui::BeginGroup();
        ImVec2 grp_render_min = ImGui::GetCursorScreenPos();

        ImGui::Text("Beam Thickness:");
        ImGui::SameLine(210.f);
        ImGui::SetNextItemWidth(180.f);
        float beam_t = ctx.beam_thickness;
        if (ImGui::SliderFloat("##beam_thick", &beam_t, 0.5f, 5.f, "%.1f px"))
            ctx.beam_thickness = beam_t;
        ImGui::SetItemTooltip("Rendered beam radius in pixels (NDI/preview output only).");

        ImGui::Spacing();

        ImGui::Text("Glow Radius:");
        ImGui::SameLine(210.f);
        ImGui::SetNextItemWidth(180.f);
        float glow_r = state.output_config.glow_radius;
        if (ImGui::SliderFloat("##glow_r", &glow_r, 2.f, 20.f, "%.1f px"))
            state.output_config.glow_radius = glow_r;
        ImGui::SetItemTooltip("Radius of the soft glow halo around each beam.");

        ImGui::Spacing();

        ImGui::Text("Glow Intensity:");
        ImGui::SameLine(210.f);
        ImGui::SetNextItemWidth(180.f);
        float glow_a = state.output_config.glow_alpha;
        if (ImGui::SliderFloat("##glow_a", &glow_a, 0.f, 1.f, "%.0f%%"))
            state.output_config.glow_alpha = glow_a;
        ImGui::SetItemTooltip("Alpha opacity of the glow halo (0 = invisible, 1 = fully opaque).");

        ImGui::Spacing();
        ImGui::EndGroup();
        {
            ImVec2 grp_render_max = ImGui::GetItemRectMax();
            grp_render_max.x = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            grp_render_max.y += 4.f;
            draw_group_border(dl_out, ImVec2(grp_render_min.x - 4.f, grp_render_min.y - 4.f), grp_render_max);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Apply##out_apply", ImVec2(80.f, 0.f))) {
            if (cbs.on_setup_beam_params)
                cbs.on_setup_beam_params(ctx.beam_thickness,
                                         state.output_config.glow_radius,
                                         state.output_config.glow_alpha);
        }
        ImGui::SetItemTooltip("Apply render settings to the output pipeline.");

        ImGui::EndTabItem();
    }

    // ── Tab 2: NDI Output ─────────────────────────────────────────────────
    if (ImGui::BeginTabItem("NDI Output"))
    {
        ImDrawList* dl_ndi = ImGui::GetWindowDrawList();

        ImGui::Spacing();
        ImGui::SeparatorText("NDI Stream Configuration");
        ImGui::Spacing();

        ImGui::BeginGroup();
        ImVec2 grp_ndi_min = ImGui::GetCursorScreenPos();

        // NDI Source Name
        static char ndi_name_buf[128] = "IDHMFIS";
        {
            static bool s_initialized = false;
            if (!s_initialized) {
                std::strncpy(ndi_name_buf,
                             state.output_config.ndi_name.c_str(),
                             sizeof(ndi_name_buf) - 1);
                ndi_name_buf[sizeof(ndi_name_buf) - 1] = '\0';
                s_initialized = true;
            }
        }

        ImGui::AlignTextToFramePadding();
        ImGui::Text("NDI Source Name:");
        ImGui::SameLine(170.f);
        ImGui::SetNextItemWidth(200.f);
        if (ImGui::InputText("##ndi_name", ndi_name_buf, sizeof(ndi_name_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SetItemTooltip(
            "Name broadcast over the network. Receivers identify this stream by this name.");

        ImGui::Spacing();

        // Resolution
        static int res_idx = 1;
        {
            static bool s_res_init = false;
            if (!s_res_init) {
                s_res_init = true;
                if      (state.output_config.ndi_width == 960)  res_idx = 0;
                else if (state.output_config.ndi_width == 3840) res_idx = 2;
                else                                             res_idx = 1;
            }
        }
        const char* resolutions[] = { "960x540 (nHD)", "1920x1080 (HD)", "3840x2160 (4K)" };
        ImGui::Text("Resolution:");
        ImGui::SameLine(170.f);
        ImGui::SetNextItemWidth(160.f);
        ImGui::Combo("##ndi_res", &res_idx, resolutions, 3);
        ImGui::SetItemTooltip("Output resolution of the NDI video stream.");

        ImGui::Spacing();

        // Frame Rate — standard broadcast rates including NTSC fractional (N/1001)
        struct FpsPreset { const char* label; int fps_N; int fps_D; };
        static constexpr FpsPreset kFpsPresets[] = {
            { "23.98 fps (24000/1001)", 24000, 1001 },
            { "24 fps",                 24,    1    },
            { "25 fps",                 25,    1    },
            { "29.97 fps (30000/1001)", 30000, 1001 },
            { "30 fps",                 30,    1    },
            { "50 fps",                 50,    1    },
            { "59.94 fps (60000/1001)", 60000, 1001 },
            { "60 fps",                 60,    1    },
        };
        static constexpr int kNumFpsPresets = 8;
        static const char* kFpsLabels[kNumFpsPresets] = {
            kFpsPresets[0].label, kFpsPresets[1].label, kFpsPresets[2].label,
            kFpsPresets[3].label, kFpsPresets[4].label, kFpsPresets[5].label,
            kFpsPresets[6].label, kFpsPresets[7].label
        };

        static int fps_idx = 7;  // default: 60 fps
        {
            static bool s_fps_init = false;
            if (!s_fps_init) {
                s_fps_init = true;
                // Match current state to a preset
                for (int i = 0; i < kNumFpsPresets; ++i) {
                    if (state.output_config.ndi_fps_N == kFpsPresets[i].fps_N &&
                        state.output_config.ndi_fps_D == kFpsPresets[i].fps_D) {
                        fps_idx = i;
                        break;
                    }
                }
            }
        }
        ImGui::Text("Frame Rate:");
        ImGui::SameLine(170.f);
        ImGui::SetNextItemWidth(220.f);
        ImGui::Combo("##ndi_fps", &fps_idx, kFpsLabels, kNumFpsPresets);
        ImGui::SetItemTooltip("NDI output frame rate.\nFractional rates (N/1001) are standard for broadcast receivers.");

        ImGui::Spacing();

        // Enable NDI Output
        bool ndi_enabled = state.output_config.ndi_enabled;
        ImGui::Text("Enable NDI Output:");
        ImGui::SameLine(170.f);
        if (ImGui::Checkbox("##ndi_enabled", &ndi_enabled))
            state.output_config.ndi_enabled = ndi_enabled;
        ImGui::SetItemTooltip("When enabled, renders a video stream over NDI for preview or capture.");

        ImGui::Spacing();

        // NDI Timing Mode
        static bool s_clock_video_init = false;
        static bool s_clock_video = true;
        if (!s_clock_video_init) {
            s_clock_video_init = true;
            s_clock_video = state.output_config.ndi_clock_video;
        }
        ImGui::Text("NDI Timing:");
        ImGui::SameLine(170.f);
        ImGui::Checkbox("NDI-clocked##ndi_clock", &s_clock_video);
        ImGui::SetItemTooltip(
            "NDI-clocked (recommended): NDI receiver paces the sends.\n"
            "Gives stable cadence to all downstream receivers.\n"
            "Adds approximately 1 frame of latency.\n\n"
            "Unchecked = app-clocked: lower latency but frame drops\n"
            "visible on engine hiccup or complex generator tick.");

        ImGui::Spacing();

        // Background Color
        static float bg_color[4] = { 0.f, 0.f, 0.f, 1.f };
        ImGui::Text("Background Color:");
        ImGui::SameLine(170.f);
        ImGui::ColorEdit4("##bg_color", bg_color,
                          ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_AlphaBar);
        ImGui::SetItemTooltip("Background fill color of the NDI video frame.");

        ImGui::Spacing();
        ImGui::EndGroup();
        {
            ImVec2 grp_ndi_max = ImGui::GetItemRectMax();
            grp_ndi_max.x = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            grp_ndi_max.y += 4.f;
            draw_group_border(dl_ndi, ImVec2(grp_ndi_min.x - 4.f, grp_ndi_min.y - 4.f), grp_ndi_max);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Apply##ndi_apply", ImVec2(80.f, 0.f)))
        {
            LayoutCallbacks::NdiConfig cfg;
            cfg.source_name = std::string(ndi_name_buf);
            switch (res_idx) {
                case 0:  cfg.width = 960;  cfg.height = 540;  break;
                case 2:  cfg.width = 3840; cfg.height = 2160; break;
                default: cfg.width = 1920; cfg.height = 1080; break;
            }
            cfg.fps_N       = kFpsPresets[fps_idx].fps_N;
            cfg.fps_D       = kFpsPresets[fps_idx].fps_D;
            cfg.enabled     = ndi_enabled;
            cfg.clock_video = s_clock_video;

            if (cbs.on_setup_ndi_config)
                cbs.on_setup_ndi_config(cfg);
        }
        ImGui::SetItemTooltip("Apply NDI configuration. Changes take effect on the next frame.");

        ImGui::EndTabItem();
    }

    // ── Tab: Network ──────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem("Network"))
    {
        // Cache interface list — refresh once per tab entry or on demand
        static std::vector<std::pair<std::string, std::string>> s_ifaces;
        static bool s_ifaces_loaded = false;
        if (!s_ifaces_loaded) {
            s_ifaces = enumerate_network_interfaces();
            s_ifaces_loaded = true;
        }

        auto& nc = state.net_config;

        // Determine the display IP for the selected interface
        const std::string& cur_ip = (nc.iface_auto || nc.iface_index < 0
                                     || nc.iface_index >= static_cast<int>(s_ifaces.size()))
            ? s_ifaces[0].second
            : s_ifaces[static_cast<size_t>(nc.iface_index)].second;
        nc.iface_ip_display = cur_ip;

        // Build combo labels buffer (null-separated list for ImGui::Combo)
        static char  s_iface_combo_buf[2048]{};
        static int   s_iface_combo_cnt = 0;
        static bool  s_combo_built     = false;
        if (!s_combo_built) {
            s_combo_built = true;
            int off = 0;
            s_iface_combo_cnt = 0;
            for (const auto& iface : s_ifaces) {
                int written = std::snprintf(s_iface_combo_buf + off,
                    sizeof(s_iface_combo_buf) - static_cast<size_t>(off),
                    "%s  [%s]", iface.first.c_str(), iface.second.c_str());
                if (written <= 0) break;
                off += written + 1;
                ++s_iface_combo_cnt;
                if (off + 2 >= static_cast<int>(sizeof(s_iface_combo_buf))) break;
            }
            if (off < static_cast<int>(sizeof(s_iface_combo_buf)))
                s_iface_combo_buf[off] = '\0';
        }

        ImGui::BeginChild("##net_scroll", ImVec2(0.f, 0.f), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        // ── Network Interface Selection ────────────────────────────────────────
        ImGui::SeparatorText("Network Interface (Global)");
        ImGui::Spacing();

        ImGui::Checkbox("Auto-detect interface##iface_auto", &nc.iface_auto);
        ImGui::SetItemTooltip("When ON, automatically uses the first non-loopback IPv4 adapter.");
        ImGui::SameLine(0.f, 12.f);
        if (ImGui::SmallButton("Refresh##iface_refresh")) {
            s_ifaces = enumerate_network_interfaces();
            s_combo_built = false;
        }
        ImGui::SetItemTooltip("Re-enumerate network adapters.");

        ImGui::BeginDisabled(nc.iface_auto);
        ImGui::Text("Interface:");
        ImGui::SameLine(130.f);
        ImGui::SetNextItemWidth(-1.f);
        if (s_iface_combo_cnt > 0) {
            int idx = nc.iface_index;
            if (ImGui::Combo("##iface_sel", &idx, s_iface_combo_buf, s_iface_combo_cnt))
                nc.iface_index = idx;
        } else {
            ImGui::TextDisabled("No adapters found");
        }
        ImGui::EndDisabled();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.9f, 0.2f, 1.f));
        ImGui::Text("Active IP:  %s", cur_ip.c_str());
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // ── ArtNet Input ───────────────────────────────────────────────────────
        ImGui::SeparatorText("ArtNet (DMX512 over IP)");
        ImGui::Spacing();

        {
            ImGui::Checkbox("Enable ArtNet Input##an_en", &nc.artnet_enabled);
            ImGui::SetItemTooltip("Receive ArtNet DMX from any Art-Net 4 lighting controller.");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Checkbox("Auto##an_auto", &nc.artnet_auto);
            ImGui::SetItemTooltip("Auto: universe 0, net 0, subnet 0, listen 0.0.0.0:6454.");

            ImGui::BeginDisabled(!nc.artnet_enabled || nc.artnet_auto);

            ImGui::Text("Universe:"); ImGui::SameLine(120.f);
            ImGui::SetNextItemWidth(70.f);
            ImGui::InputInt("##an_uni", &nc.artnet_universe, 1, 10);
            if (nc.artnet_universe < 0)  nc.artnet_universe = 0;
            if (nc.artnet_universe > 15) nc.artnet_universe = 15;
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("Net:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(60.f);
            ImGui::InputInt("##an_net", &nc.artnet_net, 1, 5);
            if (nc.artnet_net < 0)   nc.artnet_net = 0;
            if (nc.artnet_net > 127) nc.artnet_net = 127;
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("Subnet:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(60.f);
            ImGui::InputInt("##an_sub", &nc.artnet_subnet, 1, 5);
            if (nc.artnet_subnet < 0)  nc.artnet_subnet = 0;
            if (nc.artnet_subnet > 15) nc.artnet_subnet = 15;

            ImGui::Text("Listen IP:"); ImGui::SameLine(120.f);
            ImGui::SetNextItemWidth(160.f);
            {
                char buf[64];
                std::strncpy(buf, nc.artnet_listen_ip.c_str(), sizeof(buf) - 1);
                buf[63] = '\0';
                if (ImGui::InputText("##an_lip", buf, sizeof(buf)))
                    nc.artnet_listen_ip = buf;
            }
            ImGui::SetItemTooltip("0.0.0.0 = listen on all interfaces.");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("Port:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(70.f);
            ImGui::InputInt("##an_port", &nc.artnet_port, 0, 0);
            if (nc.artnet_port < 1)     nc.artnet_port = 1;
            if (nc.artnet_port > 65535) nc.artnet_port = 65535;

            ImGui::Checkbox("Merge HTP##an_htp", &nc.artnet_merge_htp);
            ImGui::SetItemTooltip("Merge multiple Art-Net sources using HTP.");
            ImGui::SameLine(0.f, 12.f);
            ImGui::Text("Merge Mode:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(90.f);
            {
                const char* modes[] = { "LTP", "HTP" };
                ImGui::Combo("##an_merge", &nc.artnet_merge_mode, modes, 2);
            }
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("Priority:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(60.f);
            ImGui::InputInt("##an_pri", &nc.artnet_priority, 1, 10);
            if (nc.artnet_priority < 0)   nc.artnet_priority = 0;
            if (nc.artnet_priority > 200) nc.artnet_priority = 200;

            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        // ── sACN (E1.31) Input ─────────────────────────────────────────────────
        ImGui::SeparatorText("sACN / E1.31 (ESTA streaming ACN)");
        ImGui::Spacing();

        {
            ImGui::Checkbox("Enable sACN Input##sacn_en", &nc.sacn_enabled);
            ImGui::SetItemTooltip("Receive sACN DMX over multicast or unicast (ESTA E1.31).");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Checkbox("Auto##sacn_auto", &nc.sacn_auto);
            ImGui::SetItemTooltip("Auto: universe 1, priority 100, multicast 239.255.0.1:5568.");

            ImGui::BeginDisabled(!nc.sacn_enabled || nc.sacn_auto);

            ImGui::Text("Universe:"); ImGui::SameLine(120.f);
            ImGui::SetNextItemWidth(70.f);
            ImGui::InputInt("##sacn_uni", &nc.sacn_universe, 1, 10);
            if (nc.sacn_universe < 1)     nc.sacn_universe = 1;
            if (nc.sacn_universe > 63999) nc.sacn_universe = 63999;
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("Priority:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(60.f);
            ImGui::InputInt("##sacn_pri", &nc.sacn_priority, 1, 10);
            if (nc.sacn_priority < 0)   nc.sacn_priority = 0;
            if (nc.sacn_priority > 200) nc.sacn_priority = 200;

            ImGui::Text("Multicast IP:"); ImGui::SameLine(120.f);
            ImGui::SetNextItemWidth(160.f);
            {
                char buf[64];
                std::strncpy(buf, nc.sacn_multicast_ip.c_str(), sizeof(buf) - 1);
                buf[63] = '\0';
                if (ImGui::InputText("##sacn_mip", buf, sizeof(buf)))
                    nc.sacn_multicast_ip = buf;
            }
            ImGui::SetItemTooltip("Standard: 239.255.0.x where x = universe & 0xFF.");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("Port:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(70.f);
            ImGui::InputInt("##sacn_port", &nc.sacn_port, 0, 0);
            if (nc.sacn_port < 1)     nc.sacn_port = 1;
            if (nc.sacn_port > 65535) nc.sacn_port = 65535;

            ImGui::Checkbox("Per-universe multicast join##sacn_pumu", &nc.sacn_per_universe);
            ImGui::SetItemTooltip(
                "Join the correct multicast group for each sACN universe individually.");

            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        // ── OSC ────────────────────────────────────────────────────────────────
        ImGui::SeparatorText("OSC (Open Sound Control)");
        ImGui::Spacing();

        {
            ImGui::Checkbox("Enable OSC Input##osc_in_en", &nc.osc_in_enabled);
            ImGui::SetItemTooltip("Receive OSC messages for real-time cue and parameter control.");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Checkbox("Auto##osc_in_auto", &nc.osc_in_auto);
            ImGui::SetItemTooltip("Auto: listen 0.0.0.0:7700.");

            ImGui::BeginDisabled(!nc.osc_in_enabled || nc.osc_in_auto);
            ImGui::Text("Listen Port:"); ImGui::SameLine(130.f);
            ImGui::SetNextItemWidth(80.f);
            ImGui::InputInt("##osc_in_port", &nc.osc_in_port, 1, 10);
            if (nc.osc_in_port < 1)     nc.osc_in_port = 1;
            if (nc.osc_in_port > 65535) nc.osc_in_port = 65535;
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("Listen IP:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(140.f);
            {
                char buf[64];
                std::strncpy(buf, nc.osc_in_ip.c_str(), sizeof(buf) - 1);
                buf[63] = '\0';
                if (ImGui::InputText("##osc_in_ip", buf, sizeof(buf)))
                    nc.osc_in_ip = buf;
            }
            ImGui::SetItemTooltip("0.0.0.0 = listen on all interfaces.");
            ImGui::EndDisabled();

            ImGui::Spacing();

            ImGui::Checkbox("Enable OSC Output##osc_out_en", &nc.osc_out_enabled);
            ImGui::SetItemTooltip("Send OSC feedback messages to a remote target.");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Checkbox("Auto##osc_out_auto", &nc.osc_out_auto);
            ImGui::SetItemTooltip("Auto: target 127.0.0.1:7701.");

            ImGui::BeginDisabled(!nc.osc_out_enabled || nc.osc_out_auto);
            ImGui::Text("Target IP:"); ImGui::SameLine(130.f);
            ImGui::SetNextItemWidth(140.f);
            {
                char buf[64];
                std::strncpy(buf, nc.osc_out_ip.c_str(), sizeof(buf) - 1);
                buf[63] = '\0';
                if (ImGui::InputText("##osc_out_ip", buf, sizeof(buf)))
                    nc.osc_out_ip = buf;
            }
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("Port:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(70.f);
            ImGui::InputInt("##osc_out_port", &nc.osc_out_port, 1, 10);
            if (nc.osc_out_port < 1)     nc.osc_out_port = 1;
            if (nc.osc_out_port > 65535) nc.osc_out_port = 65535;

            ImGui::Text("Prefix:"); ImGui::SameLine(130.f);
            ImGui::SetNextItemWidth(200.f);
            {
                char buf[64];
                std::strncpy(buf, nc.osc_prefix.c_str(), sizeof(buf) - 1);
                buf[63] = '\0';
                if (ImGui::InputText("##osc_pfx", buf, sizeof(buf)))
                    nc.osc_prefix = buf;
            }
            ImGui::SetItemTooltip("OSC address prefix for all outgoing messages, e.g. /idhmfis/");
            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        // ── CITP/CAEX ──────────────────────────────────────────────────────────
        ImGui::SeparatorText("CITP/CAEX (Capture / MA3 visualization)");
        ImGui::Spacing();

        {
            ImGui::Checkbox("Enable CITP/CAEX##citp_en", &nc.citp_enabled);
            ImGui::SetItemTooltip(
                "CITP sidecar for Capture, MA3, and other visualizers.\n"
                "TCP 6430 control + multicast 239.224.0.180:4809 discovery.");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Checkbox("Auto##citp_auto", &nc.citp_auto);
            ImGui::SetItemTooltip("Auto: TCP 6430, multicast 239.224.0.180:4809, first non-loopback.");

            ImGui::BeginDisabled(!nc.citp_enabled || nc.citp_auto);

            ImGui::Text("TCP Port:"); ImGui::SameLine(180.f);
            ImGui::SetNextItemWidth(80.f);
            ImGui::InputInt("##citp_tcp", &nc.citp_tcp_port, 0, 0);
            if (nc.citp_tcp_port < 1)     nc.citp_tcp_port = 1;
            if (nc.citp_tcp_port > 65535) nc.citp_tcp_port = 65535;

            ImGui::Text("Multicast Group:"); ImGui::SameLine(180.f);
            ImGui::SetNextItemWidth(160.f);
            {
                char buf[64];
                std::strncpy(buf, nc.citp_multicast_group.c_str(), sizeof(buf) - 1);
                buf[63] = '\0';
                if (ImGui::InputText("##citp_mgrp", buf, sizeof(buf)))
                    nc.citp_multicast_group = buf;
            }
            ImGui::SetItemTooltip("CITP standard: 239.224.0.180");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("Port:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(70.f);
            ImGui::InputInt("##citp_mport", &nc.citp_multicast_port, 0, 0);
            if (nc.citp_multicast_port < 1)     nc.citp_multicast_port = 1;
            if (nc.citp_multicast_port > 65535) nc.citp_multicast_port = 65535;

            ImGui::Text("Source Name:"); ImGui::SameLine(180.f);
            ImGui::SetNextItemWidth(200.f);
            {
                char buf[128];
                std::strncpy(buf, nc.citp_source_name.c_str(), sizeof(buf) - 1);
                buf[127] = '\0';
                if (ImGui::InputText("##citp_sname", buf, sizeof(buf)))
                    nc.citp_source_name = buf;
            }
            ImGui::SetItemTooltip("Name shown in Capture / MA3 fixture list.");

            ImGui::Text("Multicast Iface:"); ImGui::SameLine(180.f);
            ImGui::TextColored(ImVec4(0.1f, 0.9f, 0.2f, 1.f), "%s", cur_ip.c_str());
            ImGui::SetItemTooltip("CITP uses the global interface IP selected above.");

            ImGui::EndDisabled();

            ImGui::BeginDisabled(!nc.citp_enabled);
            ImGui::Checkbox("Respond to Capture discovery##citp_cap", &nc.citp_respond_capture);
            ImGui::SetItemTooltip("Reply to CITP MSEX CMtC GetPeerList to appear in Capture's source list.");
            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        // ── IDN-Stream ─────────────────────────────────────────────────────────
        ImGui::SeparatorText("IDN-Stream (ILDA Digital Network)");
        ImGui::Spacing();

        {
            ImGui::Checkbox("Enable IDN-Stream##idn_en", &nc.idn_enabled);
            ImGui::SetItemTooltip(
                "Broadcast laser frames over ILDA Digital Network (UDP 7255).\n"
                "Compatible: IDN-Toolbox, OpenIDN hardware DACs.");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Checkbox("Auto##idn_auto", &nc.idn_auto);
            ImGui::SetItemTooltip("Auto: broadcast 255.255.255.255:7255, channel 0.");

            ImGui::BeginDisabled(!nc.idn_enabled || nc.idn_auto);

            ImGui::Text("Broadcast Addr:"); ImGui::SameLine(170.f);
            ImGui::SetNextItemWidth(160.f);
            {
                char buf[64];
                std::strncpy(buf, nc.idn_broadcast_addr.c_str(), sizeof(buf) - 1);
                buf[63] = '\0';
                if (ImGui::InputText("##idn_bcast", buf, sizeof(buf)))
                    nc.idn_broadcast_addr = buf;
            }
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("Port:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(70.f);
            ImGui::InputInt("##idn_port", &nc.idn_port, 0, 0);
            if (nc.idn_port < 1)     nc.idn_port = 1;
            if (nc.idn_port > 65535) nc.idn_port = 65535;

            ImGui::Text("Channel:"); ImGui::SameLine(170.f);
            ImGui::SetNextItemWidth(70.f);
            ImGui::InputInt("##idn_chan", &nc.idn_channel, 1, 5);
            if (nc.idn_channel < 0)   nc.idn_channel = 0;
            if (nc.idn_channel > 255) nc.idn_channel = 255;
            ImGui::SetItemTooltip("IDN channel index (0 = first / default).");

            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        // ── CLS (CommonLaserStream) ────────────────────────────────────────────
        ImGui::SeparatorText("CLS (CommonLaserStream)");
        ImGui::Spacing();

        {
            ImGui::Checkbox("Enable CLS##cls_en", &nc.cls_enabled);
            ImGui::SetItemTooltip(
                "CommonLaserStream protocol: TCP 7256 control + UDP 7256 discovery.");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Checkbox("Auto##cls_auto", &nc.cls_auto);
            ImGui::SetItemTooltip("Auto: TCP 7256, UDP 7256, broadcast discovery enabled.");

            ImGui::BeginDisabled(!nc.cls_enabled || nc.cls_auto);

            ImGui::Text("TCP Port:"); ImGui::SameLine(130.f);
            ImGui::SetNextItemWidth(80.f);
            ImGui::InputInt("##cls_tcp", &nc.cls_tcp_port, 0, 0);
            if (nc.cls_tcp_port < 1)     nc.cls_tcp_port = 1;
            if (nc.cls_tcp_port > 65535) nc.cls_tcp_port = 65535;
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("UDP Port:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(80.f);
            ImGui::InputInt("##cls_udp", &nc.cls_udp_port, 0, 0);
            if (nc.cls_udp_port < 1)     nc.cls_udp_port = 1;
            if (nc.cls_udp_port > 65535) nc.cls_udp_port = 65535;

            ImGui::EndDisabled();

            ImGui::BeginDisabled(!nc.cls_enabled);
            ImGui::Checkbox("UDP Broadcast discovery##cls_bcast", &nc.cls_udp_broadcast);
            ImGui::SetItemTooltip("Send CLS discovery beacons on the UDP broadcast address.");
            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        // ── EtherDream ─────────────────────────────────────────────────────────
        ImGui::SeparatorText("EtherDream DAC (network discovery)");
        ImGui::Spacing();

        {
            ImGui::Checkbox("Enable EtherDream discovery##ed_en", &nc.etherdream_enabled);
            ImGui::SetItemTooltip(
                "Auto-discover EtherDream DACs on the LAN via broadcast probe.");
            ImGui::SameLine(0.f, 16.f);
            ImGui::Checkbox("Auto##ed_auto", &nc.etherdream_auto);
            ImGui::SetItemTooltip("Auto: 2.0 s discovery timeout, accept any IP.");

            ImGui::BeginDisabled(!nc.etherdream_enabled || nc.etherdream_auto);

            ImGui::Text("Discovery timeout:"); ImGui::SameLine(185.f);
            ImGui::SetNextItemWidth(120.f);
            ImGui::SliderFloat("##ed_tout", &nc.etherdream_timeout, 0.2f, 10.f, "%.1f s",
                               ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("How long to wait for EtherDream broadcast response (seconds).");

            ImGui::Text("Preferred IP:"); ImGui::SameLine(185.f);
            ImGui::SetNextItemWidth(160.f);
            {
                char buf[64];
                std::strncpy(buf, nc.etherdream_preferred_ip.c_str(), sizeof(buf) - 1);
                buf[63] = '\0';
                if (ImGui::InputText("##ed_ip", buf, sizeof(buf)))
                    nc.etherdream_preferred_ip = buf;
            }
            ImGui::SetItemTooltip("Leave blank to accept any EtherDream. Set a specific IP to lock to one unit.");

            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        // ── NDI Network Settings ───────────────────────────────────────────────
        ImGui::SeparatorText("NDI Output Network");
        ImGui::Spacing();

        {
            ImGui::Checkbox("Auto##ndi_net_auto", &nc.ndi_net_auto);
            ImGui::SetItemTooltip("Auto: 'IDHMFIS Laser Preview', high quality, 30 fps.");

            ImGui::BeginDisabled(nc.ndi_net_auto);

            ImGui::Text("Source Name:"); ImGui::SameLine(150.f);
            ImGui::SetNextItemWidth(220.f);
            {
                char buf[128];
                std::strncpy(buf, nc.ndi_net_source_name.c_str(), sizeof(buf) - 1);
                buf[127] = '\0';
                if (ImGui::InputText("##ndi_net_sname", buf, sizeof(buf)))
                    nc.ndi_net_source_name = buf;
            }
            ImGui::SetItemTooltip("Name shown to NDI receivers on the LAN.");

            ImGui::Text("Bandwidth:"); ImGui::SameLine(150.f);
            ImGui::SetNextItemWidth(200.f);
            {
                const char* bw_opts[] = { "Low bandwidth (proxy)", "High Quality (full res)" };
                ImGui::Combo("##ndi_net_bw", &nc.ndi_net_bandwidth, bw_opts, 2);
            }
            ImGui::SetItemTooltip("Low = reduced data rate for congested networks.");

            ImGui::Text("Frame Rate:"); ImGui::SameLine(150.f);
            ImGui::SetNextItemWidth(80.f);
            ImGui::InputInt("##ndi_net_fps", &nc.ndi_net_fps, 1, 5);
            if (nc.ndi_net_fps < 1)   nc.ndi_net_fps = 1;
            if (nc.ndi_net_fps > 120) nc.ndi_net_fps = 120;
            ImGui::SameLine();
            ImGui::TextDisabled("fps");

            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Apply All##net_apply", ImVec2(100.f, 0.f)))
        {
            if (cbs.on_network_config_apply)
                cbs.on_network_config_apply(nc);
            // Also notify via legacy OSC callback so existing receiver picks it up
            if (cbs.on_setup_osc_port)
                cbs.on_setup_osc_port(nc.osc_in_port);
        }
        ImGui::SetItemTooltip(
            "Apply all network settings.\n"
            "Protocol sockets will rebind on next engine tick.");

        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // ── Tab 4: 3D Preview ─────────────────────────────────────────────────
    if (ImGui::BeginTabItem("3D Preview"))
    {
        ImGui::Spacing();
        ImGui::SeparatorText("Preview Mode");
        ImGui::Spacing();

        auto& p3d = state.preview_3d;

        bool scan_mode = p3d.scan_mode;
        if (ImGui::Checkbox("Scan Mode", &scan_mode))
            p3d.scan_mode = scan_mode;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Off: Aerial Haze mode — shows laser beams glowing through air/haze (default, for aerial FX preview).\n"
                "On: Scan Mode — visible galvo scanner sweep with scan trail.");

        ImGui::Spacing();
        ImGui::SeparatorText("Common Settings");
        ImGui::Spacing();

        ImGui::Text("Beam Brightness:");
        ImGui::SameLine(200.f);
        ImGui::SetNextItemWidth(160.f);
        ImGui::SliderFloat("##p3d_brt", &p3d.beam_brightness, 0.f, 2.f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overall brightness multiplier for all laser rendering.");

        if (!scan_mode) {
            ImGui::Spacing();
            ImGui::SeparatorText("IRL Projector Settings");
            ImGui::Spacing();

            ImGui::Text("Haze Alpha:");
            ImGui::SameLine(200.f);
            ImGui::SetNextItemWidth(160.f);
            ImGui::SliderFloat("##p3d_haze", &p3d.haze_alpha, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opacity of the projector-to-wall haze beams.\nHigher = more visible beam scatter (hazy room).");

            ImGui::Text("Wall Glow (px):");
            ImGui::SameLine(200.f);
            ImGui::SetNextItemWidth(160.f);
            ImGui::SliderFloat("##p3d_glow", &p3d.wall_glow_px, 1.f, 20.f, "%.1f px");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Outer glow radius on the wall pattern (pixels).");

            ImGui::Text("Beam Core Width (px):");
            ImGui::SameLine(200.f);
            ImGui::SetNextItemWidth(160.f);
            ImGui::SliderFloat("##p3d_core", &p3d.beam_width_px, 0.5f, 6.f, "%.1f px");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Width of the bright core of each wall beam segment.");
        } else {
            ImGui::Spacing();
            ImGui::SeparatorText("Scan Mode Settings");
            ImGui::Spacing();

            ImGui::Text("Scan Speed:");
            ImGui::SameLine(200.f);
            ImGui::SetNextItemWidth(160.f);
            ImGui::SliderFloat("##p3d_spd", &p3d.scan_speed, 0.1f, 10.f, "%.2f scans/s");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Speed of the galvo scanner sweep in scans per second.");

            ImGui::Text("Trail Length:");
            ImGui::SameLine(200.f);
            ImGui::SetNextItemWidth(160.f);
            ImGui::SliderInt("##p3d_trail", &p3d.trail_pct, 1, 100, "%d%%");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Length of the scan trail as a percentage of the point count.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Reset to Defaults##p3d")) {
            p3d = {};
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset all 3D preview settings to defaults.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SeparatorText("Laser Positions");
        ImGui::Spacing();

        if (state.laser_placements.empty()) {
            ImGui::TextDisabled("No laser outputs patched");
        } else {
            static constexpr float kPi = 3.14159265358979323846f;
            static constexpr float kRadToDeg = 180.f / kPi;
            static constexpr float kDegToRad = kPi / 180.f;

            for (int li = 0; li < static_cast<int>(state.laser_placements.size()); ++li) {
                auto& lp = state.laser_placements[static_cast<size_t>(li)];

                // Resolve output name
                const char* out_name = "(unknown)";
                for (const auto& po : state.patched_outputs) {
                    if (po.id == lp.stream_id) {
                        out_name = po.name.c_str();
                        break;
                    }
                }

                ImGui::PushID(li);
                ImGui::SeparatorText(out_name);

                ImGui::Text("X:");
                ImGui::SameLine(60.f);
                ImGui::SetNextItemWidth(160.f);
                ImGui::DragFloat("##lp_x", &lp.pos_x, 0.1f, -6.f, 6.f, "%.2f m",
                                 ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetItemTooltip("Laser X position in the room (metres, -6 to +6).");

                ImGui::Text("Y:");
                ImGui::SameLine(60.f);
                ImGui::SetNextItemWidth(160.f);
                ImGui::DragFloat("##lp_y", &lp.pos_y, 0.1f, 0.f, 5.f, "%.2f m",
                                 ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetItemTooltip("Laser Y position in the room (metres, 0 = floor, 5 = ceiling).");

                ImGui::Text("Z:");
                ImGui::SameLine(60.f);
                ImGui::SetNextItemWidth(160.f);
                ImGui::DragFloat("##lp_z", &lp.pos_z, 0.1f, 0.f, 2.f, "%.2f m",
                                 ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetItemTooltip("Laser Z position in the room (metres, 0 = back wall, 2 = front).");

                // Yaw and pitch stored in radians, displayed in degrees
                float yaw_deg   = lp.yaw   * kRadToDeg;
                float pitch_deg = lp.pitch * kRadToDeg;

                ImGui::Text("Yaw:");
                ImGui::SameLine(60.f);
                ImGui::SetNextItemWidth(160.f);
                if (ImGui::DragFloat("##lp_yaw", &yaw_deg, 1.f, -180.f, 180.f, "%.1f deg",
                                     ImGuiSliderFlags_AlwaysClamp))
                    lp.yaw = yaw_deg * kDegToRad;
                ImGui::SetItemTooltip("Rotation around vertical axis (degrees, -180 to +180).");

                ImGui::Text("Pitch:");
                ImGui::SameLine(60.f);
                ImGui::SetNextItemWidth(160.f);
                if (ImGui::DragFloat("##lp_pitch", &pitch_deg, 1.f, -45.f, 45.f, "%.1f deg",
                                     ImGuiSliderFlags_AlwaysClamp))
                    lp.pitch = pitch_deg * kDegToRad;
                ImGui::SetItemTooltip("Rotation around horizontal axis (degrees, -45 to +45).");

                if (ImGui::SmallButton("Reset##lp_rst")) {
                    lp.pos_y  = 3.f;
                    lp.pos_z  = 0.5f;
                    lp.yaw    = 0.f;
                    lp.pitch  = 0.f;
                }
                ImGui::SetItemTooltip("Reset this laser to default position and orientation.");

                ImGui::Spacing();
                ImGui::PopID();
            }
        }

        ImGui::EndTabItem();
    }

    // ── Tab: Keybinds ─────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem("Keybinds"))
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Click a key field and press any key to rebind. Press Esc or Backspace to clear.");
        ImGui::Spacing();

        constexpr ImGuiTableFlags kKbTbl =
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##kb_tbl", 2, kKbTbl, ImVec2(-1.f, -1.f))) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("Key",    ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableHeadersRow();

            static int s_kb_capturing = -1;  // index being captured, -1 = none

            for (int ki = 0; ki < (int)state.keybinds.size(); ++ki) {
                auto& kb = state.keybinds[ki];
                ImGui::TableNextRow();
                ImGui::PushID(ki);

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(kb.action.c_str());

                ImGui::TableSetColumnIndex(1);
                bool capturing = (s_kb_capturing == ki);
                const char* key_label = (kb.key != 0)
                    ? ImGui::GetKeyName(static_cast<ImGuiKey>(kb.key))
                    : "(unbound)";
                char btn_lbl[64];
                std::snprintf(btn_lbl, sizeof(btn_lbl), "%s##kb_%d",
                    capturing ? "[press key...]" : key_label, ki);

                if (capturing) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.3f, 0.05f, 1.f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.4f, 0.08f, 1.f));
                }
                if (ImGui::Button(btn_lbl, ImVec2(-1.f, 0.f)))
                    s_kb_capturing = capturing ? -1 : ki;
                if (capturing) {
                    ImGui::PopStyleColor(2);
                    // Scan for any pressed key
                    for (int k = static_cast<int>(ImGuiKey_Tab);
                         k < static_cast<int>(ImGuiKey_ReservedForModCtrl); ++k) {
                        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) {
                            if (k == static_cast<int>(ImGuiKey_Escape) ||
                                k == static_cast<int>(ImGuiKey_Backspace))
                                kb.key = 0;
                            else
                                kb.key = k;
                            s_kb_capturing = -1;
                            break;
                        }
                    }
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ── Tab: Colors ───────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem("Colors"))
    {
        ImGui::Spacing();
        ImGui::SeparatorText("Color Input Mode");
        ImGui::Spacing();
        using CM = UIState::ColorInputMode;
        static const char* kModeNames[] = {
            "RGB % — channels as 0..100 percent (default)",
            "RGB Absolute — channels as 0..255 integers",
            "CMY — Cyan / Magenta / Yellow (0..100%)",
            "HSI — Hue (0..360\xC2\xB0) / Saturation / Intensity"
        };
        for (int mi = 0; mi < 4; ++mi) {
            bool sel = (static_cast<int>(state.color_input_mode) == mi);
            if (ImGui::RadioButton(kModeNames[mi], sel))
                state.color_input_mode = static_cast<CM>(mi);
        }
        ImGui::Spacing();
        ImGui::SeparatorText("Color Palette");
        ImGui::TextWrapped(
            "Default slots (0-7: RGBCMYW+Black, 8-15: secondaries) are always restored "
            "when starting a new show. Slots 16-31 are saved per showfile.\n"
            "REC + click empty slot = record. REM + click = clear slot.");
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("General"))
    {
        ImGui::Spacing();
        ImGui::SeparatorText("Autosave");
        ImGui::Spacing();

        bool as_changed = false;
        as_changed |= ImGui::Checkbox("Enable autosave##as", &ctx.autosave_enabled);
        ImGui::SetItemTooltip("Automatically saves a recovery copy at the specified interval.");

        if (!ctx.autosave_enabled) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(180.f);
        as_changed |= ImGui::SliderInt("Interval (seconds)##as_interval",
                                       &ctx.autosave_interval_s, 5, 600);
        ImGui::SetItemTooltip("How often to write the autosave file (5–600 seconds). Default: 30.");
        if (!ctx.autosave_enabled) ImGui::EndDisabled();

        if (as_changed && cbs.on_autosave_changed)
            cbs.on_autosave_changed(ctx.autosave_enabled, ctx.autosave_interval_s);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SeparatorText("Application");
        ImGui::Spacing();
        ImGui::TextDisabled("Version: " IDHMFIS_VERSION);
        ImGui::TextDisabled("Build: " __DATE__ " " __TIME__);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("About"))
    {
        ImGui::Spacing();
        ImGui::TextUnformatted("IDHMFIS");
        ImGui::TextUnformatted("I Don't Have Money For ILDA Software");
        ImGui::Spacing();
        ImGui::TextUnformatted("Version: " IDHMFIS_VERSION);
        ImGui::TextUnformatted("Build date: " __DATE__ " " __TIME__);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Open-sauced laser control software. "
            "By Onni Kauppinen. "
            "Becouse why should you have to pay to use a shitty projector as a \"laser?\"");
        ImGui::Spacing();
        ImGui::TextDisabled("Created by Onni Kauppinen");
        ImGui::EndTabItem();
    }

    // ── Tab: Advanced ─────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem("Advanced"))
    {
        ImGui::Spacing();

        ImGui::SeparatorText("Otaniemi Mode");
        ImGui::TextWrapped(
            "Optimized for video projectors. Applies to NDI and HDMI outputs only. "
            "Produces filled shapes, thick visible lines, and high contrast output.");
        ImGui::Spacing();

        auto& ot = state.otaniemi;
        bool ot_changed = false;

        ot_changed |= ImGui::Checkbox("Enable Otaniemi Mode", &ot.enabled);
        ImGui::SetItemTooltip(
            "Optimized for video projectors. Applies to NDI and HDMI outputs only.\n"
            "Increases line thickness, fills shapes, and boosts brightness for projector display.");

        ImGui::BeginDisabled(!ot.enabled);

        ImGui::Spacing();

        ImGui::Text("Line Thickness:");
        ImGui::SameLine(200.f);
        ImGui::SetNextItemWidth(180.f);
        ot_changed |= ImGui::SliderFloat("##ot_thick", &ot.line_thickness,
                                          1.0f, 20.0f, "%.1f px",
                                          ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Stroke width for laser lines in pixels. Default: 4.0 px.");

        ImGui::Text("Brightness Boost:");
        ImGui::SameLine(200.f);
        ImGui::SetNextItemWidth(180.f);
        ot_changed |= ImGui::SliderFloat("##ot_bright", &ot.brightness_boost,
                                          1.0f, 3.0f, "%.2fx",
                                          ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Multiplies final RGB values. 1.0 = no change, 3.0 = max. Default: 1.3.");

        ImGui::Text("Glow Radius:");
        ImGui::SameLine(200.f);
        ImGui::SetNextItemWidth(180.f);
        ot_changed |= ImGui::SliderFloat("##ot_glow", &ot.glow_radius,
                                          0.0f, 30.0f, "%.1f px",
                                          ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Glow/bloom radius in pixels. 0 = no glow. Default: 8.0 px.");

        ImGui::Spacing();
        ot_changed |= ImGui::Checkbox("Auto-Fill Shapes##ot_fill", &ot.auto_fill_shapes);
        ImGui::SetItemTooltip(
            "Fills closed shapes instead of scanning outlines.\n"
            "Uses a scanline algorithm on the rendered bitmap after drawing.");

        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Apply##ot_apply", ImVec2(80.f, 0.f))) {
            // Merge Otaniemi settings into raster config and send to engine.
            if (cbs.on_setup_beam_params)
                cbs.on_setup_beam_params(ctx.beam_thickness,
                                          state.output_config.glow_radius,
                                          state.output_config.glow_alpha);
        }
        ImGui::SetItemTooltip(
            "Apply Otaniemi settings to NDI/HDMI output.\n"
            "Also re-applies beam render parameters.");

        (void)ot_changed;  // settings are applied on Apply button press

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SeparatorText("Developer");
        ImGui::Spacing();

        {
            // Amber [!] warning icon before developer logging label
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(theme::kYellow.x, theme::kYellow.y, theme::kYellow.z, theme::kYellow.w));
            ImGui::TextUnformatted("[!]");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f, 6.f);

            bool dev_log = ctx.developer_logging;
            if (ImGui::Checkbox("Developer logging", &dev_log)) {
                ctx.developer_logging = dev_log;
                if (cbs.on_dev_logging_changed)
                    cbs.on_dev_logging_changed(dev_log);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Write a detailed session log to Logs/session.log next to the executable.\n"
                    "The file is overwritten each launch. Default: OFF.");
        }

        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();

    // Pop panel-level style vars
    ImGui::PopStyleVar(2);
}

// ─────────────────────────────────────────────────────────────────────────────
//  panel_setup_window — floating popup, opened via File > Setup... menu item.
//  Does NOT steal focus on every frame (that caused a UI-wide input freeze).
// ─────────────────────────────────────────────────────────────────────────────
void panel_setup_window(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs)
{
    if (!ctx.setup_window_open)
        return;

    // No SetNextWindowFocus here — calling it every frame stole focus from all
    // other panels, making the rest of the UI completely unresponsive.
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.f, 380.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(620.f, 560.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(200.f, 120.f), ImGuiCond_FirstUseEver);

    bool window_open = ctx.setup_window_open;
    if (!ImGui::Begin("Setup##setup_window", &window_open, ImGuiWindowFlags_NoCollapse))
    {
        ctx.setup_window_open = window_open;
        ImGui::End();
        return;
    }
    ctx.setup_window_open = window_open;

    // When the X button closes this window while in Setup view mode, fall back
    // to Programmer so the transport bar view buttons are effective again.
    if (!window_open && ctx.active_view == LayoutContext::ViewMode::Setup)
        ctx.active_view = LayoutContext::ViewMode::Programmer;

    panel_setup_content(state, ctx, cbs);
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  panel_safety_content — full Safety view (no surrounding tab bar).
//  Covers: scan-fail protection, emergency shutoff, BAM enable,
//  and global blackout zones with preview canvas.
// ─────────────────────────────────────────────────────────────────────────────
void panel_safety_content(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs)
{
    (void)ctx;
    ImGui::BeginChild("##safety_scroll", ImVec2(0.f, 0.f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // ── PANIC BUTTON ──────────────────────────────────────────────────────────
    {
        bool shutoff = state.emergency_shutoff_active;
        float btn_w = ImGui::GetContentRegionAvail().x;
        float btn_h = 60.f;
        if (shutoff) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.90f, 0.05f, 0.05f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.10f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.70f, 0.03f, 0.03f, 1.f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.25f, 0.05f, 0.05f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.08f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.90f, 0.10f, 0.10f, 1.f));
        }
        if (shutoff) {
            ImGui::BeginDisabled();
            ImGui::Button("EMERGENCY SHUTOFF \xe2\x80\x94 ACTIVE \xe2\x80\x94 Press Ctrl+Alt+Enter to release",
                          ImVec2(btn_w, btn_h));
            ImGui::EndDisabled();
            ImGui::PopStyleColor(3);
        } else {
            if (ImGui::Button("EMERGENCY SHUTOFF", ImVec2(btn_w, btn_h))) {
                state.emergency_shutoff_active = true;
                if (cbs.on_emergency_shutoff) cbs.on_emergency_shutoff(true);
            }
            ImGui::PopStyleColor(3);
        }
        ImGui::SetItemTooltip(
            "Instantly blanks ALL laser output.\n"
            "Press Ctrl+Alt+Enter to release.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Emergency Shutoff Key");
    ImGui::Spacing();

    {
        const char* key_name = (state.emergency_shutoff_key != 0)
            ? ImGui::GetKeyName(static_cast<ImGuiKey>(state.emergency_shutoff_key))
            : "(unbound)";
        ImGui::Text("Shutoff Key:");
        ImGui::SameLine(140.f);
        ImGui::Text("%s", key_name);
        ImGui::SameLine();
        if (state.emergency_key_capturing) {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.1f, 1.f), "Press a key...");
            for (int k = ImGuiKey_Tab; k < ImGuiKey_COUNT; ++k) {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) {
                    state.emergency_shutoff_key = k;
                    state.emergency_key_capturing = false;
                    break;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                state.emergency_key_capturing = false;
        } else {
            if (ImGui::SmallButton("Bind##emkey")) state.emergency_key_capturing = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##emkey")) state.emergency_shutoff_key = 0;
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Safety Status");
    ImGui::Spacing();

    {
        bool scan_fail_triggered = (state.bam.safety_status == "SCAN FAIL");
        bool interlock           = (state.bam.safety_status == "INTERLOCK");
        bool safety_ok           = state.bam.safety_ok;

        if (safety_ok) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.9f, 0.2f, 1.f));
            ImGui::TextUnformatted("  OK — all safety systems nominal");
            ImGui::PopStyleColor();
        } else if (scan_fail_triggered) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.2f, 0.2f, 1.f));
            ImGui::TextUnformatted("  SCAN FAIL — laser output killed");
            ImGui::PopStyleColor();
        } else if (interlock) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.1f, 1.f));
            ImGui::TextUnformatted("  INTERLOCK — e-stop or key switch open");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("  %s", state.bam.safety_status.c_str());
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Scan Fail Protection");
        ImGui::Spacing();

        ImGui::TextWrapped(
            "Scan Fail protection watches the laser output coordinates every engine tick "
            "(1000 Hz). If the beam position does not move enough for 100 ms, the laser "
            "is killed immediately to prevent burn damage to the audience or equipment.");
        ImGui::Spacing();

        static bool s_scan_fail_enabled = false;
        if (ImGui::Checkbox("Scan Fail Protection Enabled##sf_en", &s_scan_fail_enabled)) {
            if (cbs.on_scan_fail_enabled) cbs.on_scan_fail_enabled(s_scan_fail_enabled);
        }
        ImGui::SetItemTooltip(
            "When enabled, laser output is killed if the beam is stationary for >100 ms.\n"
            "Disable when using NDI-only output or during development without laser hardware.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (!scan_fail_triggered) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.08f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.10f, 0.10f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.05f, 0.05f, 1.f));
        if (ImGui::Button("RESET SCAN FAIL##sf_reset", ImVec2(160.f, 0.f))) {
            if (cbs.on_reset_scan_fail) cbs.on_reset_scan_fail();
        }
        ImGui::PopStyleColor(3);
        if (!scan_fail_triggered) ImGui::EndDisabled();
        ImGui::SetItemTooltip(
            "Clear the scan-fail latch and resume laser output.\n"
            "Resolve the root cause first.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Beam Attenuation Map");
    ImGui::Spacing();

    {
        bool bam_en = state.bam.enabled;
        ImGui::Text("BAM Enabled:");
        ImGui::SameLine(130.f);
        if (ImGui::Checkbox("##bam_en_safety", &bam_en)) {
            if (cbs.on_bam_enabled) cbs.on_bam_enabled(bam_en);
        }
        ImGui::SetItemTooltip(
            "Enable the Beam Attenuation Map. Output brightness is "
            "scaled per cell according to the 64x64 BAM grid.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Safety Blackout Zones");
    ImGui::Spacing();

    ImGui::TextWrapped(
        "Safety blackout zones BLANK laser output hard — enforced at the very end "
        "of the output chain. Use this to protect audience areas, performers, or "
        "equipment from accidental illumination.");
    ImGui::Spacing();

    auto& sbo = state.safety_blackout;
    bool sbo_changed = false;

    ImGui::PushStyleColor(ImGuiCol_Text,
        sbo.enabled ? ImVec4(0.f, 1.f, 0.45f, 1.f) : ImVec4(0.8f, 0.2f, 0.2f, 1.f));
    if (ImGui::Checkbox("Enable Safety Blackout##sbo", &sbo.enabled)) sbo_changed = true;
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Master on/off for all safety blackout zones and border crops.");
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Border Crops##sbo_border", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent(8.f);
        ImGui::TextDisabled("Crop each edge inward (0 = no crop, 1.0 = full field).");
        ImGui::Spacing();
        ImGui::Text("Left:");   ImGui::SameLine(90.f); ImGui::SetNextItemWidth(200.f);
        if (ImGui::SliderFloat("##sbo_left",   &sbo.borders.left,   0.f, 1.0f, "%.3f")) sbo_changed = true;
        ImGui::Text("Right:");  ImGui::SameLine(90.f); ImGui::SetNextItemWidth(200.f);
        if (ImGui::SliderFloat("##sbo_right",  &sbo.borders.right,  0.f, 1.0f, "%.3f")) sbo_changed = true;
        ImGui::Text("Top:");    ImGui::SameLine(90.f); ImGui::SetNextItemWidth(200.f);
        if (ImGui::SliderFloat("##sbo_top",    &sbo.borders.top,    0.f, 1.0f, "%.3f")) sbo_changed = true;
        ImGui::Text("Bottom:"); ImGui::SameLine(90.f); ImGui::SetNextItemWidth(200.f);
        if (ImGui::SliderFloat("##sbo_bottom", &sbo.borders.bottom, 0.f, 1.0f, "%.3f")) sbo_changed = true;
        ImGui::Text("Tilt:");   ImGui::SameLine(90.f); ImGui::SetNextItemWidth(200.f);
        if (ImGui::SliderFloat("##sbo_tilt",   &sbo.borders.tilt,   -45.f, 45.f, "%.1f deg")) sbo_changed = true;
        ImGui::SetItemTooltip("Rotate the border crop boundary for tilted projection surfaces.");
        ImGui::Unindent(8.f);
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Block Zones##sbo_zones", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent(8.f);

        if (ImGui::Button("Add Zone##sbo_add")) {
            UIState::SafetyBlackoutConfig::BlockZone z;
            z.name = "Zone " + std::to_string(static_cast<int>(sbo.zones.size()) + 1);
            sbo.zones.push_back(z);
            sbo_changed = true;
        }
        ImGui::SetItemTooltip("Add a new rectangular safety blackout block zone.");

        ImGui::Spacing();

        int remove_idx = -1;
        for (int zi = 0; zi < static_cast<int>(sbo.zones.size()); ++zi)
        {
            auto& z = sbo.zones[static_cast<size_t>(zi)];
            ImGui::PushID(zi);

            if (ImGui::Checkbox("##sbo_zen", &z.enabled)) sbo_changed = true;
            ImGui::SameLine();
            {
                char nbuf[64];
                std::strncpy(nbuf, z.name.c_str(), sizeof(nbuf) - 1);
                nbuf[sizeof(nbuf) - 1] = '\0';
                ImGui::SetNextItemWidth(120.f);
                if (ImGui::InputText("##sbo_zname", nbuf, sizeof(nbuf))) {
                    z.name = nbuf;
                    sbo_changed = true;
                }
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.15f, 0.15f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.4f, 0.05f, 0.05f, 1.f));
            if (ImGui::Button("Remove##sbo_zrm")) remove_idx = zi;
            ImGui::PopStyleColor(3);

            if (remove_idx != zi)
            {
                ImGui::Indent(20.f);
                ImGui::Text("CX:"); ImGui::SameLine(50.f); ImGui::SetNextItemWidth(160.f);
                if (ImGui::SliderFloat("##sbo_zcx", &z.cx, -1.f, 1.f, "%.3f")) sbo_changed = true;
                ImGui::Text("CY:"); ImGui::SameLine(50.f); ImGui::SetNextItemWidth(160.f);
                if (ImGui::SliderFloat("##sbo_zcy", &z.cy, -1.f, 1.f, "%.3f")) sbo_changed = true;
                ImGui::Text("W:");  ImGui::SameLine(50.f); ImGui::SetNextItemWidth(160.f);
                if (ImGui::SliderFloat("##sbo_zhw", &z.hw, 0.f, 1.f, "hw=%.3f")) sbo_changed = true;
                ImGui::Text("H:");  ImGui::SameLine(50.f); ImGui::SetNextItemWidth(160.f);
                if (ImGui::SliderFloat("##sbo_zhh", &z.hh, 0.f, 1.f, "hh=%.3f")) sbo_changed = true;
                ImGui::Text("Angle:"); ImGui::SameLine(50.f); ImGui::SetNextItemWidth(160.f);
                if (ImGui::SliderFloat("##sbo_zang", &z.angle_deg, -180.f, 180.f, "%.1f deg")) sbo_changed = true;
                ImGui::Unindent(20.f);
            }

            ImGui::Separator();
            ImGui::PopID();
        }

        if (remove_idx >= 0) {
            sbo.zones.erase(sbo.zones.begin() + remove_idx);
            sbo_changed = true;
        }

        ImGui::Unindent(8.f);
    }

    ImGui::Spacing();

    // ── Visualization canvas ───────────────────────────────────────────────────
    ImGui::SeparatorText("Preview");
    ImGui::Spacing();
    {
        const float canvas_sz = 240.f;
        ImVec2 canvas_p = ImGui::GetCursorScreenPos();
        ImDrawList* dl  = ImGui::GetWindowDrawList();

        dl->AddRectFilled(canvas_p,
                          ImVec2(canvas_p.x + canvas_sz, canvas_p.y + canvas_sz),
                          IM_COL32(20, 20, 30, 255));
        dl->AddRect(canvas_p,
                    ImVec2(canvas_p.x + canvas_sz, canvas_p.y + canvas_sz),
                    IM_COL32(80, 80, 100, 255));

        auto to_canvas = [&](float nx_, float ny_) -> ImVec2 {
            return ImVec2(canvas_p.x + (nx_ + 1.f) * 0.5f * canvas_sz,
                          canvas_p.y + (ny_ + 1.f) * 0.5f * canvas_sz);
        };

        ImVec2 ctr = to_canvas(0.f, 0.f);
        dl->AddLine(ImVec2(canvas_p.x, ctr.y), ImVec2(canvas_p.x + canvas_sz, ctr.y),
                    IM_COL32(60, 60, 80, 200));
        dl->AddLine(ImVec2(ctr.x, canvas_p.y), ImVec2(ctr.x, canvas_p.y + canvas_sz),
                    IM_COL32(60, 60, 80, 200));

        if (sbo.enabled) {
            const auto& bdr = sbo.borders;
            const float L = bdr.left   * canvas_sz;
            const float R = bdr.right  * canvas_sz;
            const float T = bdr.top    * canvas_sz;
            const float B = bdr.bottom * canvas_sz;
            const ImU32 crop_col = IM_COL32(200, 40, 40, 100);
            if (L > 0.f) dl->AddRectFilled(canvas_p, ImVec2(canvas_p.x + L, canvas_p.y + canvas_sz), crop_col);
            if (R > 0.f) dl->AddRectFilled(ImVec2(canvas_p.x + canvas_sz - R, canvas_p.y), ImVec2(canvas_p.x + canvas_sz, canvas_p.y + canvas_sz), crop_col);
            if (T > 0.f) dl->AddRectFilled(canvas_p, ImVec2(canvas_p.x + canvas_sz, canvas_p.y + T), crop_col);
            if (B > 0.f) dl->AddRectFilled(ImVec2(canvas_p.x, canvas_p.y + canvas_sz - B), ImVec2(canvas_p.x + canvas_sz, canvas_p.y + canvas_sz), crop_col);

            static constexpr float kPi = 3.14159265358979323846f;
            for (const auto& z : sbo.zones) {
                if (!z.enabled) continue;
                const float ang = z.angle_deg * kPi / 180.f;
                const float cs  = std::cos(ang);
                const float sn  = std::sin(ang);
                float corners[4][2] = {
                    { -z.hw, -z.hh }, {  z.hw, -z.hh },
                    {  z.hw,  z.hh }, { -z.hw,  z.hh }
                };
                ImVec2 pts_cv[4];
                for (int ci = 0; ci < 4; ++ci) {
                    float rx = corners[ci][0] * cs - corners[ci][1] * sn + z.cx;
                    float ry = corners[ci][0] * sn + corners[ci][1] * cs + z.cy;
                    pts_cv[ci] = to_canvas(rx, ry);
                }
                dl->AddConvexPolyFilled(pts_cv, 4, IM_COL32(220, 40, 40, 140));
                dl->AddPolyline(pts_cv, 4, IM_COL32(255, 80, 80, 220), ImDrawFlags_Closed, 1.5f);
            }
        }

        ImGui::Dummy(ImVec2(canvas_sz, canvas_sz));
    }

    if (sbo_changed && cbs.on_safety_blackout_changed)
        cbs.on_safety_blackout_changed(sbo);

    ImGui::EndChild();
}

} // namespace idhmfis
