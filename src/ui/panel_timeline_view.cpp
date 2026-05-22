// panel_timeline_view.cpp — Graphical timeline editor for the Show view.
// Displays track lanes, event blocks, playhead, and transport controls.

#include "layout.h"
#include "theme.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "../timeline/timeline_types.h"
#include "../project/project.h"
#include "../audio/timeline_audio_player.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <string>

#ifdef IDHMFIS_WINDOWS
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#   include <commdlg.h>
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Event color by type
// ─────────────────────────────────────────────────────────────────────────────
ImVec4 event_color(TimelineEventType t)
{
    switch (t) {
    case TimelineEventType::CueGo:            return theme::kAccent;
    case TimelineEventType::PlaybackGo:       return theme::kGreen;
    case TimelineEventType::PlaybackActivate: return theme::kGreen;
    case TimelineEventType::PlaybackRelease:  return theme::kTextDim;
    case TimelineEventType::SetLevel:         return theme::kWarm;
    case TimelineEventType::Flash:            return theme::kRed;
    case TimelineEventType::Command:          return theme::kWarm;
    case TimelineEventType::Marker:           return theme::kTextDim;
    case TimelineEventType::WaitForGo:        return theme::kYellow;
    }
    return { 0.5f, 0.5f, 0.5f, 1.f };
}

const char* event_type_label(TimelineEventType t)
{
    switch (t) {
    case TimelineEventType::CueGo:            return "CueGo";
    case TimelineEventType::PlaybackGo:       return "PBGo";
    case TimelineEventType::PlaybackActivate: return "PBOn";
    case TimelineEventType::PlaybackRelease:  return "PBOff";
    case TimelineEventType::SetLevel:         return "Level";
    case TimelineEventType::Flash:            return "Flash";
    case TimelineEventType::Command:          return "Cmd";
    case TimelineEventType::Marker:           return "Mark";
    case TimelineEventType::WaitForGo:        return "Wait";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
//  open_audio_file_dialog — shows a native OS file picker for audio files.
//  Returns an absolute UTF-8 path, or empty string if the user cancelled.
// ─────────────────────────────────────────────────────────────────────────────
static std::string open_audio_file_dialog()
{
#ifdef IDHMFIS_WINDOWS
    OPENFILENAMEW ofn{};
    wchar_t       buf[MAX_PATH] = {};

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = nullptr;
    ofn.lpstrFilter = L"Audio Files\0*.wav;*.mp3;*.flac;*.ogg\0All Files\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle  = L"Select Audio File";

    if (!GetOpenFileNameW(&ofn)) return {};

    // Convert UTF-16 to UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &result[0], len, nullptr, nullptr);
    return result;
#else
    // Non-Windows: no native dialog — caller should use an ImGui file browser if available.
    return {};
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  draw_timeline_settings_popup — gear button + settings popup
// ─────────────────────────────────────────────────────────────────────────────
static void draw_timeline_settings_popup(const UIState::TimelineInfo& tl,
                                          LayoutCallbacks& cbs)
{
    // Gear button
    if (ImGui::Button("##tl_gear", { 22.f, 22.f })) {
        ImGui::OpenPopup("##tl_settings");
    }
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Timeline settings");
    // Draw a gear icon using a filled concave polygon (teeth) + hub circle
    {
        ImVec2 btn_min = ImGui::GetItemRectMin();
        ImVec2 btn_max = ImGui::GetItemRectMax();
        ImVec2 c = { (btn_min.x + btn_max.x) * 0.5f,
                     (btn_min.y + btn_max.y) * 0.5f };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 col = ImGui::ColorConvertFloat4ToU32(theme::kTextDim);

        // Gear parameters
        constexpr int   N          = 7;      // number of teeth
        constexpr float r_out      = 7.0f;   // outer radius (tooth tip)
        constexpr float r_in       = 5.2f;   // inner radius (tooth valley)
        constexpr float r_hub      = 2.3f;   // hub hole radius
        constexpr float tooth_frac = 0.38f;  // fraction of slot occupied by tooth

        // Build concave gear polygon: 4 verts per tooth
        // [outer-left, outer-right, inner-right, inner-left] × N
        constexpr int kPts = 4 * N;
        ImVec2 pts[kPts];
        const float slot = 2.f * IM_PI / (float)N;
        const float th   = slot * tooth_frac * 0.5f; // half tooth arc
        for (int i = 0; i < N; ++i) {
            float base = (float)i * slot - IM_PI * 0.5f; // start from top
            float next = base + slot;
            pts[4*i+0] = { c.x + r_out * cosf(base + th),
                           c.y + r_out * sinf(base + th) };
            pts[4*i+1] = { c.x + r_out * cosf(next - th),
                           c.y + r_out * sinf(next - th) };
            pts[4*i+2] = { c.x + r_in  * cosf(next - slot * 0.1f),
                           c.y + r_in  * sinf(next - slot * 0.1f) };
            pts[4*i+3] = { c.x + r_in  * cosf(base + slot * 0.1f),
                           c.y + r_in  * sinf(base + slot * 0.1f) };
        }
        dl->AddConcavePolyFilled(pts, kPts, col);

        // Hub hole (draw with background colour to punch out centre)
        ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_Button));
        dl->AddCircleFilled(c, r_hub, bg, 12);
    }

    ImGui::SetNextWindowSize({ 320.f, 0.f }, ImGuiCond_Always);
    if (!ImGui::BeginPopup("##tl_settings")) return;

    ImGui::TextColored(theme::kAccent, "Timeline Settings");
    ImGui::Separator();

    // ── Rename ───────────────────────────────────────────────────────────────
    static char s_rename[128] = {};
    static bool s_rename_init = false;
    if (!s_rename_init) {
        std::snprintf(s_rename, sizeof(s_rename), "%s", tl.name.c_str());
        s_rename_init = true;
    }
    ImGui::SetNextItemWidth(200.f);
    if (ImGui::InputText("Name", s_rename, sizeof(s_rename),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (cbs.on_timeline_rename)
            cbs.on_timeline_rename(tl.id, s_rename);
        s_rename_init = false;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        s_rename_init = false;
    }

    // Reset name buffer when popup closes
    if (!ImGui::IsPopupOpen("##tl_settings")) s_rename_init = false;

    // ── FPS ──────────────────────────────────────────────────────────────────
    static const char* kFpsItems[]  = { "24", "25", "29.97", "30" };
    static const SmpteRate kFpsVals[] = {
        SmpteRate::Fps24, SmpteRate::Fps25,
        SmpteRate::Fps2997, SmpteRate::Fps30
    };
    int fps_idx = 1; // default 25
    for (int i = 0; i < 4; ++i)
        if (kFpsVals[i] == tl.fps) { fps_idx = i; break; }

    ImGui::SetNextItemWidth(100.f);
    if (ImGui::Combo("FPS", &fps_idx, kFpsItems, 4)) {
        // FPS change is handled by re-creating / updating the timeline;
        // for now we notify the rename callback since there's no dedicated
        // FPS callback — in practice the operator changes FPS at creation time.
        // A proper SetTimelineFps command can be added later.
        (void)fps_idx;
    }

    // ── TC Source ────────────────────────────────────────────────────────────
    static const char* kSrcItems[] = { "Internal", "LTC", "MIDI" };
    int src_idx = 0;
    if (tl.tc_slot == "CH1") src_idx = 1;
    else if (tl.tc_slot == "CH2") src_idx = 2;
    // "Internal" and legacy "Default" both map to src_idx=0

    ImGui::SetNextItemWidth(120.f);
    if (ImGui::Combo("TC Source", &src_idx, kSrcItems, 3)) {
        // Use "Internal" (not "Default") so the engine's tc_slot == "Internal" check works
        static const char* kSlots[] = { "Internal", "CH1", "CH2" };
        if (cbs.on_timeline_set_source)
            cbs.on_timeline_set_source(tl.id, kSlots[src_idx]);
        // Auto-set link_mode: Internal → true (internal clock), external → false (TC chase)
        if (cbs.on_timeline_set_link)
            cbs.on_timeline_set_link(tl.id, (src_idx == 0));
    }

    ImGui::Separator();
    ImGui::TextColored(theme::kAccent, "Audio Track");
    ImGui::Spacing();

    if (!tl.audio_track.has_value()) {
        // No audio loaded
        if (ImGui::Button("Add Audio File")) {
            std::string path = open_audio_file_dialog();
            if (!path.empty() && cbs.on_timeline_set_audio) {
                AudioTrackDef at;
                at.file_path = path;
                cbs.on_timeline_set_audio(tl.id, std::move(at));
                ImGui::CloseCurrentPopup();
            }
        }
    } else {
        // Audio loaded — show filename and controls
        const auto& at = tl.audio_track.value();
        // Extract just the filename portion for display
        std::string fname = at.file_path;
        auto slash_pos = fname.find_last_of("/\\");
        if (slash_pos != std::string::npos)
            fname = fname.substr(slash_pos + 1);

        ImGui::TextDisabled("File:");
        ImGui::SameLine();
        ImGui::TextUnformatted(fname.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetItemTooltip("%s", at.file_path.c_str());

        // Volume slider
        static float s_vol = 1.0f;
        static bool  s_vol_init = false;
        if (!s_vol_init) { s_vol = at.volume; s_vol_init = true; }
        ImGui::SetNextItemWidth(180.f);
        if (ImGui::SliderFloat("Volume##audvol", &s_vol, 0.f, 1.f, "%.0f%%",
                                ImGuiSliderFlags_None)) {
            // Re-apply audio settings on change
            if (cbs.on_timeline_set_audio) {
                AudioTrackDef updated = at;
                updated.volume = s_vol;
                cbs.on_timeline_set_audio(tl.id, std::move(updated));
            }
        }
        // Format as 0-100%
        // (ImGui SliderFloat with "%.0f%%" already shows as percentage)
        s_vol_init = false;  // re-read on next frame in case engine updated it

        ImGui::Spacing();
        if (ImGui::Button("Remove##aud_remove") && cbs.on_timeline_clear_audio) {
            cbs.on_timeline_clear_audio(tl.id);
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Draw overview bar (miniature of the whole timeline)
// ─────────────────────────────────────────────────────────────────────────────
static void draw_overview(const UIState::TimelineInfo& tl,
                          int64_t pos,
                          LayoutContext& ctx,
                          ImVec2 origin, float width, float height)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end = { origin.x + width, origin.y + height };

    dl->AddRectFilled(origin, end, ImGui::ColorConvertFloat4ToU32(theme::kBg0));
    dl->AddRect(origin, end, ImGui::ColorConvertFloat4ToU32(theme::kBorder));

    if (tl.length_frames <= 0) return;

    // Playhead
    float ph = static_cast<float>(pos) / static_cast<float>(tl.length_frames);
    float px = origin.x + ph * width;
    dl->AddLine({ px, origin.y }, { px, end.y },
               ImGui::ColorConvertFloat4ToU32(theme::kAccent), 1.5f);

    // Click to scroll the main view
    if (ImGui::IsMouseHoveringRect(origin, end) && ImGui::IsMouseClicked(0)) {
        float mx = ImGui::GetMousePos().x - origin.x;
        float frac = std::clamp(mx / width, 0.f, 1.f);
        ctx.timeline_view_scroll =
            static_cast<double>(frac * static_cast<float>(tl.length_frames));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Add event popup
// ─────────────────────────────────────────────────────────────────────────────
static void draw_add_event_popup(const std::string& timeline_id,
                                  int track_id,
                                  int64_t insert_frame,
                                  SmpteRate fps,
                                  LayoutCallbacks& cbs)
{
    static TimelineEventType s_type   = TimelineEventType::Marker;
    static char              s_target[128] = {};
    static char              s_label[128]  = {};

    ImGui::SetNextWindowSize({ 300.f, 0.f }, ImGuiCond_Always);
    if (ImGui::BeginPopup("##add_event")) {
        ImGui::Text("Add Event at %s", tc_to_string(insert_frame, fps).c_str());
        ImGui::Separator();

        static const char* type_names[] = {
            "CueGo","PlaybackGo","PlaybackActivate","PlaybackRelease",
            "SetLevel","Flash","Command","Marker","WaitForGo"
        };
        int type_idx = static_cast<int>(s_type);
        ImGui::SetNextItemWidth(180.f);
        if (ImGui::Combo("Type", &type_idx, type_names,
                         static_cast<int>(std::size(type_names))))
            s_type = static_cast<TimelineEventType>(type_idx);

        ImGui::SetNextItemWidth(180.f);
        ImGui::InputText("Target", s_target, sizeof(s_target));

        ImGui::SetNextItemWidth(180.f);
        ImGui::InputText("Label", s_label, sizeof(s_label));

        ImGui::Separator();
        if (ImGui::Button("Add") && cbs.on_timeline_add_event) {
            TimelineEvent ev;
            ev.tc_position = insert_frame;
            ev.type        = s_type;
            ev.target_id   = s_target;
            ev.label       = s_label;
            cbs.on_timeline_add_event(timeline_id, track_id, std::move(ev));
            s_target[0] = '\0';
            s_label[0]  = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  panel_timeline_view
// ─────────────────────────────────────────────────────────────────────────────
void panel_timeline_view(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs)
{
    if (!ctx.timeline_view_open) return;

    // ── Keyboard shortcuts — active when this window is focused ───────────────
    auto tl_key_action = [&](int key_id) -> bool {
        if (key_id == 0 || ImGui::GetIO().WantTextInput) return false;
        return ImGui::IsKeyPressed(static_cast<ImGuiKey>(key_id), /*repeat=*/false);
    };

    const UIState::TimelineInfo* tl_info = nullptr;
    if (!ctx.timeline_view_id.empty()) {
        for (const auto& t : state.timelines)
            if (t.id == ctx.timeline_view_id) { tl_info = &t; break; }
        if (!tl_info) ctx.timeline_view_id.clear();
    }

    std::string win_title = "Timeline Editor##tlv";

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar;
    bool win_open = ctx.timeline_view_open;
    if (!ImGui::Begin(win_title.c_str(), &win_open, flags)) {
        ctx.timeline_view_open = win_open;
        ImGui::End();
        return;
    }
    ctx.timeline_view_open = win_open;

    // Process keyboard shortcuts when window is focused
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && tl_info) {
        const std::string& tid = tl_info->id;
        if (tl_key_action(state.tl_key_play)) {
            if (tl_info->state == TimelineState::Playing) {
                if (cbs.on_timeline_pause) cbs.on_timeline_pause(tid);
            } else {
                if (cbs.on_timeline_play)  cbs.on_timeline_play(tid);
            }
        }
        if (tl_key_action(state.tl_key_stop)   && cbs.on_timeline_stop)
            cbs.on_timeline_stop(tid);
        if (tl_key_action(state.tl_key_rewind)  && cbs.on_timeline_rewind)
            cbs.on_timeline_rewind(tid);
        if (tl_key_action(state.tl_key_go)      && cbs.on_timeline_wait_for_go)
            cbs.on_timeline_wait_for_go(tid);
    }

    if (!tl_info) {
        ImVec2 csz = ImGui::GetContentRegionAvail();
        ImVec2 tp  = { ImGui::GetCursorScreenPos().x + csz.x * 0.5f - 130.f,
                       ImGui::GetCursorScreenPos().y + csz.y * 0.5f };
        ImGui::SetCursorScreenPos(tp);
        ImGui::TextDisabled("Select a timeline from the Timelines panel.");
        ImGui::End();
        return;
    }

    const UIState::TimelineInfo& tl = *tl_info;

    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2      wpos = ImGui::GetWindowPos();
    float       ww   = ImGui::GetWindowSize().x;

    // Timeline name header (stable window title avoids ghost windows on rename)
    ImGui::TextColored(theme::kAccent, "%s", tl.name.c_str());
    ImGui::SameLine(0, 6);
    draw_timeline_settings_popup(tl, cbs);
    ImGui::SameLine(0, 8);

    // ── Transport bar ─────────────────────────────────────────────────────────
    static constexpr ImVec2 kBtnSz  = { 36.f, 24.f };
    static constexpr ImVec2 kBtnPad = { 5.f, 3.f };
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kBtnPad);

        // Rewind
        if (ImGui::Button("|<##rew", kBtnSz) && cbs.on_timeline_rewind)
            cbs.on_timeline_rewind(tl.id);
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Rewind to start");
        ImGui::SameLine(0, 3);

        // Stop
        if (ImGui::Button("[]##stp", kBtnSz) && cbs.on_timeline_stop)
            cbs.on_timeline_stop(tl.id);
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Stop");
        ImGui::SameLine(0, 3);

        // Pause
        bool is_paused  = (tl.state == TimelineState::Paused);
        if (is_paused) ImGui::PushStyleColor(ImGuiCol_Button, theme::kYellow);
        if (ImGui::Button("||##pau", kBtnSz) && cbs.on_timeline_pause)
            cbs.on_timeline_pause(tl.id);
        if (is_paused) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Pause / resume");
        ImGui::SameLine(0, 3);

        // Play
        bool is_playing = (tl.state == TimelineState::Playing);
        if (is_playing) ImGui::PushStyleColor(ImGuiCol_Button, theme::kGreen);
        if (ImGui::Button(">##play", kBtnSz) && cbs.on_timeline_play)
            cbs.on_timeline_play(tl.id);
        if (is_playing) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Play");

        ImGui::SameLine(0, 12);

        // Link mode (Internal ↔ External TC)
        bool lm = tl.link_mode;
        if (!lm) ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        if (ImGui::Button(lm ? "INT##lnk" : "EXT TC##lnk", { 56.f, 24.f })
                && cbs.on_timeline_set_link)
            cbs.on_timeline_set_link(tl.id, !lm);
        if (!lm) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetItemTooltip(lm
                ? "Internal clock — click to slave to external TC"
                : "Slaved to external TC (slot: %s) — click to switch to internal", tl.tc_slot.c_str());

        ImGui::SameLine(0, 8);

        // TC display
        std::string pos_str = tc_to_string(tl.position_frames, tl.fps);
        ImGui::TextColored(theme::kAccent, "%s", pos_str.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetItemTooltip("Current playhead position (TC)");

        ImGui::SameLine(0, 12);

        // ARM REC — prominent, separate from transport
        bool rec = tl.record_armed;
        {
            ImVec4 rec_col = rec ? theme::kRed : ImVec4(0.3f, 0.08f, 0.08f, 1.f);
            ImGui::PushStyleColor(ImGuiCol_Button,        rec_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rec ? theme::kRed : ImVec4(0.45f,0.1f,0.1f,1.f));
            if (ImGui::Button(rec ? "* REC *##rec" : "REC##rec", { 60.f, 24.f })
                    && cbs.on_timeline_set_record_armed)
                cbs.on_timeline_set_record_armed(tl.id, !rec);
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip(rec
                    ? "Recording armed — click to disarm"
                    : "Click to arm for recording");
        }

        ImGui::SameLine(0, 12);

        // Table view toggle
        bool tv = ctx.timeline_view_table;
        if (tv) {
            ImVec4 active_col = theme::kGreen; active_col.w = 0.35f;
            ImGui::PushStyleColor(ImGuiCol_Button, active_col);
        }
        if (ImGui::Button("LIST##tbl", { 38.f, 24.f }))
            ctx.timeline_view_table = !ctx.timeline_view_table;
        if (tv) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Toggle table/list view");

        ImGui::PopStyleVar();
    }

    ImGui::Separator();

    // ── Overview bar ──────────────────────────────────────────────────────────
    {
        float ovh = 14.f;
        ImVec2 ov_orig = ImGui::GetCursorScreenPos();
        float  ov_w    = ImGui::GetContentRegionAvail().x;
        draw_overview(tl, tl.position_frames, ctx, ov_orig, ov_w, ovh);
        ImGui::Dummy({ ov_w, ovh });
    }

    // ── Time ruler ────────────────────────────────────────────────────────────
    const float kLeftPad  = 140.f;  // track header width
    const float kZoom     = ctx.timeline_view_zoom; // px/frame
    float ruler_h         = 20.f;

    {
        ImVec2 ruler_orig = ImGui::GetCursorScreenPos();
        float  ruler_w    = ImGui::GetContentRegionAvail().x - kLeftPad;
        ImVec2 ruler_end  = { ruler_orig.x + kLeftPad + ruler_w,
                               ruler_orig.y + ruler_h };
        dl->AddRectFilled(ruler_orig, ruler_end,
                          ImGui::ColorConvertFloat4ToU32(theme::kBg1));

        // Draw time markers every N frames (adaptive to zoom)
        int fps_int = smpte_max_frames(tl.fps);
        if (fps_int <= 0) fps_int = 25;

        // Short timestamp helper: omits hours/minutes when not needed,
        // and shows frames as sub-second digits for readability at fine zoom.
        static auto short_tc = [](int64_t frames, int fps_i) -> std::string {
            int sec = static_cast<int>(frames / fps_i);
            int fr  = static_cast<int>(frames % fps_i);
            int min = sec / 60; sec %= 60;
            int hr  = min / 60; min %= 60;
            char buf[32];
            if (hr > 0)
                std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", hr, min, sec);
            else if (min > 0)
                std::snprintf(buf, sizeof(buf), "%d:%02d.%02d", min, sec, fr);
            else
                std::snprintf(buf, sizeof(buf), "%d.%02d", sec, fr);
            return buf;
        };

        // Start at 1-second intervals and widen until marks are at least 6px apart.
        int mark_interval = fps_int;
        if (mark_interval < 1) mark_interval = 1;
        {
            float px_per_mark = static_cast<float>(mark_interval) * kZoom;
            while (px_per_mark < 6.f && mark_interval < fps_int * 600) {
                mark_interval *= 2;
                px_per_mark = static_cast<float>(mark_interval) * kZoom;
            }
        }

        // Label at every mark when there's room (>= 50 px between labels),
        // otherwise every 2nd, 4th, … mark.
        int label_every = 1;
        {
            float px_per_mark = static_cast<float>(mark_interval) * kZoom;
            while (static_cast<float>(label_every) * px_per_mark < 50.f)
                label_every *= 2;
        }

        double scroll_frames = ctx.timeline_view_scroll;
        int64_t first_mark = static_cast<int64_t>(scroll_frames / mark_interval)
                           * mark_interval;
        int64_t last_mark  = first_mark + static_cast<int64_t>(
            (ruler_w / kZoom) + mark_interval * 2);

        // Counter for label_every: count mark indices, not raw frames.
        int64_t mark_index_base = (first_mark / mark_interval);

        for (int64_t m = first_mark; m <= last_mark; m += mark_interval) {
            float x = ruler_orig.x + kLeftPad +
                      static_cast<float>(m - scroll_frames) * kZoom;
            if (x < ruler_orig.x + kLeftPad || x > ruler_end.x) continue;

            dl->AddLine({ x, ruler_orig.y + ruler_h * 0.5f },
                        { x, ruler_end.y },
                        ImGui::ColorConvertFloat4ToU32(theme::kBorder), 1.f);

            int64_t mark_idx = (m / mark_interval) - mark_index_base;
            if (mark_idx % static_cast<int64_t>(label_every) == 0) {
                std::string lbl = short_tc(m, fps_int);
                dl->AddText({ x + 2.f, ruler_orig.y + 3.f },
                             ImGui::ColorConvertFloat4ToU32(theme::kTextDim),
                             lbl.c_str());
            }
        }

        // Playhead
        float ph_x = ruler_orig.x + kLeftPad +
                     static_cast<float>(
                         tl.position_frames - scroll_frames) * kZoom;
        if (ph_x >= ruler_orig.x + kLeftPad && ph_x <= ruler_end.x) {
            dl->AddLine({ ph_x, ruler_orig.y },
                        { ph_x, ruler_end.y },
                        ImGui::ColorConvertFloat4ToU32(theme::accent()), 2.f);
        }

        float dummy_w = ruler_orig.x + kLeftPad + ruler_w - wpos.x;
        ImGui::InvisibleButton("##ruler_seek", { dummy_w, ruler_h });
        if (ImGui::IsItemActive() &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
             ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
            float mx = ImGui::GetMousePos().x;
            float rx = ruler_orig.x + kLeftPad;  // ruler content start x
            if (mx >= rx) {
                int64_t clicked_frame = static_cast<int64_t>(
                    (mx - rx) / kZoom + ctx.timeline_view_scroll);
                clicked_frame = std::clamp(clicked_frame, int64_t{0}, tl.length_frames);
                if (cbs.on_timeline_seek)
                    cbs.on_timeline_seek(tl.id, clicked_frame);
            }
        }
        // Scroll/zoom on the ruler area
        if (ImGui::IsItemHovered() && std::fabs(ImGui::GetIO().MouseWheel) > 0.f) {
            const ImGuiIO& rlio = ImGui::GetIO();
            if (rlio.KeyCtrl) {
                float zoom_before = ctx.timeline_view_zoom;
                float factor = (rlio.MouseWheel > 0.f) ? 1.15f : (1.f / 1.15f);
                float zoom_after = std::clamp(zoom_before * factor, 1.f, 400.f);
                float content_x = ImGui::GetMousePos().x - (ruler_orig.x + kLeftPad);
                if (content_x > 0.f) {
                    double mouse_frame = ctx.timeline_view_scroll
                                      + static_cast<double>(content_x) / static_cast<double>(zoom_before);
                    ctx.timeline_view_scroll = mouse_frame
                                            - static_cast<double>(content_x) / static_cast<double>(zoom_after);
                    if (ctx.timeline_view_scroll < 0.0) ctx.timeline_view_scroll = 0.0;
                }
                ctx.timeline_view_zoom = zoom_after;
            } else {
                ctx.timeline_view_scroll -= rlio.MouseWheel * 10.0;
                if (ctx.timeline_view_scroll < 0.0) ctx.timeline_view_scroll = 0.0;
            }
        }
    }

    // ── Audio waveform (shown when a track has been loaded) ───────────────────
    if (tl.audio_track.has_value()) {
        constexpr float kWaveH = 44.f;
        ImVec2 wave_orig = ImGui::GetCursorScreenPos();
        float  wave_w    = ImGui::GetContentRegionAvail().x;
        double scroll    = ctx.timeline_view_scroll;

        dl->AddRectFilled(wave_orig,
                          {wave_orig.x + wave_w, wave_orig.y + kWaveH},
                          IM_COL32(6, 12, 20, 255));
        dl->AddRectFilled(wave_orig,
                          {wave_orig.x + kLeftPad, wave_orig.y + kWaveH},
                          IM_COL32(10, 16, 26, 255));

        // Always show filename in the header region
        {
            const std::string& fp = tl.audio_track->file_path;
            size_t sl = fp.find_last_of("/\\");
            const char* fname = (sl != std::string::npos) ? fp.c_str() + sl + 1
                                                           : fp.c_str();
            dl->AddText({wave_orig.x + 4.f, wave_orig.y + kWaveH * 0.5f - 6.f},
                        IM_COL32(100, 160, 200, 200), fname);
        }

        if (!tl.audio_peaks.empty()) {
            // Draw the real waveform.
            // Map frame positions to peak indices via seconds so this works for
            // both bounded (length_frames > 0) and unbounded (length_frames == 0)
            // timelines.  The peaks array was produced at kAudioPeakRateHz peaks/s.
            int fps_hz = smpte_max_frames(tl.fps);
            if (fps_hz <= 0) fps_hz = 25;
            const double kFpsHz = static_cast<double>(fps_hz);

            float  content_w = wave_w - kLeftPad;
            const float cy    = wave_orig.y + kWaveH * 0.5f;
            const auto& peaks = tl.audio_peaks;
            for (float px = 0.f; px < content_w; px += 1.f) {
                double frame_d = scroll + static_cast<double>(px) / static_cast<double>(kZoom);
                if (frame_d < 0.0) continue;
                // Convert frame position to peak array index via seconds.
                double seconds = frame_d / kFpsHz;
                size_t idx = static_cast<size_t>(seconds * kAudioPeakRateHz);
                if (idx >= peaks.size()) continue;  // past end of audio
                float bar_h = std::max(1.f, peaks[idx] * (kWaveH * 0.85f));
                float x     = wave_orig.x + kLeftPad + px;
                dl->AddLine({x, cy - bar_h * 0.5f}, {x, cy + bar_h * 0.5f},
                            IM_COL32(0, 180, 230, 140), 1.f);
            }
        } else {
            // Peaks not yet available (decode failed or audio not yet loaded).
            float content_x = wave_orig.x + kLeftPad;
            float content_w = wave_w - kLeftPad;
            float cy        = wave_orig.y + kWaveH * 0.5f;
            dl->AddRectFilled({content_x, cy - 4.f},
                              {content_x + content_w, cy + 4.f},
                              IM_COL32(40, 70, 90, 160));
            dl->AddText({content_x + 6.f, cy - 6.f},
                        IM_COL32(120, 160, 180, 200), "No waveform data");
        }

        float ph_wx = wave_orig.x + kLeftPad +
                      static_cast<float>(static_cast<double>(tl.position_frames) - scroll) * kZoom;
        if (ph_wx >= wave_orig.x + kLeftPad && ph_wx <= wave_orig.x + wave_w) {
            dl->AddLine({ph_wx, wave_orig.y}, {ph_wx, wave_orig.y + kWaveH},
                        ImGui::ColorConvertFloat4ToU32(theme::kAccent), 2.f);
        }

        dl->AddRect(wave_orig, {wave_orig.x + wave_w, wave_orig.y + kWaveH},
                    IM_COL32(30, 50, 70, 200));

        ImGui::Dummy({wave_w, kWaveH});
        ImGui::Spacing();
    }

    // ── Track lanes ───────────────────────────────────────────────────────────
    float avail_h   = ImGui::GetContentRegionAvail().y;

    ImGui::BeginChild("##tl_lanes", { 0.f, std::max(avail_h - 60.f, 1.f) },
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_None);
    {
        // Horizontal scroll via horizontal mouse wheel
        if (ImGui::IsWindowHovered() && std::fabs(ImGui::GetIO().MouseWheelH) > 0.f) {
            ctx.timeline_view_scroll -= ImGui::GetIO().MouseWheelH * 10.0;
            if (ctx.timeline_view_scroll < 0.0) ctx.timeline_view_scroll = 0.0;
        }
        if (ImGui::IsWindowHovered() && std::fabs(ImGui::GetIO().MouseWheel) > 0.f) {
            const ImGuiIO& tlio = ImGui::GetIO();
            if (tlio.KeyCtrl) {
                // Ctrl+Scroll: zoom in/out, anchored to mouse position
                float zoom_before = ctx.timeline_view_zoom;
                float factor = (tlio.MouseWheel > 0.f) ? 1.15f : (1.f / 1.15f);
                float zoom_after = std::clamp(zoom_before * factor, 1.f, 400.f);
                // Anchor scroll so the frame under the mouse stays fixed
                float mouse_x  = ImGui::GetMousePos().x - ImGui::GetWindowPos().x;
                float content_x = mouse_x - 140.f; // subtract kLP header
                if (content_x > 0.f) {
                    double mouse_frame = ctx.timeline_view_scroll
                                      + static_cast<double>(content_x) / static_cast<double>(zoom_before);
                    ctx.timeline_view_scroll = mouse_frame
                                            - static_cast<double>(content_x) / static_cast<double>(zoom_after);
                    if (ctx.timeline_view_scroll < 0.0) ctx.timeline_view_scroll = 0.0;
                }
                ctx.timeline_view_zoom = zoom_after;
            } else if (tlio.KeyShift) {
                // Shift+Scroll: horizontal scroll
                ctx.timeline_view_scroll -= tlio.MouseWheel * 10.0;
                if (ctx.timeline_view_scroll < 0.0) ctx.timeline_view_scroll = 0.0;
            } else {
                // Plain scroll: horizontal scroll (timeline has no vertical content)
                ctx.timeline_view_scroll -= tlio.MouseWheel * 10.0;
                if (ctx.timeline_view_scroll < 0.0) ctx.timeline_view_scroll = 0.0;
            }
        }
        const float kLane  = static_cast<float>(ctx.timeline_view_track_h);
        const float kZoom2 = ctx.timeline_view_zoom;
        const float kLP    = 140.f;  // header width (matches ruler)

        if (tl.tracks.empty()) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.f);
            ImGui::TextDisabled("No tracks. Right-click to add a track.");
            ImGui::Spacing();
        }

        ImDrawList* lane_dl = ImGui::GetWindowDrawList();

        // Delete key: remove all selected events from the selected track.
        // Guard with !WantTextInput instead of IsWindowFocused — the ChildWindows
        // flag doesn't work from inside a child window (ImGui nav focus lives on the
        // root window, so IsWindowFocused always returns false here).
        if (!ImGui::GetIO().WantTextInput &&
            (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) &&
            !ctx.timeline_selected_event_ids.empty() &&
            ctx.timeline_selected_track_id >= 0 &&
            cbs.on_timeline_remove_event) {
            for (int64_t tc : ctx.timeline_selected_event_ids)
                cbs.on_timeline_remove_event(tl.id, ctx.timeline_selected_track_id, tc);
            ctx.timeline_selected_event_ids.clear();
        }

        // Persistent state for the right-click context popup (survives across frames
        // while the popup is open).  Updated the frame a right-click on an event is detected.
        static int64_t s_ctx_clicked_tc    = -1;
        static int     s_ctx_clicked_track = -1;

        for (const auto& track : tl.tracks) {
            bool track_sel = (ctx.timeline_selected_track_id == track.id);
            ImVec2 row_pos = ImGui::GetCursorScreenPos();
            (void)row_pos;

            // Header
            ImGui::PushID(track.id);
            if (track.muted) ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
            if (ImGui::Selectable(track.name.c_str(), track_sel,
                                  ImGuiSelectableFlags_None, { kLP - 8.f, kLane })) {
                ctx.timeline_selected_track_id = track.id;
                ctx.timeline_selected_event_ids.clear();
            }
            if (track.muted) ImGui::PopStyleColor();
            ImGui::SameLine(0, 0);

            // Event lane
            ImVec2 lane_orig = ImGui::GetCursorScreenPos();
            float  lane_w    = ImGui::GetContentRegionAvail().x;
            ImVec2 lane_end  = { lane_orig.x + lane_w, lane_orig.y + kLane };
            lane_dl->AddRectFilled(lane_orig, lane_end,
                ImGui::ColorConvertFloat4ToU32(track_sel ? theme::kBg2 : theme::kBg1));

            // Invisible button over the event lane for click/hover detection.
            // Must capture right-click too so BeginPopupContextWindow below
            // doesn't also fire and immediately close our event context popup.
            char lane_id[32];
            std::snprintf(lane_id, sizeof(lane_id), "##evlane_%d", track.id);
            ImGui::InvisibleButton(lane_id, { lane_w, kLane },
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            bool lane_clicked_l = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            bool lane_clicked_r = ImGui::IsItemClicked(ImGuiMouseButton_Right);
            bool lane_hovered   = ImGui::IsItemHovered();
            ImVec2 mouse_pos    = ImGui::GetMousePos();

            // Draw events and detect clicks
            int64_t clicked_tc = -1;  // TC of event clicked this frame (-1 = none)
            for (const auto& ev : track.events) {
                float ex = lane_orig.x +
                           static_cast<float>(ev.tc_position - ctx.timeline_view_scroll)
                           * kZoom2;
                if (ex < lane_orig.x || ex > lane_end.x) continue;

                bool ev_selected = track_sel &&
                    std::find(ctx.timeline_selected_event_ids.begin(),
                              ctx.timeline_selected_event_ids.end(),
                              ev.tc_position) != ctx.timeline_selected_event_ids.end();

                ImU32 ec = ImGui::ColorConvertFloat4ToU32(event_color(ev.type));

                // In REM mode tint every event red so the operator can see it's armed
                if (state.rem_mode)
                    ec = IM_COL32(255, 70, 70, 220);

                // Highlight selected events with a bright background pip
                if (ev_selected) {
                    lane_dl->AddRectFilled({ ex - 3.f, lane_orig.y + 1.f },
                                          { ex + 3.f, lane_end.y  - 1.f },
                                          IM_COL32(255, 80, 80, 50));
                    lane_dl->AddLine({ ex, lane_orig.y + 2.f },
                                     { ex, lane_end.y  - 2.f },
                                     state.rem_mode ? IM_COL32(255,80,80,255) : IM_COL32(255,255,255,220),
                                     3.f);
                } else {
                    lane_dl->AddLine({ ex, lane_orig.y + 2.f },
                                     { ex, lane_end.y  - 2.f }, ec, 2.f);
                }
                const char* lbl = event_type_label(ev.type);
                lane_dl->AddText({ ex + 3.f, lane_orig.y + 2.f }, ec, lbl);

                // Detect click proximity (within 10px)
                if ((lane_clicked_l || lane_clicked_r) && lane_hovered &&
                    std::fabs(mouse_pos.x - ex) < 10.f)
                    clicked_tc = ev.tc_position;
            }

            // Handle left-click on an event
            if (lane_clicked_l && clicked_tc >= 0) {
                if (ctx.timeline_selected_track_id != track.id) {
                    ctx.timeline_selected_track_id = track.id;
                    ctx.timeline_selected_event_ids.clear();
                }
                if (state.rem_mode && cbs.on_timeline_remove_event) {
                    cbs.on_timeline_remove_event(tl.id, track.id, clicked_tc);
                    state.rem_mode = false;
                } else {
                    bool shift = ImGui::GetIO().KeyShift;
                    auto& sel = ctx.timeline_selected_event_ids;
                    auto it = std::find(sel.begin(), sel.end(), clicked_tc);
                    if (shift) {
                        if (it != sel.end()) sel.erase(it);  // toggle off
                        else                 sel.push_back(clicked_tc);
                    } else {
                        sel.clear();
                        sel.push_back(clicked_tc);
                    }
                }
            } else if (lane_clicked_l && clicked_tc < 0) {
                // Clicked empty lane area — clear event selection, switch track
                ctx.timeline_selected_track_id = track.id;
                ctx.timeline_selected_event_ids.clear();
            }

            // Right-click: update persistent popup state and select the event
            if (lane_clicked_r && clicked_tc >= 0) {
                s_ctx_clicked_tc    = clicked_tc;
                s_ctx_clicked_track = track.id;
                ctx.timeline_selected_track_id = track.id;
                auto& sel = ctx.timeline_selected_event_ids;
                if (std::find(sel.begin(), sel.end(), clicked_tc) == sel.end()) {
                    sel.clear();
                    sel.push_back(clicked_tc);
                }
            } else if (lane_clicked_r) {
                // Right-clicked empty lane — clear the event context
                s_ctx_clicked_tc = -1;
            }

            // Context popup — uses BeginPopupContextItem so it's properly associated
            // with the InvisibleButton (which now captures right-click).  Using
            // BeginPopupContextItem instead of OpenPopup+BeginPopup avoids the bug
            // where BeginPopupContextWindow would fire simultaneously and close us.
            if (ImGui::BeginPopupContextItem("##ev_lane_ctx")) {
                if (s_ctx_clicked_tc >= 0 &&
                    !ctx.timeline_selected_event_ids.empty() &&
                    ctx.timeline_selected_track_id == s_ctx_clicked_track &&
                    cbs.on_timeline_remove_event) {
                    int n = static_cast<int>(ctx.timeline_selected_event_ids.size());
                    char del_label[64];
                    std::snprintf(del_label, sizeof(del_label),
                                  n == 1 ? "Delete Event" : "Delete %d Events", n);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                    if (ImGui::MenuItem(del_label)) {
                        for (int64_t tc : ctx.timeline_selected_event_ids)
                            cbs.on_timeline_remove_event(tl.id, ctx.timeline_selected_track_id, tc);
                        ctx.timeline_selected_event_ids.clear();
                        s_ctx_clicked_tc = -1;
                    }
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextDisabled("Right-click near an event to delete it");
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        // Right-click to add track on empty space
        if (ImGui::BeginPopupContextWindow("##tl_ctx_empty")) {
            if (ImGui::MenuItem("Add Track") && cbs.on_timeline_add_track) {
                int n = static_cast<int>(tl.tracks.size()) + 1;
                char name[32];
                std::snprintf(name, sizeof(name), "Track %d", n);
                cbs.on_timeline_add_track(tl.id, name);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    // ── Inspector / zoom controls ─────────────────────────────────────────────
    ImGui::Separator();
    {
        ImGui::SetNextItemWidth(120.f);
        float zoom = ctx.timeline_view_zoom;
        if (ImGui::DragFloat("Zoom##tlzoom", &zoom, 0.5f, 1.f, 200.f, "%.0f px/fr",
                             ImGuiSliderFlags_AlwaysClamp))
            ctx.timeline_view_zoom = zoom;

        ImGui::SameLine(0, 10);

        // Determine the target track for Add Event (selected, or first available)
        int add_event_track_id = ctx.timeline_selected_track_id;
        if (add_event_track_id < 0 && !tl.tracks.empty())
            add_event_track_id = tl.tracks[0].id;

        // Add event button — opens popup at cursor position
        if (ImGui::Button("+ Add Event")) {
            ImGui::OpenPopup("##add_event");
        }
        draw_add_event_popup(tl.id, add_event_track_id,
                             ctx.timeline_cursor_frame, tl.fps, cbs);

        ImGui::SameLine(0, 10);
        ImGui::TextDisabled("Cursor: %s",
            tc_to_string(ctx.timeline_cursor_frame, tl.fps).c_str());
    }

    // Source status border color
    {
        ImU32 border_col = ImGui::ColorConvertFloat4ToU32(theme::kBorder);
        if (tl.source_status == TimecodeSourceStatus::Active)
            border_col = ImGui::ColorConvertFloat4ToU32(theme::kGreen);
        else if (tl.source_status == TimecodeSourceStatus::Present)
            border_col = ImGui::ColorConvertFloat4ToU32(theme::kWarm);

        ImVec2 wmin = ImGui::GetWindowPos();
        ImVec2 wmax = { wmin.x + ww,
                        wmin.y + ImGui::GetWindowSize().y };
        dl->AddRect(wmin, wmax, border_col, 0.f, 0, 2.f);
    }

    (void)state;
    ImGui::End();
}

} // namespace idhmfis
